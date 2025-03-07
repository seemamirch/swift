//===--- Serialization.cpp - Read and write Swift modules -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "Serialization.h"
#include "SILFormat.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/Expr.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/RawComment.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Timer.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Strings.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitstreamWriter.h"
#include "llvm/Bitcode/RecordLayout.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/OnDiskHashTable.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"

#include <vector>

using namespace swift;
using namespace swift::serialization;
using namespace llvm::support;
using swift::version::Version;
using llvm::BCBlockRAII;

/// Used for static_assert.
static constexpr bool declIDFitsIn32Bits() {
  using Int32Info = std::numeric_limits<uint32_t>;
  using PtrIntInfo = std::numeric_limits<uintptr_t>;
  using DeclIDTraits = llvm::PointerLikeTypeTraits<DeclID>;
  return PtrIntInfo::digits - DeclIDTraits::NumLowBitsAvailable <= Int32Info::digits;
}

/// Used for static_assert.
static constexpr bool bitOffsetFitsIn32Bits() {
  // FIXME: Considering BitOffset is a _bit_ offset, and we're storing it in 31
  // bits of a PointerEmbeddedInt, the maximum offset inside a modulefile we can
  // handle happens at 2**28 _bytes_, which is only 268MB. Considering
  // Swift.swiftmodule is itself 25MB, it seems entirely possible users will
  // exceed this limit.
  using Int32Info = std::numeric_limits<uint32_t>;
  using PtrIntInfo = std::numeric_limits<uintptr_t>;
  using BitOffsetTraits = llvm::PointerLikeTypeTraits<BitOffset>;
  return PtrIntInfo::digits - BitOffsetTraits::NumLowBitsAvailable <= Int32Info::digits;
}

namespace {
  /// Used to serialize the on-disk decl hash table.
  class DeclTableInfo {
  public:
    using key_type = DeclBaseName;
    using key_type_ref = key_type;
    using data_type = Serializer::DeclTableData;
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      switch (key.getKind()) {
        case DeclBaseName::Kind::Normal:
          assert(!key.empty());
          // FIXME: DJB seed=0, audit whether the default seed could be used.
          return llvm::djbHash(key.getIdentifier().str(), 0);
        case DeclBaseName::Kind::Subscript:
          return static_cast<uint8_t>(DeclNameKind::Subscript);
        case DeclBaseName::Kind::Constructor:
          return static_cast<uint8_t>(DeclNameKind::Constructor);
        case DeclBaseName::Kind::Destructor:
          return static_cast<uint8_t>(DeclNameKind::Destructor);
      }
      llvm_unreachable("unhandled kind");
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      uint32_t keyLength = sizeof(uint8_t); // For the flag of the name's kind
      if (key.getKind() == DeclBaseName::Kind::Normal) {
        keyLength += key.getIdentifier().str().size(); // The name's length
      }
      assert(keyLength == static_cast<uint16_t>(keyLength));

      uint32_t dataLength = (sizeof(uint32_t) + 1) * data.size();
      assert(dataLength == static_cast<uint16_t>(dataLength));

      endian::Writer writer(out, little);
      writer.write<uint16_t>(keyLength);
      writer.write<uint16_t>(dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      endian::Writer writer(out, little);
      switch (key.getKind()) {
      case DeclBaseName::Kind::Normal:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Normal));
        writer.OS << key.getIdentifier().str();
        break;
      case DeclBaseName::Kind::Subscript:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Subscript));
        break;
      case DeclBaseName::Kind::Constructor:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Constructor));
        break;
      case DeclBaseName::Kind::Destructor:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Destructor));
        break;
      }
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      endian::Writer writer(out, little);
      for (auto entry : data) {
        writer.write<uint8_t>(entry.first);
        writer.write<uint32_t>(entry.second);
      }
    }
  };

  class ExtensionTableInfo {
    serialization::Serializer &Serializer;
    llvm::SmallDenseMap<const NominalTypeDecl *,std::string,4> MangledNameCache;

  public:
    explicit ExtensionTableInfo(serialization::Serializer &serializer)
        : Serializer(serializer) {}

    using key_type = Identifier;
    using key_type_ref = key_type;
    using data_type = Serializer::ExtensionTableData;
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      assert(!key.empty());
      // FIXME: DJB seed=0, audit whether the default seed could be used.
      return llvm::djbHash(key.str(), 0);
    }

    int32_t getNameDataForBase(const NominalTypeDecl *nominal,
                               StringRef *dataToWrite = nullptr) {
      if (nominal->getDeclContext()->isModuleScopeContext())
        return -Serializer.addContainingModuleRef(nominal->getDeclContext());

      auto &mangledName = MangledNameCache[nominal];
      if (mangledName.empty())
        mangledName = Mangle::ASTMangler().mangleNominalType(nominal);

      assert(llvm::isUInt<31>(mangledName.size()));
      if (dataToWrite)
        *dataToWrite = mangledName;
      return mangledName.size();
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      uint32_t keyLength = key.str().size();
      assert(keyLength == static_cast<uint16_t>(keyLength));
      uint32_t dataLength = (sizeof(uint32_t) * 2) * data.size();
      for (auto dataPair : data) {
        int32_t nameData = getNameDataForBase(dataPair.first);
        if (nameData > 0)
          dataLength += nameData;
      }
      assert(dataLength == static_cast<uint16_t>(dataLength));
      endian::Writer writer(out, little);
      writer.write<uint16_t>(keyLength);
      writer.write<uint16_t>(dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      out << key.str();
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      endian::Writer writer(out, little);
      for (auto entry : data) {
        StringRef dataToWrite;
        writer.write<uint32_t>(entry.second);
        writer.write<int32_t>(getNameDataForBase(entry.first, &dataToWrite));
        out << dataToWrite;
      }
    }
  };

  class LocalDeclTableInfo {
  public:
    using key_type = std::string;
    using key_type_ref = StringRef;
    using data_type = DeclID;
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      assert(!key.empty());
      // FIXME: DJB seed=0, audit whether the default seed could be used.
      return llvm::djbHash(key, 0);
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      uint32_t keyLength = key.size();
      assert(keyLength == static_cast<uint16_t>(keyLength));
      uint32_t dataLength = sizeof(uint32_t);
      endian::Writer writer(out, little);
      writer.write<uint16_t>(keyLength);
      // No need to write the data length; it's constant.
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      out << key;
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      endian::Writer writer(out, little);
      writer.write<uint32_t>(data);
    }
  };

  using LocalTypeHashTableGenerator =
    llvm::OnDiskChainedHashTableGenerator<LocalDeclTableInfo>;

  class NestedTypeDeclsTableInfo {
  public:
    using key_type = Identifier;
    using key_type_ref = const key_type &;
    using data_type = Serializer::NestedTypeDeclsData; // (parent, child) pairs
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      assert(!key.empty());
      // FIXME: DJB seed=0, audit whether the default seed could be used.
      return llvm::djbHash(key.str(), 0);
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      uint32_t keyLength = key.str().size();
      assert(keyLength == static_cast<uint16_t>(keyLength));
      uint32_t dataLength = (sizeof(uint32_t) * 2) * data.size();
      assert(dataLength == static_cast<uint16_t>(dataLength));
      endian::Writer writer(out, little);
      writer.write<uint16_t>(keyLength);
      writer.write<uint16_t>(dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      // FIXME: Avoid writing string data for identifiers here.
      out << key.str();
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      endian::Writer writer(out, little);
      for (auto entry : data) {
        writer.write<uint32_t>(entry.first);
        writer.write<uint32_t>(entry.second);
      }
    }
  };

  class DeclMemberNamesTableInfo {
  public:
    using key_type = DeclBaseName;
    using key_type_ref = const key_type &;
    using data_type = BitOffset; // Offsets to sub-tables
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      switch (key.getKind()) {
      case DeclBaseName::Kind::Normal:
        assert(!key.empty());
        // FIXME: DJB seed=0, audit whether the default seed could be used.
        return llvm::djbHash(key.getIdentifier().str(), 0);
      case DeclBaseName::Kind::Subscript:
        return static_cast<uint8_t>(DeclNameKind::Subscript);
      case DeclBaseName::Kind::Constructor:
        return static_cast<uint8_t>(DeclNameKind::Constructor);
      case DeclBaseName::Kind::Destructor:
        return static_cast<uint8_t>(DeclNameKind::Destructor);
      }
      llvm_unreachable("unhandled kind");
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      uint32_t keyLength = sizeof(uint8_t); // For the flag of the name's kind
      if (key.getKind() == DeclBaseName::Kind::Normal) {
        keyLength += key.getIdentifier().str().size(); // The name's length
      }
      assert(keyLength == static_cast<uint16_t>(keyLength));
      uint32_t dataLength = sizeof(uint32_t);
      endian::Writer writer(out, little);
      writer.write<uint16_t>(keyLength);
      // No need to write dataLength, it's constant.
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      endian::Writer writer(out, little);
      switch (key.getKind()) {
      case DeclBaseName::Kind::Normal:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Normal));
        writer.OS << key.getIdentifier().str();
        break;
      case DeclBaseName::Kind::Subscript:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Subscript));
        break;
      case DeclBaseName::Kind::Constructor:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Constructor));
        break;
      case DeclBaseName::Kind::Destructor:
        writer.write<uint8_t>(static_cast<uint8_t>(DeclNameKind::Destructor));
        break;
      }
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(bitOffsetFitsIn32Bits(), "BitOffset too large");
      endian::Writer writer(out, little);
      writer.write<uint32_t>(static_cast<uint32_t>(data));
    }
  };

  class DeclMembersTableInfo {
  public:
    using key_type = DeclID;
    using key_type_ref = const key_type &;
    using data_type = Serializer::DeclMembersData; // Vector of DeclIDs
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      return llvm::hash_value(static_cast<uint32_t>(key));
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      // This will trap if a single ValueDecl has more than 16383 members
      // with the same DeclBaseName. Seems highly unlikely.
      assert((data.size() < (1 << 14)) && "Too many members");
      uint32_t dataLength = sizeof(uint32_t) * data.size(); // value DeclIDs
      endian::Writer writer(out, little);
      // No need to write the key length; it's constant.
      writer.write<uint16_t>(dataLength);
      return { sizeof(uint32_t), dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      assert(len == sizeof(uint32_t));
      endian::Writer writer(out, little);
      writer.write<uint32_t>(key);
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      endian::Writer writer(out, little);
      for (auto entry : data) {
        writer.write<uint32_t>(entry);
      }
    }
  };
} // end anonymous namespace

namespace llvm {
  template<> struct DenseMapInfo<Serializer::DeclTypeUnion> {
    using DeclTypeUnion = Serializer::DeclTypeUnion;
    static inline DeclTypeUnion getEmptyKey() { return nullptr; }
    static inline DeclTypeUnion getTombstoneKey() { return swift::Type(); }
    static unsigned getHashValue(const DeclTypeUnion &val) {
      return DenseMapInfo<const void *>::getHashValue(val.getOpaqueValue());
    }
    static bool isEqual(const DeclTypeUnion &lhs, const DeclTypeUnion &rhs) {
      return lhs == rhs;
    }
  };
} // namespace llvm

static ModuleDecl *getModule(ModuleOrSourceFile DC) {
  if (auto M = DC.dyn_cast<ModuleDecl *>())
    return M;
  return DC.get<SourceFile *>()->getParentModule();
}

static ASTContext &getContext(ModuleOrSourceFile DC) {
  return getModule(DC)->getASTContext();
}

static bool shouldSerializeAsLocalContext(const DeclContext *DC) {
  return DC->isLocalContext() && !isa<AbstractFunctionDecl>(DC) &&
        !isa<SubscriptDecl>(DC);
}

static const Decl *getDeclForContext(const DeclContext *DC) {
  switch (DC->getContextKind()) {
  case DeclContextKind::Module:
    // Use a null decl to represent the module.
    return nullptr;
  case DeclContextKind::FileUnit:
    return getDeclForContext(DC->getParent());
  case DeclContextKind::SerializedLocal:
    llvm_unreachable("Serialized local contexts should only come from deserialization");
  case DeclContextKind::Initializer:
  case DeclContextKind::AbstractClosureExpr:
    // FIXME: What about default functions?
    llvm_unreachable("shouldn't serialize decls from anonymous closures");
  case DeclContextKind::GenericTypeDecl:
    return cast<GenericTypeDecl>(DC);
  case DeclContextKind::ExtensionDecl:
    return cast<ExtensionDecl>(DC);
  case DeclContextKind::TopLevelCodeDecl:
    llvm_unreachable("shouldn't serialize the main module");
  case DeclContextKind::AbstractFunctionDecl:
    return cast<AbstractFunctionDecl>(DC);
  case DeclContextKind::SubscriptDecl:
    return cast<SubscriptDecl>(DC);
  case DeclContextKind::EnumElementDecl:
    return cast<EnumElementDecl>(DC);
  }

  llvm_unreachable("Unhandled DeclContextKind in switch.");
}

namespace {
  struct Accessors {
    uint8_t OpaqueReadOwnership;
    uint8_t ReadImpl, WriteImpl, ReadWriteImpl;
    SmallVector<AccessorDecl *, 8> Decls;
  };
} // end anonymous namespace

static uint8_t getRawOpaqueReadOwnership(swift::OpaqueReadOwnership ownership) {
  switch (ownership) {
#define CASE(KIND)                                            \
  case swift::OpaqueReadOwnership::KIND:                      \
    return uint8_t(serialization::OpaqueReadOwnership::KIND);
  CASE(Owned)
  CASE(Borrowed)
  CASE(OwnedOrBorrowed)
#undef CASE
  }
  llvm_unreachable("bad kind");  
}

static uint8_t getRawReadImplKind(swift::ReadImplKind kind) {
  switch (kind) {
#define CASE(KIND)                                     \
  case swift::ReadImplKind::KIND:                      \
    return uint8_t(serialization::ReadImplKind::KIND);
  CASE(Stored)
  CASE(Get)
  CASE(Inherited)
  CASE(Address)
  CASE(Read)
#undef CASE
  }
  llvm_unreachable("bad kind");
}

static unsigned getRawWriteImplKind(swift::WriteImplKind kind) {
  switch (kind) {
#define CASE(KIND)                                      \
  case swift::WriteImplKind::KIND:                      \
    return uint8_t(serialization::WriteImplKind::KIND);
  CASE(Immutable)
  CASE(Stored)
  CASE(Set)
  CASE(StoredWithObservers)
  CASE(InheritedWithObservers)
  CASE(MutableAddress)
  CASE(Modify)
#undef CASE
  }
  llvm_unreachable("bad kind");
}

static unsigned getRawReadWriteImplKind(swift::ReadWriteImplKind kind) {
  switch (kind) {
#define CASE(KIND)                                          \
  case swift::ReadWriteImplKind::KIND:                      \
    return uint8_t(serialization::ReadWriteImplKind::KIND);
  CASE(Immutable)
  CASE(Stored)
  CASE(MutableAddress)
  CASE(MaterializeToTemporary)
  CASE(Modify)
#undef CASE
  }
  llvm_unreachable("bad kind");
}

static Accessors getAccessors(const AbstractStorageDecl *storage) {
  Accessors accessors;
  accessors.OpaqueReadOwnership =
    getRawOpaqueReadOwnership(storage->getOpaqueReadOwnership());
  auto impl = storage->getImplInfo();
  accessors.ReadImpl = getRawReadImplKind(impl.getReadImpl());
  accessors.WriteImpl = getRawWriteImplKind(impl.getWriteImpl());
  accessors.ReadWriteImpl = getRawReadWriteImplKind(impl.getReadWriteImpl());
  auto decls = storage->getAllAccessors();
  accessors.Decls.append(decls.begin(), decls.end());
  return accessors;
}

DeclID Serializer::addLocalDeclContextRef(const DeclContext *DC) {
  assert(DC->isLocalContext() && "Expected a local DeclContext");
  auto &id = LocalDeclContextIDs[DC];
  if (id != 0)
    return id;

  id = ++LastLocalDeclContextID;
  LocalDeclContextsToWrite.push(DC);
  return id;
}

GenericSignatureID Serializer::addGenericSignatureRef(
                                                const GenericSignature *env) {
  if (!env) return 0;

  auto &id = GenericSignatureIDs[env];
  if (id != 0)
    return id;

  id = ++LastGenericSignatureID;
  GenericSignaturesToWrite.push(env);
  return id;
}

GenericEnvironmentID Serializer::addGenericEnvironmentRef(
                                                const GenericEnvironment *env) {
  if (!env) return 0;

  auto &id = GenericEnvironmentIDs[env];
  if (id != 0)
    return id;

  id = ++LastGenericEnvironmentID;
  GenericEnvironmentsToWrite.push(env);
  return id;
}

SubstitutionMapID Serializer::addSubstitutionMapRef(
                                              SubstitutionMap substitutions) {
  if (!substitutions) return 0;

  auto &id = SubstitutionMapIDs[substitutions];
  if (id != 0)
    return id;

  id = ++LastSubstitutionMapID;
  SubstitutionMapsToWrite.push(substitutions);
  return id;
}

DeclContextID Serializer::addDeclContextRef(const DeclContext *DC) {
  switch (DC->getContextKind()) {
  case DeclContextKind::Module:
  case DeclContextKind::FileUnit: // Skip up to the module
    return 0;
  default:
    break;
  }

  // If this decl context is a plain old serializable decl, queue it up for
  // normal serialization.
  if (shouldSerializeAsLocalContext(DC))
    addLocalDeclContextRef(DC);
  else
    addDeclRef(getDeclForContext(DC));

  auto &id = DeclContextIDs[DC];
  if (id)
    return id;

  id = ++LastDeclContextID;
  DeclContextsToWrite.push(DC);

  return id;
}

DeclID Serializer::addDeclRef(const Decl *D, bool allowTypeAliasXRef) {
  if (!D)
    return 0;

  DeclID &id = DeclAndTypeIDs[D];
  if (id != 0)
    return id;

  assert((!isDeclXRef(D) || isa<ValueDecl>(D) || isa<OperatorDecl>(D) ||
          isa<PrecedenceGroupDecl>(D)) &&
         "cannot cross-reference this decl");

  assert((!isDeclXRef(D) ||
          !D->getAttrs().hasAttribute<ForbidSerializingReferenceAttr>()) &&
         "cannot cross-reference this decl");

  assert((allowTypeAliasXRef || !isa<TypeAliasDecl>(D) ||
          D->getModuleContext() == M) &&
         "cannot cross-reference typealiases directly (use the TypeAliasType)");

  id = ++LastDeclID;
  DeclsAndTypesToWrite.push(D);
  return id;
}

serialization::TypeID Serializer::addTypeRef(Type ty) {
  if (!ty)
    return 0;

#ifndef NDEBUG
  PrettyStackTraceType trace(M->getASTContext(), "serializing", ty);
  assert(!ty->hasError() && "Serializing error type");
#endif

  auto &id = DeclAndTypeIDs[ty];
  if (id != 0)
    return id;

  id = ++LastTypeID;
  DeclsAndTypesToWrite.push(ty);
  return id;
}

IdentifierID Serializer::addDeclBaseNameRef(DeclBaseName ident) {
  switch (ident.getKind()) {
  case DeclBaseName::Kind::Normal: {
    if (ident.empty())
      return 0;

    IdentifierID &id = IdentifierIDs[ident.getIdentifier()];
    if (id != 0)
      return id;

    id = ++LastUniquedStringID;
    StringsToWrite.push_back(ident.getIdentifier().str());
    return id;
  }
  case DeclBaseName::Kind::Subscript:
    return SUBSCRIPT_ID;
  case DeclBaseName::Kind::Constructor:
    return CONSTRUCTOR_ID;
  case DeclBaseName::Kind::Destructor:
    return DESTRUCTOR_ID;
  }
  llvm_unreachable("unhandled kind");
}

std::pair<StringRef, IdentifierID> Serializer::addUniquedString(StringRef str) {
  if (str.empty())
    return {str, 0};

  decltype(UniquedStringIDs)::iterator iter;
  bool isNew;
  std::tie(iter, isNew) =
      UniquedStringIDs.insert({str, LastUniquedStringID + 1});

  if (!isNew)
    return {iter->getKey(), iter->getValue()};

  ++LastUniquedStringID;
  // Note that we use the string data stored in the StringMap.
  StringsToWrite.push_back(iter->getKey());
  return {iter->getKey(), LastUniquedStringID};
}

IdentifierID Serializer::addFilename(StringRef filename) {
  assert(!filename.empty() && "Attemping to add an empty filename");

  return addUniquedString(filename).second;
}

IdentifierID Serializer::addContainingModuleRef(const DeclContext *DC) {
  assert(!isa<ModuleDecl>(DC) &&
         "References should be to things within modules");
  const FileUnit *file = cast<FileUnit>(DC->getModuleScopeContext());
  const ModuleDecl *M = file->getParentModule();

  if (M == this->M)
    return CURRENT_MODULE_ID;
  if (M == this->M->getASTContext().TheBuiltinModule)
    return BUILTIN_MODULE_ID;

  auto clangImporter =
    static_cast<ClangImporter *>(
      this->M->getASTContext().getClangModuleLoader());
  if (M == clangImporter->getImportedHeaderModule())
    return OBJC_HEADER_MODULE_ID;

  auto exportedModuleName = file->getExportedModuleName();
  assert(!exportedModuleName.empty());
  auto exportedModuleID = M->getASTContext().getIdentifier(exportedModuleName);
  return addDeclBaseNameRef(exportedModuleID);
}

SILLayoutID Serializer::addSILLayoutRef(SILLayout *layout) {
  auto &id = SILLayouts[layout];
  if (id != 0)
    return id;
  
  id = ++LastSILLayoutID;
  SILLayoutsToWrite.push(layout);
  return id;
}

NormalConformanceID Serializer::addConformanceRef(
                      const NormalProtocolConformance *conformance) {
  assert(conformance->getDeclContext()->getParentModule() == M &&
         "cannot reference conformance from another module");
  auto &conformanceID = NormalConformances[conformance];
  if (conformanceID)
    return conformanceID;

  conformanceID = ++LastNormalConformanceID;
  NormalConformancesToWrite.push(conformance);

  return conformanceID;
}

/// Record the name of a block.
void SerializerBase::emitBlockID(unsigned ID, StringRef name,
                                 SmallVectorImpl<unsigned char> &nameBuffer) {
  SmallVector<unsigned, 1> idBuffer;
  idBuffer.push_back(ID);
  Out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETBID, idBuffer);

  // Emit the block name if present.
  if (name.empty())
    return;
  nameBuffer.resize(name.size());
  memcpy(nameBuffer.data(), name.data(), name.size());
  Out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_BLOCKNAME, nameBuffer);
}

void SerializerBase::emitRecordID(unsigned ID, StringRef name,
                                  SmallVectorImpl<unsigned char> &nameBuffer) {
  assert(ID < 256 && "can't fit record ID in next to name");
  nameBuffer.resize(name.size()+1);
  nameBuffer[0] = ID;
  memcpy(nameBuffer.data()+1, name.data(), name.size());
  Out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME, nameBuffer);
}

void Serializer::writeBlockInfoBlock() {
  BCBlockRAII restoreBlock(Out, llvm::bitc::BLOCKINFO_BLOCK_ID, 2);

  SmallVector<unsigned char, 64> nameBuffer;
#define BLOCK(X) emitBlockID(X ## _ID, #X, nameBuffer)
#define BLOCK_RECORD(K, X) emitRecordID(K::X, #X, nameBuffer)

  BLOCK(MODULE_BLOCK);

  BLOCK(CONTROL_BLOCK);
  BLOCK_RECORD(control_block, METADATA);
  BLOCK_RECORD(control_block, MODULE_NAME);
  BLOCK_RECORD(control_block, TARGET);

  BLOCK(OPTIONS_BLOCK);
  BLOCK_RECORD(options_block, SDK_PATH);
  BLOCK_RECORD(options_block, XCC);
  BLOCK_RECORD(options_block, IS_SIB);
  BLOCK_RECORD(options_block, IS_TESTABLE);
  BLOCK_RECORD(options_block, ARE_PRIVATE_IMPORTS_ENABLED);
  BLOCK_RECORD(options_block, RESILIENCE_STRATEGY);

  BLOCK(INPUT_BLOCK);
  BLOCK_RECORD(input_block, IMPORTED_MODULE);
  BLOCK_RECORD(input_block, LINK_LIBRARY);
  BLOCK_RECORD(input_block, IMPORTED_HEADER);
  BLOCK_RECORD(input_block, IMPORTED_HEADER_CONTENTS);
  BLOCK_RECORD(input_block, MODULE_FLAGS);
  BLOCK_RECORD(input_block, SEARCH_PATH);
  BLOCK_RECORD(input_block, FILE_DEPENDENCY);
  BLOCK_RECORD(input_block, DEPENDENCY_DIRECTORY);
  BLOCK_RECORD(input_block, PARSEABLE_INTERFACE_PATH);

  BLOCK(DECLS_AND_TYPES_BLOCK);
#define RECORD(X) BLOCK_RECORD(decls_block, X);
#include "swift/Serialization/DeclTypeRecordNodes.def"

  BLOCK(IDENTIFIER_DATA_BLOCK);
  BLOCK_RECORD(identifier_block, IDENTIFIER_DATA);

  BLOCK(INDEX_BLOCK);
  BLOCK_RECORD(index_block, TYPE_OFFSETS);
  BLOCK_RECORD(index_block, DECL_OFFSETS);
  BLOCK_RECORD(index_block, IDENTIFIER_OFFSETS);
  BLOCK_RECORD(index_block, TOP_LEVEL_DECLS);
  BLOCK_RECORD(index_block, OPERATORS);
  BLOCK_RECORD(index_block, EXTENSIONS);
  BLOCK_RECORD(index_block, CLASS_MEMBERS_FOR_DYNAMIC_LOOKUP);
  BLOCK_RECORD(index_block, OPERATOR_METHODS);
  BLOCK_RECORD(index_block, OBJC_METHODS);
  BLOCK_RECORD(index_block, ENTRY_POINT);
  BLOCK_RECORD(index_block, LOCAL_DECL_CONTEXT_OFFSETS);
  BLOCK_RECORD(index_block, GENERIC_SIGNATURE_OFFSETS);
  BLOCK_RECORD(index_block, GENERIC_ENVIRONMENT_OFFSETS);
  BLOCK_RECORD(index_block, SUBSTITUTION_MAP_OFFSETS);
  BLOCK_RECORD(index_block, DECL_CONTEXT_OFFSETS);
  BLOCK_RECORD(index_block, LOCAL_TYPE_DECLS);
  BLOCK_RECORD(index_block, NORMAL_CONFORMANCE_OFFSETS);
  BLOCK_RECORD(index_block, SIL_LAYOUT_OFFSETS);
  BLOCK_RECORD(index_block, PRECEDENCE_GROUPS);
  BLOCK_RECORD(index_block, NESTED_TYPE_DECLS);
  BLOCK_RECORD(index_block, DECL_MEMBER_NAMES);
  BLOCK_RECORD(index_block, ORDERED_TOP_LEVEL_DECLS);

  BLOCK(DECL_MEMBER_TABLES_BLOCK);
  BLOCK_RECORD(decl_member_tables_block, DECL_MEMBERS);

  BLOCK(SIL_BLOCK);
  BLOCK_RECORD(sil_block, SIL_FUNCTION);
  BLOCK_RECORD(sil_block, SIL_BASIC_BLOCK);
  BLOCK_RECORD(sil_block, SIL_ONE_VALUE_ONE_OPERAND);
  BLOCK_RECORD(sil_block, SIL_ONE_TYPE);
  BLOCK_RECORD(sil_block, SIL_ONE_OPERAND);
  BLOCK_RECORD(sil_block, SIL_ONE_TYPE_ONE_OPERAND);
  BLOCK_RECORD(sil_block, SIL_ONE_TYPE_VALUES);
  BLOCK_RECORD(sil_block, SIL_TWO_OPERANDS);
  BLOCK_RECORD(sil_block, SIL_TAIL_ADDR);
  BLOCK_RECORD(sil_block, SIL_INST_APPLY);
  BLOCK_RECORD(sil_block, SIL_INST_NO_OPERAND);
  BLOCK_RECORD(sil_block, SIL_VTABLE);
  BLOCK_RECORD(sil_block, SIL_VTABLE_ENTRY);
  BLOCK_RECORD(sil_block, SIL_GLOBALVAR);
  BLOCK_RECORD(sil_block, SIL_INST_CAST);
  BLOCK_RECORD(sil_block, SIL_INIT_EXISTENTIAL);
  BLOCK_RECORD(sil_block, SIL_WITNESS_TABLE);
  BLOCK_RECORD(sil_block, SIL_WITNESS_METHOD_ENTRY);
  BLOCK_RECORD(sil_block, SIL_WITNESS_BASE_ENTRY);
  BLOCK_RECORD(sil_block, SIL_WITNESS_ASSOC_PROTOCOL);
  BLOCK_RECORD(sil_block, SIL_WITNESS_ASSOC_ENTRY);
  BLOCK_RECORD(sil_block, SIL_WITNESS_CONDITIONAL_CONFORMANCE);
  BLOCK_RECORD(sil_block, SIL_DEFAULT_WITNESS_TABLE);
  BLOCK_RECORD(sil_block, SIL_DEFAULT_WITNESS_TABLE_NO_ENTRY);
  BLOCK_RECORD(sil_block, SIL_INST_WITNESS_METHOD);
  BLOCK_RECORD(sil_block, SIL_SPECIALIZE_ATTR);
  BLOCK_RECORD(sil_block, SIL_ONE_OPERAND_EXTRA_ATTR);
  BLOCK_RECORD(sil_block, SIL_TWO_OPERANDS_EXTRA_ATTR);

  // These layouts can exist in both decl blocks and sil blocks.
#define BLOCK_RECORD_WITH_NAMESPACE(K, X) emitRecordID(X, #X, nameBuffer)
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::INVALID_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::ABSTRACT_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::NORMAL_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::SELF_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::SPECIALIZED_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::INHERITED_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::NORMAL_PROTOCOL_CONFORMANCE_ID);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::PROTOCOL_CONFORMANCE_XREF);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::GENERIC_PARAM_LIST);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::GENERIC_REQUIREMENT);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::LAYOUT_REQUIREMENT);

  BLOCK(SIL_INDEX_BLOCK);
  BLOCK_RECORD(sil_index_block, SIL_FUNC_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_FUNC_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_VTABLE_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_VTABLE_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_GLOBALVAR_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_GLOBALVAR_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_WITNESS_TABLE_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_WITNESS_TABLE_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_DEFAULT_WITNESS_TABLE_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_DEFAULT_WITNESS_TABLE_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_PROPERTY_OFFSETS);

#undef BLOCK
#undef BLOCK_RECORD
}

void Serializer::writeHeader(const SerializationOptions &options) {
  {
    BCBlockRAII restoreBlock(Out, CONTROL_BLOCK_ID, 3);
    control_block::ModuleNameLayout ModuleName(Out);
    control_block::MetadataLayout Metadata(Out);
    control_block::TargetLayout Target(Out);

    ModuleName.emit(ScratchRecord, M->getName().str());

    SmallString<32> versionStringBuf;
    llvm::raw_svector_ostream versionString(versionStringBuf);
    versionString << Version::getCurrentLanguageVersion();
    size_t shortVersionStringLength = versionString.tell();
    versionString << '('
                  << M->getASTContext().LangOpts.EffectiveLanguageVersion;
    size_t compatibilityVersionStringLength =
        versionString.tell() - shortVersionStringLength - 1;
    versionString << ")/" << version::getSwiftFullVersion();
    Metadata.emit(ScratchRecord,
                  SWIFTMODULE_VERSION_MAJOR, SWIFTMODULE_VERSION_MINOR,
                  shortVersionStringLength,
                  compatibilityVersionStringLength,
                  versionString.str());

    Target.emit(ScratchRecord, M->getASTContext().LangOpts.Target.str());

    {
      llvm::BCBlockRAII restoreBlock(Out, OPTIONS_BLOCK_ID, 4);

      options_block::IsSIBLayout IsSIB(Out);
      IsSIB.emit(ScratchRecord, options.IsSIB);

      if (M->isTestingEnabled()) {
        options_block::IsTestableLayout IsTestable(Out);
        IsTestable.emit(ScratchRecord);
      }

      if (M->arePrivateImportsEnabled()) {
        options_block::ArePrivateImportsEnabledLayout PrivateImports(Out);
        PrivateImports.emit(ScratchRecord);
      }

      if (M->getResilienceStrategy() != ResilienceStrategy::Default) {
        options_block::ResilienceStrategyLayout Strategy(Out);
        Strategy.emit(ScratchRecord, unsigned(M->getResilienceStrategy()));
      }

      if (options.SerializeOptionsForDebugging) {
        options_block::SDKPathLayout SDKPath(Out);
        options_block::XCCLayout XCC(Out);

        SDKPath.emit(ScratchRecord, M->getASTContext().SearchPathOpts.SDKPath);
        auto &Opts = options.ExtraClangOptions;
        for (auto Arg = Opts.begin(), E = Opts.end(); Arg != E; ++Arg) { 
          // FIXME: This is a hack and calls for a better design.
          //
          // Filter out any -ivfsoverlay options that include an
          // unextended-module-overlay.yaml overlay. By convention the Xcode
          // buildsystem uses these while *building* mixed Objective-C and Swift
          // frameworks; but they should never be used to *import* the module
          // defined in the framework.
          if (StringRef(*Arg).startswith("-ivfsoverlay")) {
            auto Next = std::next(Arg);
            if (Next != E &&
                StringRef(*Next).endswith("unextended-module-overlay.yaml")) {
              ++Arg;
              continue;
            }
          }
          XCC.emit(ScratchRecord, *Arg);
        }
      }
    }
  }
}

using ImportPathBlob = llvm::SmallString<64>;
static void flattenImportPath(const ModuleDecl::ImportedModule &import,
                              ImportPathBlob &out) {
  SmallVector<StringRef, 4> reverseSubmoduleNames(
      import.second->getReverseFullModuleName(), {});

  interleave(reverseSubmoduleNames.rbegin(), reverseSubmoduleNames.rend(),
             [&out](StringRef next) { out.append(next); },
             [&out] { out.push_back('\0'); });

  if (import.first.empty())
    return;

  out.push_back('\0');
  assert(import.first.size() == 1 && "can only handle top-level decl imports");
  auto accessPathElem = import.first.front();
  out.append(accessPathElem.first.str());
}

uint64_t getRawModTimeOrHash(const SerializationOptions::FileDependency &dep) {
  if (dep.isHashBased()) return dep.getContentHash();
  return dep.getModificationTime();
}

using ImportSet = llvm::SmallSet<ModuleDecl::ImportedModule, 8,
                                 ModuleDecl::OrderImportedModules>;
static ImportSet getImportsAsSet(const ModuleDecl *M,
                                 ModuleDecl::ImportFilter filter) {
  SmallVector<ModuleDecl::ImportedModule, 8> imports;
  M->getImportedModules(imports, filter);
  ImportSet importSet;
  importSet.insert(imports.begin(), imports.end());
  return importSet;
}

void Serializer::writeInputBlock(const SerializationOptions &options) {
  BCBlockRAII restoreBlock(Out, INPUT_BLOCK_ID, 4);
  input_block::ImportedModuleLayout ImportedModule(Out);
  input_block::LinkLibraryLayout LinkLibrary(Out);
  input_block::ImportedHeaderLayout ImportedHeader(Out);
  input_block::ImportedHeaderContentsLayout ImportedHeaderContents(Out);
  input_block::SearchPathLayout SearchPath(Out);
  input_block::FileDependencyLayout FileDependency(Out);
  input_block::DependencyDirectoryLayout DependencyDirectory(Out);
  input_block::ParseableInterfaceLayout ParseableInterface(Out);

  if (options.SerializeOptionsForDebugging) {
    const SearchPathOptions &searchPathOpts = M->getASTContext().SearchPathOpts;
    // Put the framework search paths first so that they'll be preferred upon
    // deserialization.
    for (auto &framepath : searchPathOpts.FrameworkSearchPaths)
      SearchPath.emit(ScratchRecord, /*framework=*/true, framepath.IsSystem,
                      framepath.Path);
    for (auto &path : searchPathOpts.ImportSearchPaths)
      SearchPath.emit(ScratchRecord, /*framework=*/false, /*system=*/false, path);
  }

  // Note: We're not using StringMap here because we don't need to own the
  // strings.
  llvm::DenseMap<StringRef, unsigned> dependencyDirectories;
  for (auto const &dep : options.Dependencies) {
    StringRef directoryName = llvm::sys::path::parent_path(dep.getPath());
    unsigned &dependencyDirectoryIndex = dependencyDirectories[directoryName];
    if (!dependencyDirectoryIndex) {
      // This name must be newly-added. Give it a new ID (and skip 0).
      dependencyDirectoryIndex = dependencyDirectories.size();
      DependencyDirectory.emit(ScratchRecord, directoryName);
    }
    FileDependency.emit(ScratchRecord,
                        dep.getSize(),
                        getRawModTimeOrHash(dep),
                        dep.isHashBased(),
                        dep.isSDKRelative(),
                        dependencyDirectoryIndex,
                        llvm::sys::path::filename(dep.getPath()));
  }

  if (!options.ParseableInterface.empty())
    ParseableInterface.emit(ScratchRecord, options.ParseableInterface);

  ModuleDecl::ImportFilter allImportFilter;
  allImportFilter |= ModuleDecl::ImportFilterKind::Public;
  allImportFilter |= ModuleDecl::ImportFilterKind::Private;
  allImportFilter |= ModuleDecl::ImportFilterKind::ImplementationOnly;
  SmallVector<ModuleDecl::ImportedModule, 8> allImports;
  M->getImportedModules(allImports, allImportFilter);
  ModuleDecl::removeDuplicateImports(allImports);

  // Collect the public and private imports as a subset so that we can
  // distinguish them.
  ImportSet publicImportSet =
      getImportsAsSet(M, ModuleDecl::ImportFilterKind::Public);
  ImportSet privateImportSet =
      getImportsAsSet(M, ModuleDecl::ImportFilterKind::Private);

  auto clangImporter =
    static_cast<ClangImporter *>(M->getASTContext().getClangModuleLoader());
  ModuleDecl *bridgingHeaderModule = clangImporter->getImportedHeaderModule();
  ModuleDecl::ImportedModule bridgingHeaderImport{{}, bridgingHeaderModule};

  // Make sure the bridging header module is always at the top of the import
  // list, mimicking how it is processed before any module imports when
  // compiling source files.
  if (llvm::is_contained(allImports, bridgingHeaderImport)) {
    off_t importedHeaderSize = 0;
    time_t importedHeaderModTime = 0;
    std::string contents;
    if (!options.ImportedHeader.empty()) {
      contents = clangImporter->getBridgingHeaderContents(
          options.ImportedHeader, importedHeaderSize, importedHeaderModTime);
    }
    assert(publicImportSet.count(bridgingHeaderImport));
    ImportedHeader.emit(ScratchRecord,
                        publicImportSet.count(bridgingHeaderImport),
                        importedHeaderSize, importedHeaderModTime,
                        options.ImportedHeader);
    if (!contents.empty()) {
      contents.push_back('\0');
      ImportedHeaderContents.emit(ScratchRecord, contents);
    }
  }

  ModuleDecl *theBuiltinModule = M->getASTContext().TheBuiltinModule;
  for (auto import : allImports) {
    if (import.second == theBuiltinModule ||
        import.second == bridgingHeaderModule) {
      continue;
    }

    ImportPathBlob importPath;
    flattenImportPath(import, importPath);

    serialization::ImportControl stableImportControl;
    // The order of checks here is important, since a module can be imported
    // differently in different files, and we need to record the "most visible"
    // form here.
    if (publicImportSet.count(import))
      stableImportControl = ImportControl::Exported;
    else if (privateImportSet.count(import))
      stableImportControl = ImportControl::Normal;
    else
      stableImportControl = ImportControl::ImplementationOnly;

    ImportedModule.emit(ScratchRecord,
                        static_cast<uint8_t>(stableImportControl),
                        !import.first.empty(), importPath);
  }

  if (!options.ModuleLinkName.empty()) {
    LinkLibrary.emit(ScratchRecord, serialization::LibraryKind::Library,
                     options.AutolinkForceLoad, options.ModuleLinkName);
  }
}

/// Translate AST default argument kind to the Serialization enum values, which
/// are guaranteed to be stable.
static uint8_t getRawStableDefaultArgumentKind(swift::DefaultArgumentKind kind) {
  switch (kind) {
#define CASE(X) \
  case swift::DefaultArgumentKind::X: \
    return static_cast<uint8_t>(serialization::DefaultArgumentKind::X);
  CASE(None)
  CASE(Normal)
  CASE(Inherited)
  CASE(Column)
  CASE(File)
  CASE(Line)
  CASE(Function)
  CASE(DSOHandle)
  CASE(NilLiteral)
  CASE(EmptyArray)
  CASE(EmptyDictionary)
  CASE(StoredProperty)
#undef CASE
  }

  llvm_unreachable("Unhandled DefaultArgumentKind in switch.");
}

static uint8_t
getRawStableMetatypeRepresentation(const AnyMetatypeType *metatype) {
  if (!metatype->hasRepresentation()) {
    return serialization::MetatypeRepresentation::MR_None;
  }

  switch (metatype->getRepresentation()) {
  case swift::MetatypeRepresentation::Thin:
    return serialization::MetatypeRepresentation::MR_Thin;
  case swift::MetatypeRepresentation::Thick:
    return serialization::MetatypeRepresentation::MR_Thick;
  case swift::MetatypeRepresentation::ObjC:
    return serialization::MetatypeRepresentation::MR_ObjC;
  }
  llvm_unreachable("bad representation");
}

/// Translate from the requirement kind to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableRequirementKind(RequirementKind kind) {
#define CASE(KIND)            \
  case RequirementKind::KIND: \
    return GenericRequirementKind::KIND;

  switch (kind) {
  CASE(Conformance)
  CASE(Superclass)
  CASE(SameType)
  CASE(Layout)
  }
#undef CASE

  llvm_unreachable("Unhandled RequirementKind in switch.");
}

void Serializer::writeGenericRequirements(ArrayRef<Requirement> requirements,
                                          const std::array<unsigned, 256> &abbrCodes) {
  using namespace decls_block;

  if (requirements.empty())
    return;

  auto reqAbbrCode = abbrCodes[GenericRequirementLayout::Code];
  auto layoutReqAbbrCode = abbrCodes[LayoutRequirementLayout::Code];
  for (const auto &req : requirements) {
    if (req.getKind() != RequirementKind::Layout)
      GenericRequirementLayout::emitRecord(
          Out, ScratchRecord, reqAbbrCode,
          getRawStableRequirementKind(req.getKind()),
          addTypeRef(req.getFirstType()), addTypeRef(req.getSecondType()));
    else {
      // Write layout requirement.
      auto layout = req.getLayoutConstraint();
      unsigned size = 0;
      unsigned alignment = 0;
      if (layout->isKnownSizeTrivial()) {
        size = layout->getTrivialSizeInBits();
        alignment = layout->getAlignmentInBits();
      }
      LayoutRequirementKind rawKind = LayoutRequirementKind::UnknownLayout;
      switch (layout->getKind()) {
      case LayoutConstraintKind::NativeRefCountedObject:
        rawKind = LayoutRequirementKind::NativeRefCountedObject;
        break;
      case LayoutConstraintKind::RefCountedObject:
        rawKind = LayoutRequirementKind::RefCountedObject;
        break;
      case LayoutConstraintKind::Trivial:
        rawKind = LayoutRequirementKind::Trivial;
        break;
      case LayoutConstraintKind::TrivialOfExactSize:
        rawKind = LayoutRequirementKind::TrivialOfExactSize;
        break;
      case LayoutConstraintKind::TrivialOfAtMostSize:
        rawKind = LayoutRequirementKind::TrivialOfAtMostSize;
        break;
      case LayoutConstraintKind::Class:
        rawKind = LayoutRequirementKind::Class;
        break;
      case LayoutConstraintKind::NativeClass:
        rawKind = LayoutRequirementKind::NativeClass;
        break;
      case LayoutConstraintKind::UnknownLayout:
        rawKind = LayoutRequirementKind::UnknownLayout;
        break;
      }
      LayoutRequirementLayout::emitRecord(
          Out, ScratchRecord, layoutReqAbbrCode, rawKind,
          addTypeRef(req.getFirstType()), size, alignment);
    }
  }
}

void Serializer::writeGenericSignature(const GenericSignature *sig) {
  using namespace decls_block;

  // Record the offset of this generic environment.
  auto id = GenericSignatureIDs[sig];
  assert(id != 0 && "generic signature not referenced properly");
  (void)id;

  assert((id - 1) == GenericSignatureOffsets.size());
  GenericSignatureOffsets.push_back(Out.GetCurrentBitNo());

  assert(sig != nullptr);

  // Record the generic parameters.
  SmallVector<uint64_t, 4> rawParamIDs;
  for (auto *paramTy : sig->getGenericParams()) {
    rawParamIDs.push_back(addTypeRef(paramTy));
  }

  auto abbrCode = DeclTypeAbbrCodes[GenericSignatureLayout::Code];
  GenericSignatureLayout::emitRecord(Out, ScratchRecord, abbrCode, rawParamIDs);

  writeGenericRequirements(sig->getRequirements(), DeclTypeAbbrCodes);
}

void Serializer::writeGenericEnvironment(const GenericEnvironment *env) {
  using namespace decls_block;

  // Record the offset of this generic environment.
  auto id = GenericEnvironmentIDs[env];
  assert(id != 0 && "generic environment not referenced properly");
  (void)id;

  assert((id - 1) == GenericEnvironmentOffsets.size());
  assert(env != nullptr);

  // Determine whether we must use SIL mode, because one of the generic
  // parameters has a declaration with module context.
  bool SILMode = false;
  for (auto *paramTy : env->getGenericParams()) {
    if (auto *decl = paramTy->getDecl()) {
      if (decl->getDeclContext()->isModuleScopeContext()) {
        SILMode = true;
        break;
      }
    }
  }

  // If not in SIL mode, generic environments just contain a reference to
  // the generic signature.
  if (!SILMode) {
    // Record the generic signature directly.
    auto genericSigID = addGenericSignatureRef(env->getGenericSignature());
    GenericEnvironmentOffsets.push_back((genericSigID << 1) | 0x01);
    return;
  }

  // Record the current bit.
  GenericEnvironmentOffsets.push_back((Out.GetCurrentBitNo() << 1));

  // Record the generic parameters.
  SmallVector<uint64_t, 4> rawParamIDs;
  for (auto *paramTy : env->getGenericParams()) {
    auto *decl = paramTy->getDecl();

    // In SIL mode, add the name and canonicalize the parameter type.
    if (decl)
      rawParamIDs.push_back(addDeclBaseNameRef(decl->getName()));
    else
      rawParamIDs.push_back(addDeclBaseNameRef(Identifier()));

    paramTy = paramTy->getCanonicalType()->castTo<GenericTypeParamType>();
    rawParamIDs.push_back(addTypeRef(paramTy));
  }

  auto envAbbrCode = DeclTypeAbbrCodes[SILGenericEnvironmentLayout::Code];
  SILGenericEnvironmentLayout::emitRecord(Out, ScratchRecord, envAbbrCode,
                                          rawParamIDs);

  writeGenericRequirements(env->getGenericSignature()->getRequirements(),
                           DeclTypeAbbrCodes);
}

void Serializer::writeSubstitutionMap(const SubstitutionMap substitutions) {
  using namespace decls_block;
  assert(substitutions);

  // Record the offset of this substitution map.
  auto id = SubstitutionMapIDs[substitutions];
  assert(id != 0 && "generic environment not referenced properly");
  (void)id;

  // Record the current bit.
  assert((id - 1) == SubstitutionMapOffsets.size());
  SubstitutionMapOffsets.push_back(Out.GetCurrentBitNo());

  // Collect the replacement types.
  SmallVector<uint64_t, 4> rawReplacementTypes;
  for (auto type : substitutions.getReplacementTypes())
    rawReplacementTypes.push_back(addTypeRef(type));

  auto substitutionsAbbrCode = DeclTypeAbbrCodes[SubstitutionMapLayout::Code];
  SubstitutionMapLayout::emitRecord(Out, ScratchRecord, substitutionsAbbrCode,
                                    addGenericSignatureRef(
                                      substitutions.getGenericSignature()),
                                    substitutions.getConformances().size(),
                                    rawReplacementTypes);

  writeConformances(substitutions.getConformances(), DeclTypeAbbrCodes);
}

void Serializer::writeSILLayout(SILLayout *layout) {
  using namespace decls_block;
  auto foundLayoutID = SILLayouts.find(layout);
  assert(foundLayoutID != SILLayouts.end() && "layout not referenced properly");
  assert(foundLayoutID->second - 1 == SILLayoutOffsets.size());
  (void) foundLayoutID;
  SILLayoutOffsets.push_back(Out.GetCurrentBitNo());
  
  SmallVector<unsigned, 16> data;
  // Save field types.
  for (auto &field : layout->getFields()) {
    unsigned typeRef = addTypeRef(field.getLoweredType());
    // Set the high bit if mutable.
    if (field.isMutable())
      typeRef |= 0x80000000U;
    data.push_back(typeRef);
  }
  
  unsigned abbrCode
    = DeclTypeAbbrCodes[SILLayoutLayout::Code];

  SILLayoutLayout::emitRecord(
                        Out, ScratchRecord, abbrCode,
                        addGenericSignatureRef(layout->getGenericSignature()),
                        layout->getFields().size(),
                        data);
}

void Serializer::writeNormalConformance(
       const NormalProtocolConformance *conformance) {
  using namespace decls_block;

  // The conformance must be complete, or we can't serialize it.
  assert(conformance->isComplete());

  auto conformanceID = NormalConformances[conformance];
  assert(conformanceID != 0 && "normal conformance not referenced properly");
  (void)conformanceID;

  assert((conformanceID - 1) == NormalConformanceOffsets.size());
  NormalConformanceOffsets.push_back(Out.GetCurrentBitNo());

  auto protocol = conformance->getProtocol();

  SmallVector<DeclID, 32> data;
  unsigned numValueWitnesses = 0;
  unsigned numTypeWitnesses = 0;

  conformance->forEachTypeWitness(/*resolver=*/nullptr,
                                  [&](AssociatedTypeDecl *assocType,
                                      Type type, TypeDecl *typeDecl) {
    data.push_back(addDeclRef(assocType));
    data.push_back(addTypeRef(type));
    data.push_back(addDeclRef(typeDecl, /*allowTypeAliasXRef*/true));
    ++numTypeWitnesses;
    return false;
  });

  conformance->forEachValueWitness(nullptr,
    [&](ValueDecl *req, Witness witness) {
      ++numValueWitnesses;
      data.push_back(addDeclRef(req));
      data.push_back(addDeclRef(witness.getDecl()));
      assert(witness.getDecl() || req->getAttrs().hasAttribute<OptionalAttr>()
             || req->getAttrs().isUnavailable(req->getASTContext()));

      // If there is no witness, we're done.
      if (!witness.getDecl()) return;

      auto subs = witness.getSubstitutions();

      // Canonicalize away typealiases, since these substitutions aren't used
      // for diagnostics and we reference fewer declarations that way.
      subs = subs.getCanonical();

      // Map archetypes to type parameters, since we always substitute them
      // away. Note that in a merge-modules pass, we're serializing conformances
      // that we deserialized, so they will already have their replacement types
      // in terms of interface types; hence the hasArchetypes() check is
      // necessary for correctness, not just as a fast path.
      if (subs.hasArchetypes())
        subs = subs.mapReplacementTypesOutOfContext();

      data.push_back(addSubstitutionMapRef(subs));
  });

  unsigned numSignatureConformances =
      conformance->getSignatureConformances().size();

  unsigned abbrCode
    = DeclTypeAbbrCodes[NormalProtocolConformanceLayout::Code];
  auto ownerID = addDeclContextRef(conformance->getDeclContext());
  NormalProtocolConformanceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                              addDeclRef(protocol), ownerID,
                                              numTypeWitnesses,
                                              numValueWitnesses,
                                              numSignatureConformances,
                                              data);

  // Write requirement signature conformances.
  for (auto reqConformance : conformance->getSignatureConformances())
    writeConformance(reqConformance, DeclTypeAbbrCodes);
}

void
Serializer::writeConformance(ProtocolConformance *conformance,
                             const std::array<unsigned, 256> &abbrCodes,
                             GenericEnvironment *genericEnv) {
  writeConformance(ProtocolConformanceRef(conformance), abbrCodes, genericEnv);
}

void
Serializer::writeConformance(ProtocolConformanceRef conformanceRef,
                             const std::array<unsigned, 256> &abbrCodes,
                             GenericEnvironment *genericEnv) {
  using namespace decls_block;

  if (conformanceRef.isInvalid()) {
    unsigned abbrCode = abbrCodes[InvalidProtocolConformanceLayout::Code];
    InvalidProtocolConformanceLayout::emitRecord(Out, ScratchRecord, abbrCode);
    return;
  }

  if (conformanceRef.isAbstract()) {
    unsigned abbrCode = abbrCodes[AbstractProtocolConformanceLayout::Code];
    AbstractProtocolConformanceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                      addDeclRef(conformanceRef.getAbstract()));
    return;
  }

  auto conformance = conformanceRef.getConcrete();
  switch (conformance->getKind()) {
  case ProtocolConformanceKind::Normal: {
    auto normal = cast<NormalProtocolConformance>(conformance);
    if (!isDeclXRef(getDeclForContext(normal->getDeclContext()))
        && !isa<ClangModuleUnit>(normal->getDeclContext()
                                       ->getModuleScopeContext())) {
      // A normal conformance in this module file.
      unsigned abbrCode = abbrCodes[NormalProtocolConformanceIdLayout::Code];
      NormalProtocolConformanceIdLayout::emitRecord(Out, ScratchRecord,
                                                    abbrCode,
                                                    addConformanceRef(normal));
    } else {
      // A conformance in a different module file.
      unsigned abbrCode = abbrCodes[ProtocolConformanceXrefLayout::Code];
      ProtocolConformanceXrefLayout::emitRecord(
        Out, ScratchRecord,
        abbrCode,
        addDeclRef(normal->getProtocol()),
        addDeclRef(normal->getType()->getAnyNominal()),
        addContainingModuleRef(normal->getDeclContext()));
    }
    break;
  }

  case ProtocolConformanceKind::Self: {
    auto self = cast<SelfProtocolConformance>(conformance);
    unsigned abbrCode = abbrCodes[SelfProtocolConformanceLayout::Code];
    auto protocolID = addDeclRef(self->getProtocol());
    SelfProtocolConformanceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                              protocolID);
    break;
  }

  case ProtocolConformanceKind::Specialized: {
    auto conf = cast<SpecializedProtocolConformance>(conformance);
    unsigned abbrCode = abbrCodes[SpecializedProtocolConformanceLayout::Code];
    auto type = conf->getType();
    if (genericEnv && type->hasArchetype())
      type = type->mapTypeOutOfContext();
    SpecializedProtocolConformanceLayout::emitRecord(
                           Out, ScratchRecord,
                           abbrCode,
                           addTypeRef(type),
                           addSubstitutionMapRef(conf->getSubstitutionMap()));

    writeConformance(conf->getGenericConformance(), abbrCodes, genericEnv);
    break;
  }

  case ProtocolConformanceKind::Inherited: {
    auto conf = cast<InheritedProtocolConformance>(conformance);
    unsigned abbrCode
      = abbrCodes[InheritedProtocolConformanceLayout::Code];

    auto type = conf->getType();
    if (genericEnv && type->hasArchetype())
      type = type->mapTypeOutOfContext();

    InheritedProtocolConformanceLayout::emitRecord(
      Out, ScratchRecord, abbrCode, addTypeRef(type));

    writeConformance(conf->getInheritedConformance(), abbrCodes, genericEnv);
    break;
  }
  }
}

void
Serializer::writeConformances(ArrayRef<ProtocolConformanceRef> conformances,
                              const std::array<unsigned, 256> &abbrCodes) {
  using namespace decls_block;

  for (auto conformance : conformances)
    writeConformance(conformance, abbrCodes);
}

void
Serializer::writeConformances(ArrayRef<ProtocolConformance*> conformances,
                              const std::array<unsigned, 256> &abbrCodes) {
  using namespace decls_block;

  for (auto conformance : conformances)
    writeConformance(conformance, abbrCodes);
}

static uint8_t getRawStableOptionalTypeKind(swift::OptionalTypeKind kind) {
  switch (kind) {
  case swift::OTK_None:
    return static_cast<uint8_t>(serialization::OptionalTypeKind::None);
  case swift::OTK_Optional:
    return static_cast<uint8_t>(serialization::OptionalTypeKind::Optional);
  case swift::OTK_ImplicitlyUnwrappedOptional:
    return static_cast<uint8_t>(
             serialization::OptionalTypeKind::ImplicitlyUnwrappedOptional);
  }

  llvm_unreachable("Unhandled OptionalTypeKind in switch.");
}

static bool shouldSerializeMember(Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::TopLevelCode:
  case DeclKind::Extension:
  case DeclKind::Module:
  case DeclKind::PrecedenceGroup:
    llvm_unreachable("decl should never be a member");

  case DeclKind::MissingMember:
    llvm_unreachable("should never need to reserialize a member placeholder");

  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
    return false;

  case DeclKind::EnumCase:
    return false;

  case DeclKind::OpaqueType:
    return true;
      
  case DeclKind::EnumElement:
  case DeclKind::Protocol:
  case DeclKind::Constructor:
  case DeclKind::Destructor:
  case DeclKind::PatternBinding:
  case DeclKind::Subscript:
  case DeclKind::TypeAlias:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Var:
  case DeclKind::Param:
  case DeclKind::Func:
  case DeclKind::Accessor:
    return true;
  }

  llvm_unreachable("Unhandled DeclKind in switch.");
}

static serialization::AccessorKind getStableAccessorKind(swift::AccessorKind K){
  switch (K) {
#define ACCESSOR(ID) \
  case swift::AccessorKind::ID: return serialization::ID;
#include "swift/AST/AccessorKinds.def"
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}

static serialization::CtorInitializerKind
getStableCtorInitializerKind(swift::CtorInitializerKind K){
  switch (K) {
#define CASE(NAME) \
  case swift::CtorInitializerKind::NAME: return serialization::NAME;
      CASE(Designated)
      CASE(Convenience)
      CASE(Factory)
      CASE(ConvenienceFactory)
#undef CASE
  }

  llvm_unreachable("Unhandled CtorInitializerKind in switch.");
}

void Serializer::writeCrossReference(const DeclContext *DC, uint32_t pathLen) {
  using namespace decls_block;

  unsigned abbrCode;

  switch (DC->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::Initializer:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::SerializedLocal:
  case DeclContextKind::EnumElementDecl:
    llvm_unreachable("cannot cross-reference this context");

  case DeclContextKind::Module:
    llvm_unreachable("should only cross-reference something within a file");

  case DeclContextKind::FileUnit:
    abbrCode = DeclTypeAbbrCodes[XRefLayout::Code];
    XRefLayout::emitRecord(Out, ScratchRecord, abbrCode,
                           addContainingModuleRef(DC), pathLen);
    break;

  case DeclContextKind::GenericTypeDecl: {
    auto generic = cast<GenericTypeDecl>(DC);

    // Opaque return types are unnamed and need a special xref.
    if (auto opaque = dyn_cast<OpaqueTypeDecl>(generic)) {
      if (!opaque->hasName()) {
        abbrCode = DeclTypeAbbrCodes[XRefOpaqueReturnTypePathPieceLayout::Code];
        
        XRefOpaqueReturnTypePathPieceLayout::emitRecord(Out, ScratchRecord,
                  abbrCode,
                  addDeclBaseNameRef(opaque->getOpaqueReturnTypeIdentifier()));
        break;
      }
    }
      
    assert(generic->hasName());
    
    writeCrossReference(DC->getParent(), pathLen + 1);

    abbrCode = DeclTypeAbbrCodes[XRefTypePathPieceLayout::Code];

    Identifier discriminator;
    if (generic->isOutermostPrivateOrFilePrivateScope()) {
      auto *containingFile = cast<FileUnit>(generic->getModuleScopeContext());
      discriminator = containingFile->getDiscriminatorForPrivateValue(generic);
    }

    bool isProtocolExt = DC->getParent()->getExtendedProtocolDecl();

    XRefTypePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                        addDeclBaseNameRef(generic->getName()),
                                        addDeclBaseNameRef(discriminator),
                                        isProtocolExt,
                                        generic->hasClangNode());
    break;
  }

  case DeclContextKind::ExtensionDecl: {
    auto ext = cast<ExtensionDecl>(DC);
    auto nominal = ext->getExtendedNominal();
    assert(nominal);
    writeCrossReference(nominal, pathLen + 1);

    abbrCode = DeclTypeAbbrCodes[XRefExtensionPathPieceLayout::Code];
    CanGenericSignature genericSig(nullptr);
    if (ext->isConstrainedExtension()) {
      genericSig = ext->getGenericSignature()->getCanonicalSignature();
    }
    XRefExtensionPathPieceLayout::emitRecord(
        Out, ScratchRecord, abbrCode, addContainingModuleRef(DC),
        addGenericSignatureRef(genericSig));
    break;
  }

  case DeclContextKind::SubscriptDecl: {
    auto SD = cast<SubscriptDecl>(DC);
    writeCrossReference(DC->getParent(), pathLen + 1);
    
    Type ty = SD->getInterfaceType()->getCanonicalType();

    abbrCode = DeclTypeAbbrCodes[XRefValuePathPieceLayout::Code];
    bool isProtocolExt = SD->getDeclContext()->getExtendedProtocolDecl();
    XRefValuePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         addTypeRef(ty), SUBSCRIPT_ID,
                                         isProtocolExt, SD->hasClangNode(),
                                         SD->isStatic());
    break;
  }
      
  case DeclContextKind::AbstractFunctionDecl: {
    if (auto fn = dyn_cast<AccessorDecl>(DC)) {
      auto storage = fn->getStorage();
      writeCrossReference(storage->getDeclContext(), pathLen + 2);

      Type ty = storage->getInterfaceType()->getCanonicalType();
      IdentifierID nameID = addDeclBaseNameRef(storage->getBaseName());
      bool isProtocolExt = fn->getDeclContext()->getExtendedProtocolDecl();
      abbrCode = DeclTypeAbbrCodes[XRefValuePathPieceLayout::Code];
      XRefValuePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                           addTypeRef(ty), nameID,
                                           isProtocolExt,
                                           storage->hasClangNode(),
                                           storage->isStatic());

      abbrCode =
        DeclTypeAbbrCodes[XRefOperatorOrAccessorPathPieceLayout::Code];
      auto emptyID = addDeclBaseNameRef(Identifier());
      auto accessorKind = getStableAccessorKind(fn->getAccessorKind());
      assert(!fn->isObservingAccessor() &&
             "cannot form cross-reference to observing accessors");
      XRefOperatorOrAccessorPathPieceLayout::emitRecord(Out, ScratchRecord,
                                                        abbrCode, emptyID,
                                                        accessorKind);
      break;
    }

    auto fn = cast<AbstractFunctionDecl>(DC);
    writeCrossReference(DC->getParent(), pathLen + 1 + fn->isOperator());

    Type ty = fn->getInterfaceType()->getCanonicalType();

    if (auto ctor = dyn_cast<ConstructorDecl>(DC)) {
      abbrCode = DeclTypeAbbrCodes[XRefInitializerPathPieceLayout::Code];
      XRefInitializerPathPieceLayout::emitRecord(
        Out, ScratchRecord, abbrCode, addTypeRef(ty),
        (bool)ctor->getDeclContext()->getExtendedProtocolDecl(),
        ctor->hasClangNode(),
        getStableCtorInitializerKind(ctor->getInitKind()));
      break;
    }

    abbrCode = DeclTypeAbbrCodes[XRefValuePathPieceLayout::Code];
    bool isProtocolExt = fn->getDeclContext()->getExtendedProtocolDecl();
    XRefValuePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         addTypeRef(ty),
                                         addDeclBaseNameRef(fn->getBaseName()),
                                         isProtocolExt, fn->hasClangNode(),
                                         fn->isStatic());

    if (fn->isOperator()) {
      // Encode the fixity as a filter on the func decls, to distinguish prefix
      // and postfix operators.
      auto op = cast<FuncDecl>(fn)->getOperatorDecl();
      assert(op);
      abbrCode = DeclTypeAbbrCodes[XRefOperatorOrAccessorPathPieceLayout::Code];
      auto emptyID = addDeclBaseNameRef(Identifier());
      auto fixity = getStableFixity(op->getKind());
      XRefOperatorOrAccessorPathPieceLayout::emitRecord(Out, ScratchRecord,
                                                        abbrCode, emptyID,
                                                        fixity);
    }
    break;
  }
  }
}

void Serializer::writeCrossReference(const Decl *D) {
  using namespace decls_block;

  unsigned abbrCode;

  if (auto op = dyn_cast<OperatorDecl>(D)) {
    writeCrossReference(op->getDeclContext(), 1);

    abbrCode = DeclTypeAbbrCodes[XRefOperatorOrAccessorPathPieceLayout::Code];
    auto nameID = addDeclBaseNameRef(op->getName());
    auto fixity = getStableFixity(op->getKind());
    XRefOperatorOrAccessorPathPieceLayout::emitRecord(Out, ScratchRecord,
                                                      abbrCode, nameID,
                                                      fixity);
    return;
  }

  if (auto prec = dyn_cast<PrecedenceGroupDecl>(D)) {
    writeCrossReference(prec->getDeclContext(), 1);

    abbrCode = DeclTypeAbbrCodes[XRefOperatorOrAccessorPathPieceLayout::Code];
    auto nameID = addDeclBaseNameRef(prec->getName());
    uint8_t fixity = OperatorKind::PrecedenceGroup;
    XRefOperatorOrAccessorPathPieceLayout::emitRecord(Out, ScratchRecord,
                                                      abbrCode, nameID,
                                                      fixity);
    return;
  }

  if (auto fn = dyn_cast<AbstractFunctionDecl>(D)) {
    // Functions are special because they might be operators.
    writeCrossReference(fn, 0);
    return;
  }

  writeCrossReference(D->getDeclContext());

  if (auto opaque = dyn_cast<OpaqueTypeDecl>(D)) {
    abbrCode = DeclTypeAbbrCodes[XRefOpaqueReturnTypePathPieceLayout::Code];
    XRefOpaqueReturnTypePathPieceLayout::emitRecord(Out, ScratchRecord,
                   abbrCode,
                   addDeclBaseNameRef(opaque->getOpaqueReturnTypeIdentifier()));
    return;
  }
  
  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(D)) {
    assert(!D->getDeclContext()->isModuleScopeContext() &&
           "Cannot cross reference a generic type decl at module scope.");
    abbrCode = DeclTypeAbbrCodes[XRefGenericParamPathPieceLayout::Code];
    XRefGenericParamPathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                                genericParam->getDepth(),
                                                genericParam->getIndex());
    return;
  }

  bool isProtocolExt = D->getDeclContext()->getExtendedProtocolDecl();
  if (auto type = dyn_cast<TypeDecl>(D)) {
    abbrCode = DeclTypeAbbrCodes[XRefTypePathPieceLayout::Code];

    Identifier discriminator;
    if (type->isOutermostPrivateOrFilePrivateScope()) {
      auto *containingFile =
         cast<FileUnit>(type->getDeclContext()->getModuleScopeContext());
      discriminator = containingFile->getDiscriminatorForPrivateValue(type);
    }

    XRefTypePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                        addDeclBaseNameRef(type->getName()),
                                        addDeclBaseNameRef(discriminator),
                                        isProtocolExt, D->hasClangNode());
    return;
  }

  auto val = cast<ValueDecl>(D);
  auto ty = val->getInterfaceType()->getCanonicalType();
  abbrCode = DeclTypeAbbrCodes[XRefValuePathPieceLayout::Code];
  IdentifierID iid = addDeclBaseNameRef(val->getBaseName());
  XRefValuePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       addTypeRef(ty), iid, isProtocolExt,
                                       D->hasClangNode(), val->isStatic());
}

/// Translate from the AST associativity enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableAssociativity(swift::Associativity assoc) {
  switch (assoc) {
  case swift::Associativity::Left:
    return serialization::Associativity::LeftAssociative;
  case swift::Associativity::Right:
    return serialization::Associativity::RightAssociative;
  case swift::Associativity::None:
    return serialization::Associativity::NonAssociative;
  }

  llvm_unreachable("Unhandled Associativity in switch.");
}

static serialization::StaticSpellingKind
getStableStaticSpelling(swift::StaticSpellingKind SS) {
  switch (SS) {
  case swift::StaticSpellingKind::None:
    return serialization::StaticSpellingKind::None;
  case swift::StaticSpellingKind::KeywordStatic:
    return serialization::StaticSpellingKind::KeywordStatic;
  case swift::StaticSpellingKind::KeywordClass:
    return serialization::StaticSpellingKind::KeywordClass;
  }

  llvm_unreachable("Unhandled StaticSpellingKind in switch.");
}

static uint8_t getRawStableAccessLevel(swift::AccessLevel access) {
  switch (access) {
#define CASE(NAME) \
  case swift::AccessLevel::NAME: \
    return static_cast<uint8_t>(serialization::AccessLevel::NAME);
  CASE(Private)
  CASE(FilePrivate)
  CASE(Internal)
  CASE(Public)
  CASE(Open)
#undef CASE
  }

  llvm_unreachable("Unhandled AccessLevel in switch.");
}

static serialization::SelfAccessKind
getStableSelfAccessKind(swift::SelfAccessKind MM) {
  switch (MM) {
  case swift::SelfAccessKind::NonMutating:
    return serialization::SelfAccessKind::NonMutating;
  case swift::SelfAccessKind::Mutating:
    return serialization::SelfAccessKind::Mutating;
  case swift::SelfAccessKind::__Consuming:
    return serialization::SelfAccessKind::__Consuming;
  }

  llvm_unreachable("Unhandled StaticSpellingKind in switch.");
}

#ifndef NDEBUG
// This is done with a macro so that we get a slightly more useful assertion.
# define DECL(KIND, PARENT)\
LLVM_ATTRIBUTE_UNUSED \
static void verifyAttrSerializable(const KIND ## Decl *D) {\
  for (auto Attr : D->getAttrs()) {\
    assert(Attr->canAppearOnDecl(D) && "attribute cannot appear on a " #KIND);\
  }\
}
# include "swift/AST/DeclNodes.def"

#else
static void verifyAttrSerializable(const Decl *D) {}
#endif

static inline unsigned getOptionalOrZero(const llvm::Optional<unsigned> &X) {
  if (X.hasValue())
    return X.getValue();
  return 0;
}

bool Serializer::isDeclXRef(const Decl *D) const {
  const DeclContext *topLevel = D->getDeclContext()->getModuleScopeContext();
  if (topLevel->getParentModule() != M)
    return true;
  if (!SF || topLevel == SF)
    return false;
  // Special-case for SIL generic parameter decls, which don't have a real
  // DeclContext.
  if (!isa<FileUnit>(topLevel)) {
    assert(isa<GenericTypeParamDecl>(D) && "unexpected decl kind");
    return false;
  }
  return true;
}

void Serializer::writeDeclContext(const DeclContext *DC) {
  using namespace decls_block;
  auto isDecl = false;
  auto id = DeclContextIDs[DC];
  assert(id != 0 && "decl context not referenced properly");
  (void)id;

  assert((id - 1) == DeclContextOffsets.size());
  DeclContextOffsets.push_back(Out.GetCurrentBitNo());

  auto abbrCode = DeclTypeAbbrCodes[DeclContextLayout::Code];
  DeclContextID declOrDeclContextID = 0;

  switch (DC->getContextKind()) {
  case DeclContextKind::AbstractFunctionDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::GenericTypeDecl:
  case DeclContextKind::ExtensionDecl:
  case DeclContextKind::EnumElementDecl:
    declOrDeclContextID = addDeclRef(getDeclForContext(DC));
    isDecl = true;
    break;

  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::Initializer:
  case DeclContextKind::SerializedLocal:
    declOrDeclContextID = addLocalDeclContextRef(DC);
    break;
  case DeclContextKind::Module:
    llvm_unreachable("References to the module are serialized implicitly");
  case DeclContextKind::FileUnit:
    llvm_unreachable("Can't serialize a FileUnit");
  }

  DeclContextLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                declOrDeclContextID, isDecl);
}

void Serializer::writePatternBindingInitializer(PatternBindingDecl *binding,
                                                unsigned bindingIndex) {
  using namespace decls_block;
  auto abbrCode = DeclTypeAbbrCodes[PatternBindingInitializerLayout::Code];

  StringRef initStr;
  SmallString<128> scratch;
  auto &entry = binding->getPatternList()[bindingIndex];
  auto varDecl = entry.getAnchoringVarDecl();
  if (entry.hasInitStringRepresentation() &&
      varDecl->isInitExposedToClients()) {
    initStr = entry.getInitStringRepresentation(scratch);
  }

  PatternBindingInitializerLayout::emitRecord(Out, ScratchRecord,
                                              abbrCode, addDeclRef(binding),
                                              bindingIndex, initStr);
}

void
Serializer::writeDefaultArgumentInitializer(const DeclContext *parentContext,
                                            unsigned index) {
  using namespace decls_block;
  auto abbrCode = DeclTypeAbbrCodes[DefaultArgumentInitializerLayout::Code];
  DefaultArgumentInitializerLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                               addDeclContextRef(parentContext),
                                               index);
}

void Serializer::writeAbstractClosureExpr(const DeclContext *parentContext,
                                          Type Ty, bool isImplicit,
                                          unsigned discriminator) {
  using namespace decls_block;
  auto abbrCode = DeclTypeAbbrCodes[AbstractClosureExprLayout::Code];
  AbstractClosureExprLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                        addTypeRef(Ty), isImplicit,
                                        discriminator,
                                        addDeclContextRef(parentContext));
}

void Serializer::writeLocalDeclContext(const DeclContext *DC) {
  using namespace decls_block;

  assert(shouldSerializeAsLocalContext(DC) &&
         "Can't serialize as local context");

  auto id = LocalDeclContextIDs[DC];
  assert(id != 0 && "decl context not referenced properly");
  (void)id;

  assert((id - 1)== LocalDeclContextOffsets.size());
  LocalDeclContextOffsets.push_back(Out.GetCurrentBitNo());

  switch (DC->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr: {
    auto ACE = cast<AbstractClosureExpr>(DC);
    writeAbstractClosureExpr(ACE->getParent(), ACE->getType(),
                             ACE->isImplicit(), ACE->getDiscriminator());
    break;
  }

  case DeclContextKind::Initializer: {
    if (auto PBI = dyn_cast<PatternBindingInitializer>(DC)) {
      writePatternBindingInitializer(PBI->getBinding(), PBI->getBindingIndex());
    } else if (auto DAI = dyn_cast<DefaultArgumentInitializer>(DC)) {
      writeDefaultArgumentInitializer(DAI->getParent(), DAI->getIndex());
    }
    break;
  }

  case DeclContextKind::TopLevelCodeDecl: {
    auto abbrCode = DeclTypeAbbrCodes[TopLevelCodeDeclContextLayout::Code];
    TopLevelCodeDeclContextLayout::emitRecord(Out, ScratchRecord, abbrCode,
      addDeclContextRef(DC->getParent()));
    break;
  }

  // If we are merging already serialized modules with local decl contexts,
  // we handle them here in a similar fashion.
  case DeclContextKind::SerializedLocal: {
    auto local = cast<SerializedLocalDeclContext>(DC);
    switch (local->getLocalDeclContextKind()) {
    case LocalDeclContextKind::AbstractClosure: {
      auto SACE = cast<SerializedAbstractClosureExpr>(local);
      writeAbstractClosureExpr(SACE->getParent(), SACE->getType(),
                               SACE->isImplicit(), SACE->getDiscriminator());
      return;
    }
    case LocalDeclContextKind::DefaultArgumentInitializer: {
      auto DAI = cast<SerializedDefaultArgumentInitializer>(local);
      writeDefaultArgumentInitializer(DAI->getParent(), DAI->getIndex());
      return;
    }
    case LocalDeclContextKind::PatternBindingInitializer: {
      auto PBI = cast<SerializedPatternBindingInitializer>(local);
      writePatternBindingInitializer(PBI->getBinding(), PBI->getBindingIndex());
      return;
    }
    case LocalDeclContextKind::TopLevelCodeDecl: {
      auto abbrCode = DeclTypeAbbrCodes[TopLevelCodeDeclContextLayout::Code];
      TopLevelCodeDeclContextLayout::emitRecord(Out, ScratchRecord,
        abbrCode, addDeclContextRef(DC->getParent()));
      return;
    }
    }
  }

  default:
    llvm_unreachable("Trying to write a DeclContext that isn't local");
  }
}

static ForeignErrorConventionKind getRawStableForeignErrorConventionKind(
                                    ForeignErrorConvention::Kind kind) {
  switch (kind) {
  case ForeignErrorConvention::ZeroResult:
    return ForeignErrorConventionKind::ZeroResult;
  case ForeignErrorConvention::NonZeroResult:
    return ForeignErrorConventionKind::NonZeroResult;
  case ForeignErrorConvention::ZeroPreservedResult:
    return ForeignErrorConventionKind::ZeroPreservedResult;
  case ForeignErrorConvention::NilResult:
    return ForeignErrorConventionKind::NilResult;
  case ForeignErrorConvention::NonNilError:
    return ForeignErrorConventionKind::NonNilError;
  }

  llvm_unreachable("Unhandled ForeignErrorConvention in switch.");
}

/// Translate from the AST VarDeclSpecifier enum to the
/// Serialization enum values, which are guaranteed to be stable.
static uint8_t getRawStableVarDeclSpecifier(swift::VarDecl::Specifier sf) {
  switch (sf) {
  case swift::VarDecl::Specifier::Let:
    return uint8_t(serialization::VarDeclSpecifier::Let);
  case swift::VarDecl::Specifier::Var:
    return uint8_t(serialization::VarDeclSpecifier::Var);
  case swift::VarDecl::Specifier::InOut:
    return uint8_t(serialization::VarDeclSpecifier::InOut);
  case swift::VarDecl::Specifier::Shared:
    return uint8_t(serialization::VarDeclSpecifier::Shared);
  case swift::VarDecl::Specifier::Owned:
    return uint8_t(serialization::VarDeclSpecifier::Owned);
  }
  llvm_unreachable("bad variable decl specifier kind");
}

/// Returns true if the declaration of \p decl depends on \p problemContext
/// based on lexical nesting.
///
/// - \p decl is \p problemContext
/// - \p decl is declared within \p problemContext
/// - \p decl is declared in an extension of a type that depends on
///   \p problemContext
static bool contextDependsOn(const NominalTypeDecl *decl,
                             const DeclContext *problemContext) {
  SmallPtrSet<const ExtensionDecl *, 8> seenExtensionDCs;

  const DeclContext *dc = decl;
  do {
    if (dc == problemContext)
      return true;

    if (auto *extension = dyn_cast<ExtensionDecl>(dc)) {
      if (extension->isChildContextOf(problemContext))
        return true;

      // Avoid cycles when Left.Nested depends on Right.Nested somehow.
      bool isNewlySeen = seenExtensionDCs.insert(extension).second;
      if (!isNewlySeen)
        break;
      dc = extension->getSelfNominalTypeDecl();

    } else {
      dc = dc->getParent();
    }
  } while (dc);

  return false;
}

static void collectDependenciesFromType(llvm::SmallSetVector<Type, 4> &seen,
                                        Type ty,
                                        const DeclContext *excluding) {
  ty.visit([&](Type next) {
    auto *nominal = next->getAnyNominal();
    if (!nominal)
      return;
    if (contextDependsOn(nominal, excluding))
      return;
    seen.insert(nominal->getDeclaredInterfaceType());
  });
}

static void
collectDependenciesFromRequirement(llvm::SmallSetVector<Type, 4> &seen,
                                   const Requirement &req,
                                   const DeclContext *excluding) {
  collectDependenciesFromType(seen, req.getFirstType(), excluding);
  if (req.getKind() != RequirementKind::Layout)
    collectDependenciesFromType(seen, req.getSecondType(), excluding);
}

static SmallVector<Type, 4> collectDependenciesFromType(Type ty) {
  llvm::SmallSetVector<Type, 4> result;
  collectDependenciesFromType(result, ty, /*excluding*/nullptr);
  return result.takeVector();
}

class Serializer::DeclSerializer : public DeclVisitor<DeclSerializer> {
  Serializer &S;
  DeclID id;
  bool didVerifyAttrs = false;

  template <typename DeclKind>
  void verifyAttrSerializable(const DeclKind *D) {
    ::verifyAttrSerializable(D);
    didVerifyAttrs = true;
  }

  void writeDeclAttribute(const DeclAttribute *DA) {
    using namespace decls_block;

    // Completely ignore attributes that aren't serialized.
    if (DA->isNotSerialized())
      return;

    // Ignore attributes that have been marked invalid. (This usually means
    // type-checking removed them, but only provided a warning rather than an
    // error.)
    if (DA->isInvalid())
      return;

    switch (DA->getKind()) {
    case DAK_RawDocComment:
    case DAK_ReferenceOwnership: // Serialized as part of the type.
    case DAK_AccessControl:
    case DAK_SetterAccess:
    case DAK_ObjCBridged:
    case DAK_SynthesizedProtocol:
    case DAK_Implements:
    case DAK_ObjCRuntimeName:
    case DAK_RestatedObjCConformance:
    case DAK_ClangImporterSynthesizedType:
    case DAK_PrivateImport:
      llvm_unreachable("cannot serialize attribute");

    case DAK_Count:
      llvm_unreachable("not a real attribute");

  #define SIMPLE_DECL_ATTR(_, CLASS, ...)\
    case DAK_##CLASS: { \
      auto abbrCode = S.DeclTypeAbbrCodes[CLASS##DeclAttrLayout::Code]; \
      CLASS##DeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode, \
                                        DA->isImplicit()); \
      return; \
    }
  #include "swift/AST/Attr.def"

    case DAK_SILGenName: {
      auto *theAttr = cast<SILGenNameAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[SILGenNameDeclAttrLayout::Code];
      SILGenNameDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
  	                                       theAttr->isImplicit(),
  	                                       theAttr->Name);
      return;
    }

    case DAK_CDecl: {
      auto *theAttr = cast<CDeclAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[CDeclDeclAttrLayout::Code];
      CDeclDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                      theAttr->isImplicit(),
                                      theAttr->Name);
      return;
    }

    case DAK_Alignment: {
      auto *theAlignment = cast<AlignmentAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[AlignmentDeclAttrLayout::Code];
      AlignmentDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                          theAlignment->isImplicit(),
                                          theAlignment->getValue());
      return;
    }

    case DAK_SwiftNativeObjCRuntimeBase: {
      auto *theBase = cast<SwiftNativeObjCRuntimeBaseAttr>(DA);
      auto abbrCode
        = S.DeclTypeAbbrCodes[SwiftNativeObjCRuntimeBaseDeclAttrLayout::Code];
      auto nameID = S.addDeclBaseNameRef(theBase->BaseClassName);

      SwiftNativeObjCRuntimeBaseDeclAttrLayout::emitRecord(
          S.Out, S.ScratchRecord, abbrCode,
          theBase->isImplicit(), nameID);
      return;
    }

    case DAK_Semantics: {
      auto *theAttr = cast<SemanticsAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[SemanticsDeclAttrLayout::Code];
      SemanticsDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                        theAttr->isImplicit(),
                                        theAttr->Value);
      return;
    }

    case DAK_Inline: {
      auto *theAttr = cast<InlineAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[InlineDeclAttrLayout::Code];
      InlineDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                       (unsigned)theAttr->getKind());
      return;
    }

    case DAK_Optimize: {
      auto *theAttr = cast<OptimizeAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[OptimizeDeclAttrLayout::Code];
      OptimizeDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                         (unsigned)theAttr->getMode());
      return;
    }

    case DAK_Effects: {
      auto *theAttr = cast<EffectsAttr>(DA);
      auto abbrCode = S.DeclTypeAbbrCodes[EffectsDeclAttrLayout::Code];
      EffectsDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                       (unsigned)theAttr->getKind());
      return;
    }

    case DAK_Available: {
#define LIST_VER_TUPLE_PIECES(X)\
    X##_Major, X##_Minor, X##_Subminor, X##_HasMinor, X##_HasSubminor
#define DEF_VER_TUPLE_PIECES(X, X_Expr)\
    unsigned X##_Major = 0, X##_Minor = 0, X##_Subminor = 0,\
             X##_HasMinor = 0, X##_HasSubminor = 0;\
    const auto &X##_Val = X_Expr;\
    if (X##_Val.hasValue()) {\
      const auto &Y = X##_Val.getValue();\
      X##_Major = Y.getMajor();\
      X##_Minor = getOptionalOrZero(Y.getMinor());\
      X##_Subminor = getOptionalOrZero(Y.getSubminor());\
      X##_HasMinor = Y.getMinor().hasValue();\
      X##_HasSubminor = Y.getSubminor().hasValue();\
    }

      auto *theAttr = cast<AvailableAttr>(DA);
      DEF_VER_TUPLE_PIECES(Introduced, theAttr->Introduced)
      DEF_VER_TUPLE_PIECES(Deprecated, theAttr->Deprecated)
      DEF_VER_TUPLE_PIECES(Obsoleted, theAttr->Obsoleted)

      llvm::SmallString<32> blob;
      blob.append(theAttr->Message);
      blob.append(theAttr->Rename);
      auto abbrCode = S.DeclTypeAbbrCodes[AvailableDeclAttrLayout::Code];
      AvailableDeclAttrLayout::emitRecord(
          S.Out, S.ScratchRecord, abbrCode,
          theAttr->isImplicit(),
          theAttr->isUnconditionallyUnavailable(),
          theAttr->isUnconditionallyDeprecated(),
          theAttr->isPackageDescriptionVersionSpecific(),
          LIST_VER_TUPLE_PIECES(Introduced),
          LIST_VER_TUPLE_PIECES(Deprecated),
          LIST_VER_TUPLE_PIECES(Obsoleted),
          static_cast<unsigned>(theAttr->Platform),
          theAttr->Message.size(),
          theAttr->Rename.size(),
          blob);
      return;
#undef LIST_VER_TUPLE_PIECES
#undef DEF_VER_TUPLE_PIECES
    }

    case DAK_ObjC: {
      auto *theAttr = cast<ObjCAttr>(DA);
      SmallVector<IdentifierID, 4> pieces;
      unsigned numArgs = 0;
      if (auto name = theAttr->getName()) {
        numArgs = name->getNumArgs() + 1;
        for (auto piece : name->getSelectorPieces()) {
          pieces.push_back(S.addDeclBaseNameRef(piece));
        }
      }
      auto abbrCode = S.DeclTypeAbbrCodes[ObjCDeclAttrLayout::Code];
      ObjCDeclAttrLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                     theAttr->isImplicit(),
                                     theAttr->isSwift3Inferred(),
                                     theAttr->isNameImplicit(), numArgs, pieces);
      return;
    }

    case DAK_Specialize: {
      auto abbrCode = S.DeclTypeAbbrCodes[SpecializeDeclAttrLayout::Code];
      auto SA = cast<SpecializeAttr>(DA);

      SpecializeDeclAttrLayout::emitRecord(
          S.Out, S.ScratchRecord, abbrCode,
          (unsigned)SA->isExported(),
          (unsigned)SA->getSpecializationKind());
      S.writeGenericRequirements(SA->getRequirements(), S.DeclTypeAbbrCodes);
      return;
    }

    case DAK_DynamicReplacement: {
      auto abbrCode =
          S.DeclTypeAbbrCodes[DynamicReplacementDeclAttrLayout::Code];
      auto theAttr = cast<DynamicReplacementAttr>(DA);
      auto replacedFun = theAttr->getReplacedFunctionName();
      SmallVector<IdentifierID, 4> pieces;
      pieces.push_back(S.addDeclBaseNameRef(replacedFun.getBaseName()));
      for (auto argName : replacedFun.getArgumentNames())
        pieces.push_back(S.addDeclBaseNameRef(argName));
      assert(theAttr->getReplacedFunction());
      DynamicReplacementDeclAttrLayout::emitRecord(
          S.Out, S.ScratchRecord, abbrCode, false, /*implicit flag*/
          S.addDeclRef(theAttr->getReplacedFunction()), pieces.size(), pieces);
      return;
    }

    case DAK_Custom: {
      auto abbrCode = S.DeclTypeAbbrCodes[CustomDeclAttrLayout::Code];
      auto theAttr = cast<CustomAttr>(DA);
      CustomDeclAttrLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode, theAttr->isImplicit(),
        S.addTypeRef(theAttr->getTypeLoc().getType()));
      return;
    }

    case DAK_ProjectedValueProperty: {
      auto abbrCode =
          S.DeclTypeAbbrCodes[ProjectedValuePropertyDeclAttrLayout::Code];
      auto theAttr = cast<ProjectedValuePropertyAttr>(DA);
      ProjectedValuePropertyDeclAttrLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode, theAttr->isImplicit(),
        S.addDeclBaseNameRef(theAttr->ProjectionPropertyName));
      break;
    }

    }
  }

  void writeDiscriminatorsIfNeeded(const ValueDecl *value) {
    using namespace decls_block;

    auto *storage = dyn_cast<AbstractStorageDecl>(value);
    auto access = value->getFormalAccess();
    // Emit the private descriminator for private decls.
    // FIXME: We shouldn't need to encode this for /all/ private decls.
    // In theory we can follow the same rules as mangling and only include
    // the outermost private context.
    bool shouldEmitPrivateDescriminator =
        access <= swift::AccessLevel::FilePrivate &&
        !value->getDeclContext()->isLocalContext();

    // Emit the the filename for private mapping for private decls and
    // decls with private accessors if compiled with -enable-private-imports.
    bool shouldEmitFilenameForPrivate =
        S.M->arePrivateImportsEnabled() &&
        !value->getDeclContext()->isLocalContext() &&
        (access <= swift::AccessLevel::FilePrivate ||
         (storage &&
          storage->getFormalAccess() >= swift::AccessLevel::Internal &&
          storage->hasPrivateAccessor()));

    if (shouldEmitFilenameForPrivate || shouldEmitPrivateDescriminator) {
      auto topLevelContext = value->getDeclContext()->getModuleScopeContext();
      if (auto *enclosingFile = dyn_cast<FileUnit>(topLevelContext)) {
        if (shouldEmitPrivateDescriminator) {
          Identifier discriminator =
              enclosingFile->getDiscriminatorForPrivateValue(value);
          unsigned abbrCode =
              S.DeclTypeAbbrCodes[PrivateDiscriminatorLayout::Code];
          PrivateDiscriminatorLayout::emitRecord(
              S.Out, S.ScratchRecord, abbrCode,
              S.addDeclBaseNameRef(discriminator));
        }
        auto getFilename = [](FileUnit *enclosingFile,
                              const ValueDecl *decl) -> StringRef {
          if (auto *SF = dyn_cast<SourceFile>(enclosingFile)) {
            return llvm::sys::path::filename(SF->getFilename());
          } else if (auto *LF = dyn_cast<LoadedFile>(enclosingFile)) {
            return LF->getFilenameForPrivateDecl(decl);
          }
          return StringRef();
        };
        if (shouldEmitFilenameForPrivate) {
          auto filename = getFilename(enclosingFile, value);
          if (!filename.empty()) {
            auto filenameID = S.addFilename(filename);
            FilenameForPrivateLayout::emitRecord(
                S.Out, S.ScratchRecord,
                S.DeclTypeAbbrCodes[FilenameForPrivateLayout::Code],
                filenameID);
          }
        }
      }
    }

    if (value->getDeclContext()->isLocalContext()) {
      auto discriminator = value->getLocalDiscriminator();
      auto abbrCode = S.DeclTypeAbbrCodes[LocalDiscriminatorLayout::Code];
      LocalDiscriminatorLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                           discriminator);
    }
  }

  void writeForeignErrorConvention(const ForeignErrorConvention &fec) {
    using namespace decls_block;

    auto kind = getRawStableForeignErrorConventionKind(fec.getKind());
    uint8_t isOwned = fec.isErrorOwned() == ForeignErrorConvention::IsOwned;
    uint8_t isReplaced = bool(fec.isErrorParameterReplacedWithVoid());
    TypeID errorParameterTypeID = S.addTypeRef(fec.getErrorParameterType());
    TypeID resultTypeID;
    switch (fec.getKind()) {
    case ForeignErrorConvention::ZeroResult:
    case ForeignErrorConvention::NonZeroResult:
      resultTypeID = S.addTypeRef(fec.getResultType());
      break;

    case ForeignErrorConvention::ZeroPreservedResult:
    case ForeignErrorConvention::NilResult:
    case ForeignErrorConvention::NonNilError:
      resultTypeID = 0;
      break;
    }

    auto abbrCode = S.DeclTypeAbbrCodes[ForeignErrorConventionLayout::Code];
    ForeignErrorConventionLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                             static_cast<uint8_t>(kind),
                                             isOwned,
                                             isReplaced,
                                             fec.getErrorParameterIndex(),
                                             errorParameterTypeID,
                                             resultTypeID);
  }

  void writeGenericParams(const GenericParamList *genericParams) {
    using namespace decls_block;

    // Don't write anything if there are no generic params.
    if (!genericParams)
      return;

    SmallVector<DeclID, 4> paramIDs;
    for (auto next : genericParams->getParams())
      paramIDs.push_back(S.addDeclRef(next));

    unsigned abbrCode = S.DeclTypeAbbrCodes[GenericParamListLayout::Code];
    GenericParamListLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                       paramIDs);
  }

  void writeParameterList(const ParameterList *PL) {
    using namespace decls_block;

    SmallVector<DeclID, 8> paramIDs;
    for (const ParamDecl *param : *PL)
      paramIDs.push_back(S.addDeclRef(param));

    unsigned abbrCode = S.DeclTypeAbbrCodes[ParameterListLayout::Code];
    ParameterListLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode, paramIDs);
  }

  /// Writes an array of members for a decl context.
  ///
  /// \param parentID The DeclID of the context.
  /// \param members The decls within the context.
  /// \param isClass True if the context could be a class context (class,
  ///        class extension, or protocol).
  void writeMembers(DeclID parentID, DeclRange members, bool isClass) {
    using namespace decls_block;

    SmallVector<DeclID, 16> memberIDs;
    for (auto member : members) {
      if (!shouldSerializeMember(member))
        continue;

      DeclID memberID = S.addDeclRef(member);
      memberIDs.push_back(memberID);

      if (auto VD = dyn_cast<ValueDecl>(member)) {
        // Record parent->members in subtable of DeclMemberNames
        if (VD->hasName() &&
            !VD->getBaseName().empty()) {
          std::unique_ptr<DeclMembersTable> &memberTable =
            S.DeclMemberNames[VD->getBaseName()].second;
          if (!memberTable) {
            memberTable = llvm::make_unique<DeclMembersTable>();
          }
          (*memberTable)[parentID].push_back(memberID);
        }

        // Same as above, but for @_implements attributes
        if (auto A = VD->getAttrs().getAttribute<ImplementsAttr>()) {
          std::unique_ptr<DeclMembersTable> &memberTable =
            S.DeclMemberNames[A->getMemberName().getBaseName()].second;
          if (!memberTable) {
            memberTable = llvm::make_unique<DeclMembersTable>();
          }
          (*memberTable)[parentID].push_back(memberID);
        }

        // Possibly add a record to ClassMembersForDynamicLookup too.
        if (isClass) {
          if (VD->canBeAccessedByDynamicLookup()) {
            auto &list = S.ClassMembersForDynamicLookup[VD->getBaseName()];
            list.push_back({getKindForTable(VD), memberID});
          }
        }
      }
    }

    unsigned abbrCode = S.DeclTypeAbbrCodes[MembersLayout::Code];
    MembersLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode, memberIDs);
  }

  /// Writes the given pattern, recursively.
  void writePattern(const Pattern *pattern) {
    using namespace decls_block;

    // Retrieve the type of the pattern.
    auto getPatternType = [&] {
      Type type = pattern->getType();

      // If we have a contextual type, map out to an interface type.
      if (type->hasArchetype())
        type = type->mapTypeOutOfContext();

      return type;
    };

    assert(pattern && "null pattern");
    switch (pattern->getKind()) {
    case PatternKind::Paren: {
      unsigned abbrCode = S.DeclTypeAbbrCodes[ParenPatternLayout::Code];
      ParenPatternLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                     pattern->isImplicit());
      writePattern(cast<ParenPattern>(pattern)->getSubPattern());
      break;
    }
    case PatternKind::Tuple: {
      auto tuple = cast<TuplePattern>(pattern);

      unsigned abbrCode = S.DeclTypeAbbrCodes[TuplePatternLayout::Code];
      TuplePatternLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                     S.addTypeRef(getPatternType()),
                                     tuple->getNumElements(),
                                     tuple->isImplicit());

      abbrCode = S.DeclTypeAbbrCodes[TuplePatternEltLayout::Code];
      for (auto &elt : tuple->getElements()) {
        // FIXME: Default argument expressions?
        TuplePatternEltLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                          S.addDeclBaseNameRef(elt.getLabel()));
        writePattern(elt.getPattern());
      }
      break;
    }
    case PatternKind::Named: {
      auto named = cast<NamedPattern>(pattern);

      unsigned abbrCode = S.DeclTypeAbbrCodes[NamedPatternLayout::Code];
      NamedPatternLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                     S.addDeclRef(named->getDecl()),
                                     S.addTypeRef(getPatternType()),
                                     named->isImplicit());
      break;
    }
    case PatternKind::Any: {
      unsigned abbrCode = S.DeclTypeAbbrCodes[AnyPatternLayout::Code];
      AnyPatternLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                   S.addTypeRef(getPatternType()),
                                   pattern->isImplicit());
      break;
    }
    case PatternKind::Typed: {
      auto typed = cast<TypedPattern>(pattern);

      unsigned abbrCode = S.DeclTypeAbbrCodes[TypedPatternLayout::Code];
      TypedPatternLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                     S.addTypeRef(getPatternType()),
                                     typed->isImplicit());
      writePattern(typed->getSubPattern());
      break;
    }
    case PatternKind::Is:
    case PatternKind::EnumElement:
    case PatternKind::OptionalSome:
    case PatternKind::Bool:
    case PatternKind::Expr:
      llvm_unreachable("Refutable patterns cannot be serialized");

    case PatternKind::Var: {
      auto var = cast<VarPattern>(pattern);

      unsigned abbrCode = S.DeclTypeAbbrCodes[VarPatternLayout::Code];
      VarPatternLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                   var->isLet(), var->isImplicit());
      writePattern(var->getSubPattern());
      break;
    }
    }
  }

  void writeDefaultWitnessTable(const ProtocolDecl *proto) {
    using namespace decls_block;

    SmallVector<DeclID, 16> witnessIDs;

    for (auto member : proto->getMembers()) {
      if (auto *value = dyn_cast<ValueDecl>(member)) {
        auto witness = proto->getDefaultWitness(value);
        if (!witness)
          continue;

        DeclID requirementID = S.addDeclRef(value);
        DeclID witnessID = S.addDeclRef(witness.getDecl());
        witnessIDs.push_back(requirementID);
        witnessIDs.push_back(witnessID);

        // FIXME: Substitutions
      }
    }

    unsigned abbrCode = S.DeclTypeAbbrCodes[DefaultWitnessTableLayout::Code];
    DefaultWitnessTableLayout::emitRecord(S.Out, S.ScratchRecord,
                                          abbrCode, witnessIDs);
  }

  /// Writes the body text of the provided funciton, if the function is
  /// inlinable and has body text.
  void writeInlinableBodyTextIfNeeded(const AbstractFunctionDecl *AFD) {
    using namespace decls_block;
    // Only serialize the text for an inlinable function body if we're emitting
    // a partial module. It's not needed in the final module file, but it's
    // needed in partial modules so you can emit a parseable interface after
    // merging them.
    if (!S.SF) return;

    if (AFD->getResilienceExpansion() != swift::ResilienceExpansion::Minimal)
      return;

    if (!AFD->hasInlinableBodyText()) return;
    SmallString<128> scratch;
    auto body = AFD->getInlinableBodyText(scratch);

    unsigned abbrCode = S.DeclTypeAbbrCodes[InlinableBodyTextLayout::Code];
    InlinableBodyTextLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode, body);
  }

public:
  DeclSerializer(Serializer &S, DeclID id) : S(S), id(id) {}
  ~DeclSerializer() {
    assert(didVerifyAttrs);
  }

  void visit(const Decl *D) {
    // Emit attributes (if any).
    for (auto Attr : D->getAttrs())
      writeDeclAttribute(Attr);

    if (auto VD = dyn_cast<ValueDecl>(D)) {
      // Hack: synthesize a 'final' attribute if finality was inferred.
      if (VD->isFinal() && !D->getAttrs().hasAttribute<FinalAttr>())
        writeDeclAttribute(
            new (D->getASTContext()) FinalAttr(/*Implicit=*/false));
    }

    if (auto *value = dyn_cast<ValueDecl>(D))
      writeDiscriminatorsIfNeeded(value);

    DeclVisitor<DeclSerializer>::visit(const_cast<Decl *>(D));
  }

  /// If this gets referenced, we forgot to handle a decl.
  void visitDecl(const Decl *) = delete;

  void visitExtensionDecl(const ExtensionDecl *extension) {
    using namespace decls_block;

    verifyAttrSerializable(extension);

    auto contextID = S.addDeclContextRef(extension->getDeclContext());
    Type baseTy = extension->getExtendedType();
    assert(!baseTy->hasUnboundGenericType());
    assert(!baseTy->hasArchetype());

    // FIXME: Use the canonical type here in order to minimize circularity
    // issues at deserialization time. A known problematic case here is
    // "extension of typealias Foo"; "typealias Foo = SomeKit.Bar"; and then
    // trying to import Bar accidentally asking for all of its extensions
    // (perhaps because we're searching for a conformance).
    //
    // We could limit this to only the problematic cases, but it seems like a
    // simpler user model to just always desugar extension types.
    baseTy = baseTy->getCanonicalType();

    // Make sure the base type has registered itself as a provider of generic
    // parameters.
    auto baseNominal = baseTy->getAnyNominal();
    (void)S.addDeclRef(baseNominal);

    auto conformances = extension->getLocalConformances(
                          ConformanceLookupKind::All, nullptr);

    SmallVector<TypeID, 8> inheritedAndDependencyTypes;
    for (auto inherited : extension->getInherited()) {
      assert(!inherited.getType()->hasArchetype());
      inheritedAndDependencyTypes.push_back(S.addTypeRef(inherited.getType()));
    }
    size_t numInherited = inheritedAndDependencyTypes.size();

    llvm::SmallSetVector<Type, 4> dependencies;
    collectDependenciesFromType(dependencies, baseTy, /*excluding*/nullptr);
    for (Requirement req : extension->getGenericRequirements()) {
      collectDependenciesFromRequirement(dependencies, req,
                                         /*excluding*/nullptr);
    }
    for (auto dependencyTy : dependencies)
      inheritedAndDependencyTypes.push_back(S.addTypeRef(dependencyTy));

    unsigned abbrCode = S.DeclTypeAbbrCodes[ExtensionLayout::Code];
    ExtensionLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                S.addTypeRef(baseTy),
                                contextID,
                                extension->isImplicit(),
                                S.addGenericEnvironmentRef(
                                           extension->getGenericEnvironment()),
                                conformances.size(),
                                numInherited,
                                inheritedAndDependencyTypes);

    bool isClassExtension = false;
    if (baseNominal) {
      isClassExtension = isa<ClassDecl>(baseNominal) ||
                         isa<ProtocolDecl>(baseNominal);
    }

    // Extensions of nested generic types have multiple generic parameter
    // lists. Collect them all, from the innermost to outermost.
    SmallVector<GenericParamList *, 2> allGenericParams;
    for (auto *genericParams = extension->getGenericParams();
         genericParams != nullptr;
         genericParams = genericParams->getOuterParameters()) {
      allGenericParams.push_back(genericParams);
    }

    // Reverse the list, and write the parameter lists, from outermost
    // to innermost.
    std::reverse(allGenericParams.begin(), allGenericParams.end());
    for (auto *genericParams : allGenericParams)
      writeGenericParams(genericParams);

    writeMembers(id, extension->getMembers(), isClassExtension);
    S.writeConformances(conformances, S.DeclTypeAbbrCodes);
  }

  void visitPatternBindingDecl(const PatternBindingDecl *binding) {
    using namespace decls_block;
    verifyAttrSerializable(binding);

    auto contextID = S.addDeclContextRef(binding->getDeclContext());
    SmallVector<uint64_t, 2> initContextIDs;
    for (unsigned i : range(binding->getNumPatternEntries())) {
      auto initContextID =
          S.addDeclContextRef(binding->getPatternList()[i].getInitContext());
      if (!initContextIDs.empty()) {
        initContextIDs.push_back(initContextID);
      } else if (initContextID) {
        initContextIDs.append(i, 0);
        initContextIDs.push_back(initContextID);
      }
    }

    unsigned abbrCode = S.DeclTypeAbbrCodes[PatternBindingLayout::Code];
    PatternBindingLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode, contextID, binding->isImplicit(),
        binding->isStatic(),
        uint8_t(getStableStaticSpelling(binding->getStaticSpelling())),
        binding->getNumPatternEntries(),
        initContextIDs);

    DeclContext *owningDC = nullptr;
    if (binding->getDeclContext()->isTypeContext())
      owningDC = binding->getDeclContext();

    for (auto entry : binding->getPatternList()) {
      writePattern(entry.getPattern());
      // Ignore initializer; external clients don't need to know about it.
    }
  }

  void visitPrecedenceGroupDecl(const PrecedenceGroupDecl *group) {
    using namespace decls_block;
    verifyAttrSerializable(group);

    auto contextID = S.addDeclContextRef(group->getDeclContext());
    auto nameID = S.addDeclBaseNameRef(group->getName());
    auto associativity = getRawStableAssociativity(group->getAssociativity());

    SmallVector<DeclID, 8> relations;
    for (auto &rel : group->getHigherThan())
      relations.push_back(S.addDeclRef(rel.Group));
    for (auto &rel : group->getLowerThan())
      relations.push_back(S.addDeclRef(rel.Group));

    unsigned abbrCode = S.DeclTypeAbbrCodes[PrecedenceGroupLayout::Code];
    PrecedenceGroupLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                      nameID, contextID, associativity,
                                      group->isAssignment(),
                                      group->getHigherThan().size(),
                                      relations);
  }

  void visitInfixOperatorDecl(const InfixOperatorDecl *op) {
    using namespace decls_block;
    verifyAttrSerializable(op);

    auto contextID = S.addDeclContextRef(op->getDeclContext());
    auto nameID = S.addDeclBaseNameRef(op->getName());
    auto groupID = S.addDeclRef(op->getPrecedenceGroup());
    SmallVector<DeclID, 1> designatedNominalTypeDeclIDs;
    for (auto *decl : op->getDesignatedNominalTypes())
      designatedNominalTypeDeclIDs.push_back(S.addDeclRef(decl));

    unsigned abbrCode = S.DeclTypeAbbrCodes[InfixOperatorLayout::Code];
    InfixOperatorLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode, nameID,
                                    contextID, groupID,
                                    designatedNominalTypeDeclIDs);

  }

  template <typename Layout>
  void visitUnaryOperatorDecl(const OperatorDecl *op) {
    auto contextID = S.addDeclContextRef(op->getDeclContext());
    SmallVector<DeclID, 1> designatedNominalTypeDeclIDs;
    for (auto *decl : op->getDesignatedNominalTypes())
      designatedNominalTypeDeclIDs.push_back(S.addDeclRef(decl));

    unsigned abbrCode = S.DeclTypeAbbrCodes[Layout::Code];
    Layout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                       S.addDeclBaseNameRef(op->getName()),
                       contextID, designatedNominalTypeDeclIDs);
  }

  void visitPrefixOperatorDecl(const PrefixOperatorDecl *op) {
    using namespace decls_block;
    verifyAttrSerializable(op);
    visitUnaryOperatorDecl<PrefixOperatorLayout>(op);
  }

  void visitPostfixOperatorDecl(const PostfixOperatorDecl *op) {
    using namespace decls_block;
    verifyAttrSerializable(op);
    visitUnaryOperatorDecl<PostfixOperatorLayout>(op);
  }

  void visitTypeAliasDecl(const TypeAliasDecl *typeAlias) {
    using namespace decls_block;
    assert(!typeAlias->isObjC() && "ObjC typealias is not meaningful");
    verifyAttrSerializable(typeAlias);

    auto contextID = S.addDeclContextRef(typeAlias->getDeclContext());

    auto underlying = typeAlias->getUnderlyingTypeLoc().getType();

    llvm::SmallSetVector<Type, 4> dependencies;
    collectDependenciesFromType(dependencies, underlying->getCanonicalType(),
                                /*excluding*/nullptr);
    for (Requirement req : typeAlias->getGenericRequirements()) {
      collectDependenciesFromRequirement(dependencies, req,
                                         /*excluding*/nullptr);
    }

    SmallVector<TypeID, 4> dependencyIDs;
    for (Type dep : dependencies)
      dependencyIDs.push_back(S.addTypeRef(dep));

    uint8_t rawAccessLevel =
      getRawStableAccessLevel(typeAlias->getFormalAccess());

    unsigned abbrCode = S.DeclTypeAbbrCodes[TypeAliasLayout::Code];
    TypeAliasLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                S.addDeclBaseNameRef(typeAlias->getName()),
                                contextID,
                                S.addTypeRef(underlying),
                                /*no longer used*/TypeID(),
                                typeAlias->isImplicit(),
                                S.addGenericEnvironmentRef(
                                             typeAlias->getGenericEnvironment()),
                                rawAccessLevel,
                                dependencyIDs);
    writeGenericParams(typeAlias->getGenericParams());
  }

  void visitGenericTypeParamDecl(const GenericTypeParamDecl *genericParam) {
    using namespace decls_block;
    verifyAttrSerializable(genericParam);

    unsigned abbrCode = S.DeclTypeAbbrCodes[GenericTypeParamDeclLayout::Code];
    GenericTypeParamDeclLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                S.addDeclBaseNameRef(genericParam->getName()),
                                genericParam->isImplicit(),
                                genericParam->getDepth(),
                                genericParam->getIndex());
  }

  void visitAssociatedTypeDecl(const AssociatedTypeDecl *assocType) {
    using namespace decls_block;
    verifyAttrSerializable(assocType);

    auto contextID = S.addDeclContextRef(assocType->getDeclContext());
    SmallVector<DeclID, 4> overriddenAssocTypeIDs;
    for (auto overridden : assocType->getOverriddenDecls()) {
      overriddenAssocTypeIDs.push_back(S.addDeclRef(overridden));
    }

    unsigned abbrCode = S.DeclTypeAbbrCodes[AssociatedTypeDeclLayout::Code];
    AssociatedTypeDeclLayout::emitRecord(
      S.Out, S.ScratchRecord, abbrCode,
      S.addDeclBaseNameRef(assocType->getName()),
      contextID,
      S.addTypeRef(assocType->getDefaultDefinitionType()),
      assocType->isImplicit(),
      overriddenAssocTypeIDs);
  }

  void visitStructDecl(const StructDecl *theStruct) {
    using namespace decls_block;
    verifyAttrSerializable(theStruct);

    auto contextID = S.addDeclContextRef(theStruct->getDeclContext());

    auto conformances = theStruct->getLocalConformances(
                          ConformanceLookupKind::All, nullptr);

    SmallVector<TypeID, 4> inheritedAndDependencyTypes;
    for (auto inherited : theStruct->getInherited()) {
      assert(!inherited.getType()->hasArchetype());
      inheritedAndDependencyTypes.push_back(S.addTypeRef(inherited.getType()));
    }

    llvm::SmallSetVector<Type, 4> dependencyTypes;
    for (Requirement req : theStruct->getGenericRequirements()) {
      collectDependenciesFromRequirement(dependencyTypes, req,
                                         /*excluding*/nullptr);
    }
    for (Type ty : dependencyTypes)
      inheritedAndDependencyTypes.push_back(S.addTypeRef(ty));

    uint8_t rawAccessLevel =
      getRawStableAccessLevel(theStruct->getFormalAccess());

    unsigned abbrCode = S.DeclTypeAbbrCodes[StructLayout::Code];
    StructLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                             S.addDeclBaseNameRef(theStruct->getName()),
                             contextID,
                             theStruct->isImplicit(),
                             theStruct->isObjC(),
                             S.addGenericEnvironmentRef(
                                            theStruct->getGenericEnvironment()),
                             rawAccessLevel,
                             conformances.size(),
                             theStruct->getInherited().size(),
                             inheritedAndDependencyTypes);


    writeGenericParams(theStruct->getGenericParams());
    writeMembers(id, theStruct->getMembers(), false);
    S.writeConformances(conformances, S.DeclTypeAbbrCodes);
  }

  void visitEnumDecl(const EnumDecl *theEnum) {
    using namespace decls_block;
    verifyAttrSerializable(theEnum);

    auto contextID = S.addDeclContextRef(theEnum->getDeclContext());

    auto conformances = theEnum->getLocalConformances(
                          ConformanceLookupKind::All, nullptr);

    SmallVector<TypeID, 4> inheritedAndDependencyTypes;
    for (auto inherited : theEnum->getInherited()) {
      assert(!inherited.getType()->hasArchetype());
      inheritedAndDependencyTypes.push_back(S.addTypeRef(inherited.getType()));
    }

    llvm::SmallSetVector<Type, 4> dependencyTypes;
    for (const EnumElementDecl *nextElt : theEnum->getAllElements()) {
      if (!nextElt->hasAssociatedValues())
        continue;
      // FIXME: Types in the same module are still important for enums. It's
      // possible an enum element has a payload that references a type
      // declaration from the same module that can't be imported (for whatever
      // reason). However, we need a more robust handling of deserialization
      // dependencies that can handle circularities. rdar://problem/32359173
      collectDependenciesFromType(dependencyTypes,
                                  nextElt->getArgumentInterfaceType(),
                                  /*excluding*/theEnum->getParentModule());
    }
    for (Requirement req : theEnum->getGenericRequirements()) {
      collectDependenciesFromRequirement(dependencyTypes, req,
                                         /*excluding*/nullptr);
    }
    for (Type ty : dependencyTypes)
      inheritedAndDependencyTypes.push_back(S.addTypeRef(ty));

    uint8_t rawAccessLevel =
      getRawStableAccessLevel(theEnum->getFormalAccess());

    unsigned abbrCode = S.DeclTypeAbbrCodes[EnumLayout::Code];
    EnumLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                            S.addDeclBaseNameRef(theEnum->getName()),
                            contextID,
                            theEnum->isImplicit(),
                            theEnum->isObjC(),
                            S.addGenericEnvironmentRef(
                                             theEnum->getGenericEnvironment()),
                            S.addTypeRef(theEnum->getRawType()),
                            rawAccessLevel,
                            conformances.size(),
                            theEnum->getInherited().size(),
                            inheritedAndDependencyTypes);

    writeGenericParams(theEnum->getGenericParams());
    writeMembers(id, theEnum->getMembers(), false);
    S.writeConformances(conformances, S.DeclTypeAbbrCodes);
  }

  void visitClassDecl(const ClassDecl *theClass) {
    using namespace decls_block;
    verifyAttrSerializable(theClass);
    assert(!theClass->isForeign());

    auto contextID = S.addDeclContextRef(theClass->getDeclContext());

    auto conformances = theClass->getLocalConformances(
                          ConformanceLookupKind::NonInherited, nullptr);

    SmallVector<TypeID, 4> inheritedAndDependencyTypes;
    for (auto inherited : theClass->getInherited()) {
      assert(!inherited.getType()->hasArchetype());
      inheritedAndDependencyTypes.push_back(S.addTypeRef(inherited.getType()));
    }

    llvm::SmallSetVector<Type, 4> dependencyTypes;
    if (theClass->hasSuperclass()) {
      // FIXME: Nested types can still be a problem here: it's possible that (for
      // whatever reason) they won't be able to be deserialized, in which case
      // we'll be in trouble forming the actual superclass type. However, we
      // need a more robust handling of deserialization dependencies that can
      // handle circularities. rdar://problem/50835214
      collectDependenciesFromType(dependencyTypes, theClass->getSuperclass(),
                                  /*excluding*/theClass);
    }
    for (Requirement req : theClass->getGenericRequirements()) {
      collectDependenciesFromRequirement(dependencyTypes, req,
                                         /*excluding*/nullptr);
    }
    for (Type ty : dependencyTypes)
      inheritedAndDependencyTypes.push_back(S.addTypeRef(ty));

    uint8_t rawAccessLevel =
      getRawStableAccessLevel(theClass->getFormalAccess());

    bool inheritsSuperclassInitializers =
        const_cast<ClassDecl *>(theClass)->
          inheritsSuperclassInitializers(nullptr);

    unsigned abbrCode = S.DeclTypeAbbrCodes[ClassLayout::Code];
    ClassLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                            S.addDeclBaseNameRef(theClass->getName()),
                            contextID,
                            theClass->isImplicit(),
                            theClass->isObjC(),
                            theClass->requiresStoredPropertyInits(),
                            inheritsSuperclassInitializers,
                            S.addGenericEnvironmentRef(
                                             theClass->getGenericEnvironment()),
                            S.addTypeRef(theClass->getSuperclass()),
                            rawAccessLevel,
                            conformances.size(),
                            theClass->getInherited().size(),
                            inheritedAndDependencyTypes);

    writeGenericParams(theClass->getGenericParams());
    writeMembers(id, theClass->getMembers(), true);
    S.writeConformances(conformances, S.DeclTypeAbbrCodes);
  }

  void visitProtocolDecl(const ProtocolDecl *proto) {
    using namespace decls_block;
    verifyAttrSerializable(proto);

    auto contextID = S.addDeclContextRef(proto->getDeclContext());

    SmallVector<DeclID, 8> inherited;
    for (auto element : proto->getInherited()) {
      assert(!element.getType()->hasArchetype());
      inherited.push_back(S.addTypeRef(element.getType()));
    }

    uint8_t rawAccessLevel = getRawStableAccessLevel(proto->getFormalAccess());

    unsigned abbrCode = S.DeclTypeAbbrCodes[ProtocolLayout::Code];
    ProtocolLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                               S.addDeclBaseNameRef(proto->getName()),
                               contextID,
                               proto->isImplicit(),
                               const_cast<ProtocolDecl *>(proto)
                                 ->requiresClass(),
                               proto->isObjC(),
                               proto->existentialTypeSupported(
                                 /*resolver=*/nullptr),
                               S.addGenericEnvironmentRef(
                                                proto->getGenericEnvironment()),
                               rawAccessLevel,
                               inherited);

    const_cast<ProtocolDecl*>(proto)->createGenericParamsIfMissing();
    writeGenericParams(proto->getGenericParams());
    S.writeGenericRequirements(
      proto->getRequirementSignature(), S.DeclTypeAbbrCodes);
    writeMembers(id, proto->getMembers(), true);
    writeDefaultWitnessTable(proto);
  }

  void visitVarDecl(const VarDecl *var) {
    using namespace decls_block;
    verifyAttrSerializable(var);

    auto contextID = S.addDeclContextRef(var->getDeclContext());

    Accessors accessors = getAccessors(var);
    uint8_t rawAccessLevel = getRawStableAccessLevel(var->getFormalAccess());
    uint8_t rawSetterAccessLevel = rawAccessLevel;
    if (var->isSettable(nullptr))
      rawSetterAccessLevel =
        getRawStableAccessLevel(var->getSetterFormalAccess());

    unsigned numBackingProperties = 0;
    Type ty = var->getInterfaceType();
    SmallVector<TypeID, 2> arrayFields;
    for (auto accessor : accessors.Decls)
      arrayFields.push_back(S.addDeclRef(accessor));

    if (auto backingInfo = var->getPropertyWrapperBackingPropertyInfo()) {
      if (backingInfo.backingVar) {
        ++numBackingProperties;
        arrayFields.push_back(S.addDeclRef(backingInfo.backingVar));
      }
      if (backingInfo.storageWrapperVar) {
        ++numBackingProperties;
        arrayFields.push_back(S.addDeclRef(backingInfo.storageWrapperVar));
      }
    }
    for (Type dependency : collectDependenciesFromType(ty->getCanonicalType()))
      arrayFields.push_back(S.addTypeRef(dependency));

    VarDecl *lazyStorage = nullptr;
    if (var->getAttrs().hasAttribute<LazyAttr>())
      lazyStorage = var->getLazyStorageProperty();

    unsigned abbrCode = S.DeclTypeAbbrCodes[VarLayout::Code];
    VarLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                          S.addDeclBaseNameRef(var->getName()),
                          contextID,
                          var->isImplicit(),
                          var->isObjC(),
                          var->isStatic(),
                          getRawStableVarDeclSpecifier(var->getSpecifier()),
                          var->hasNonPatternBindingInit(),
                          var->isGetterMutating(),
                          var->isSetterMutating(),
                          var->isLazyStorageProperty(),
                          S.addDeclRef(lazyStorage),
                          accessors.OpaqueReadOwnership,
                          accessors.ReadImpl,
                          accessors.WriteImpl,
                          accessors.ReadWriteImpl,
                          accessors.Decls.size(),
                          S.addTypeRef(ty),
                          S.addDeclRef(var->getOverriddenDecl()),
                          rawAccessLevel, rawSetterAccessLevel,
                          S.addDeclRef(var->getOpaqueResultTypeDecl()),
                          numBackingProperties,
                          arrayFields);
  }

  void visitParamDecl(const ParamDecl *param) {
    using namespace decls_block;
    verifyAttrSerializable(param);

    auto contextID = S.addDeclContextRef(param->getDeclContext());
    Type interfaceType = param->getInterfaceType();

    // Only save the text for normal and stored property default arguments, not
    // any of the special ones.
    StringRef defaultArgumentText;
    SmallString<128> scratch;
    swift::DefaultArgumentKind argKind = param->getDefaultArgumentKind();
    if (argKind == swift::DefaultArgumentKind::Normal ||
        argKind == swift::DefaultArgumentKind::StoredProperty)
      defaultArgumentText =
        param->getDefaultValueStringRepresentation(scratch);

    unsigned abbrCode = S.DeclTypeAbbrCodes[ParamLayout::Code];
    ParamLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
        S.addDeclBaseNameRef(param->getArgumentName()),
        S.addDeclBaseNameRef(param->getName()),
        contextID,
        getRawStableVarDeclSpecifier(param->getSpecifier()),
        S.addTypeRef(interfaceType),
        param->isVariadic(),
        param->isAutoClosure(),
        getRawStableDefaultArgumentKind(argKind),
        defaultArgumentText);

    if (interfaceType->hasError()) {
      param->getDeclContext()->dumpContext();
      interfaceType->dump();
      llvm_unreachable("error in interface type of parameter");
    }
  }

  void visitFuncDecl(const FuncDecl *fn) {
    using namespace decls_block;
    verifyAttrSerializable(fn);

    auto contextID = S.addDeclContextRef(fn->getDeclContext());

    unsigned abbrCode = S.DeclTypeAbbrCodes[FuncLayout::Code];
    SmallVector<IdentifierID, 4> nameComponentsAndDependencies;
    nameComponentsAndDependencies.push_back(
        S.addDeclBaseNameRef(fn->getFullName().getBaseName()));
    for (auto argName : fn->getFullName().getArgumentNames())
      nameComponentsAndDependencies.push_back(S.addDeclBaseNameRef(argName));

    uint8_t rawAccessLevel = getRawStableAccessLevel(fn->getFormalAccess());

    Type ty = fn->getInterfaceType();
    for (auto dependency : collectDependenciesFromType(ty->getCanonicalType()))
      nameComponentsAndDependencies.push_back(S.addTypeRef(dependency));

    FuncLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                           contextID,
                           fn->isImplicit(),
                           fn->isStatic(),
                           uint8_t(
                             getStableStaticSpelling(fn->getStaticSpelling())),
                           fn->isObjC(),
                           uint8_t(
                             getStableSelfAccessKind(fn->getSelfAccessKind())),
                           fn->hasForcedStaticDispatch(),
                           fn->hasThrows(),
                           S.addGenericEnvironmentRef(
                                                  fn->getGenericEnvironment()),
                           S.addTypeRef(fn->getResultInterfaceType()),
                           S.addDeclRef(fn->getOperatorDecl()),
                           S.addDeclRef(fn->getOverriddenDecl()),
                           fn->getFullName().getArgumentNames().size() +
                             fn->getFullName().isCompoundName(),
                           rawAccessLevel,
                           fn->needsNewVTableEntry(),
                           S.addDeclRef(fn->getOpaqueResultTypeDecl()),
                           nameComponentsAndDependencies);

    writeGenericParams(fn->getGenericParams());

    // Write the body parameters.
    writeParameterList(fn->getParameters());

    if (auto errorConvention = fn->getForeignErrorConvention())
      writeForeignErrorConvention(*errorConvention);

    writeInlinableBodyTextIfNeeded(fn);
  }

  void visitOpaqueTypeDecl(const OpaqueTypeDecl *opaqueDecl) {
    using namespace decls_block;
    verifyAttrSerializable(opaqueDecl);

    auto namingDeclID = S.addDeclRef(opaqueDecl->getNamingDecl());
    auto contextID = S.addDeclContextRef(opaqueDecl->getDeclContext());
    auto interfaceSigID = S.addGenericSignatureRef(
        opaqueDecl->getOpaqueInterfaceGenericSignature());
    auto interfaceTypeID =
      S.addTypeRef(opaqueDecl->getUnderlyingInterfaceType());

    auto genericEnvID = S.addGenericEnvironmentRef(opaqueDecl->getGenericEnvironment());

    SubstitutionMapID underlyingTypeID = 0;
    if (auto underlying = opaqueDecl->getUnderlyingTypeSubstitutions())
      underlyingTypeID = S.addSubstitutionMapRef(*underlying);

    unsigned abbrCode = S.DeclTypeAbbrCodes[OpaqueTypeLayout::Code];
    OpaqueTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                 contextID, namingDeclID, interfaceSigID,
                                 interfaceTypeID, genericEnvID,
                                 underlyingTypeID);
    writeGenericParams(opaqueDecl->getGenericParams());
  }

  void visitAccessorDecl(const AccessorDecl *fn) {
    using namespace decls_block;
    verifyAttrSerializable(fn);

    auto contextID = S.addDeclContextRef(fn->getDeclContext());

    unsigned abbrCode = S.DeclTypeAbbrCodes[AccessorLayout::Code];

    uint8_t rawAccessLevel = getRawStableAccessLevel(fn->getFormalAccess());
    uint8_t rawAccessorKind =
      uint8_t(getStableAccessorKind(fn->getAccessorKind()));

    Type ty = fn->getInterfaceType();
    SmallVector<IdentifierID, 4> dependencies;
    for (auto dependency : collectDependenciesFromType(ty->getCanonicalType()))
      dependencies.push_back(S.addTypeRef(dependency));

    AccessorLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                               contextID,
                               fn->isImplicit(),
                               fn->isStatic(),
                               uint8_t(getStableStaticSpelling(
                                                  fn->getStaticSpelling())),
                               fn->isObjC(),
                               uint8_t(getStableSelfAccessKind(
                                                  fn->getSelfAccessKind())),
                               fn->hasForcedStaticDispatch(),
                               fn->hasThrows(),
                               S.addGenericEnvironmentRef(
                                                  fn->getGenericEnvironment()),
                               S.addTypeRef(fn->getResultInterfaceType()),
                               S.addDeclRef(fn->getOverriddenDecl()),
                               S.addDeclRef(fn->getStorage()),
                               rawAccessorKind,
                               rawAccessLevel,
                               fn->needsNewVTableEntry(),
                               dependencies);

    writeGenericParams(fn->getGenericParams());

    // Write the body parameters.
    writeParameterList(fn->getParameters());

    if (auto errorConvention = fn->getForeignErrorConvention())
      writeForeignErrorConvention(*errorConvention);

    writeInlinableBodyTextIfNeeded(fn);
  }

  void visitEnumElementDecl(const EnumElementDecl *elem) {
    using namespace decls_block;
    verifyAttrSerializable(elem);

    auto contextID = S.addDeclContextRef(elem->getDeclContext());

    SmallVector<IdentifierID, 4> nameComponentsAndDependencies;
    auto baseName = S.addDeclBaseNameRef(elem->getBaseName());
    nameComponentsAndDependencies.push_back(baseName);
    for (auto argName : elem->getFullName().getArgumentNames())
      nameComponentsAndDependencies.push_back(S.addDeclBaseNameRef(argName));

    Type ty = elem->getInterfaceType();
    for (Type dependency : collectDependenciesFromType(ty->getCanonicalType()))
      nameComponentsAndDependencies.push_back(S.addTypeRef(dependency));

    // We only serialize the raw values of @objc enums, because they're part
    // of the ABI. That isn't the case for Swift enums.
    auto RawValueKind = EnumElementRawValueKind::None;
    bool Negative = false;
    StringRef RawValueText;
    if (elem->getParentEnum()->isObjC()) {
      // Currently ObjC enums always have integer raw values.
      RawValueKind = EnumElementRawValueKind::IntegerLiteral;
      auto ILE = cast<IntegerLiteralExpr>(elem->getRawValueExpr());
      RawValueText = ILE->getDigitsText();
      Negative = ILE->isNegative();
    }

    unsigned abbrCode = S.DeclTypeAbbrCodes[EnumElementLayout::Code];
    EnumElementLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                  contextID,
                                  elem->isImplicit(),
                                  elem->hasAssociatedValues(),
                                  (unsigned)RawValueKind,
                                  Negative,
                                  S.addUniquedStringRef(RawValueText),
                                  elem->getFullName().getArgumentNames().size()+1,
                                  nameComponentsAndDependencies);
    if (auto *PL = elem->getParameterList())
      writeParameterList(PL);
  }

  void visitSubscriptDecl(const SubscriptDecl *subscript) {
    using namespace decls_block;
    verifyAttrSerializable(subscript);

    auto contextID = S.addDeclContextRef(subscript->getDeclContext());

    Accessors accessors = getAccessors(subscript);

    SmallVector<IdentifierID, 4> nameComponentsAndDependencies;
    for (auto argName : subscript->getFullName().getArgumentNames())
      nameComponentsAndDependencies.push_back(S.addDeclBaseNameRef(argName));

    for (auto accessor : accessors.Decls)
      nameComponentsAndDependencies.push_back(S.addDeclRef(accessor));

    Type ty = subscript->getInterfaceType();
    for (Type dependency : collectDependenciesFromType(ty->getCanonicalType()))
      nameComponentsAndDependencies.push_back(S.addTypeRef(dependency));

    uint8_t rawAccessLevel =
      getRawStableAccessLevel(subscript->getFormalAccess());
    uint8_t rawSetterAccessLevel = rawAccessLevel;
    if (subscript->isSettable())
      rawSetterAccessLevel =
        getRawStableAccessLevel(subscript->getSetterFormalAccess());
    uint8_t rawStaticSpelling =
      uint8_t(getStableStaticSpelling(subscript->getStaticSpelling()));

    unsigned abbrCode = S.DeclTypeAbbrCodes[SubscriptLayout::Code];
    SubscriptLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                contextID,
                                subscript->isImplicit(),
                                subscript->isObjC(),
                                subscript->isGetterMutating(),
                                subscript->isSetterMutating(),
                                accessors.OpaqueReadOwnership,
                                accessors.ReadImpl,
                                accessors.WriteImpl,
                                accessors.ReadWriteImpl,
                                accessors.Decls.size(),
                                S.addGenericEnvironmentRef(
                                            subscript->getGenericEnvironment()),
                                S.addTypeRef(subscript->getElementInterfaceType()),
                                S.addDeclRef(subscript->getOverriddenDecl()),
                                rawAccessLevel,
                                rawSetterAccessLevel,
                                rawStaticSpelling,
                                subscript->
                                  getFullName().getArgumentNames().size(),
                                S.addDeclRef(subscript->getOpaqueResultTypeDecl()),
                                nameComponentsAndDependencies);

    writeGenericParams(subscript->getGenericParams());
    writeParameterList(subscript->getIndices());
  }

  void visitConstructorDecl(const ConstructorDecl *ctor) {
    using namespace decls_block;
    verifyAttrSerializable(ctor);

    auto contextID = S.addDeclContextRef(ctor->getDeclContext());

    SmallVector<IdentifierID, 4> nameComponentsAndDependencies;
    for (auto argName : ctor->getFullName().getArgumentNames())
      nameComponentsAndDependencies.push_back(S.addDeclBaseNameRef(argName));

    Type ty = ctor->getInterfaceType();
    for (Type dependency : collectDependenciesFromType(ty->getCanonicalType()))
      nameComponentsAndDependencies.push_back(S.addTypeRef(dependency));

    uint8_t rawAccessLevel = getRawStableAccessLevel(ctor->getFormalAccess());

    bool firstTimeRequired = ctor->isRequired();
    if (auto *overridden = ctor->getOverriddenDecl())
      if (firstTimeRequired && overridden->isRequired())
        firstTimeRequired = false;

    unsigned abbrCode = S.DeclTypeAbbrCodes[ConstructorLayout::Code];
    ConstructorLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                  contextID,
                                  getRawStableOptionalTypeKind(
                                    ctor->getFailability()),
                                  ctor->isImplicit(),
                                  ctor->isObjC(),
                                  ctor->hasStubImplementation(),
                                  ctor->hasThrows(),
                                  getStableCtorInitializerKind(
                                    ctor->getInitKind()),
                                  S.addGenericEnvironmentRef(
                                                 ctor->getGenericEnvironment()),
                                  S.addDeclRef(ctor->getOverriddenDecl()),
                                  rawAccessLevel,
                                  ctor->needsNewVTableEntry(),
                                  firstTimeRequired,
                                  ctor->getFullName().getArgumentNames().size(),
                                  nameComponentsAndDependencies);

    writeGenericParams(ctor->getGenericParams());
    writeParameterList(ctor->getParameters());

    if (auto errorConvention = ctor->getForeignErrorConvention())
      writeForeignErrorConvention(*errorConvention);

    writeInlinableBodyTextIfNeeded(ctor);
  }

  void visitDestructorDecl(const DestructorDecl *dtor) {
    using namespace decls_block;
    verifyAttrSerializable(dtor);

    auto contextID = S.addDeclContextRef(dtor->getDeclContext());

    unsigned abbrCode = S.DeclTypeAbbrCodes[DestructorLayout::Code];
    DestructorLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                 contextID,
                                 dtor->isImplicit(),
                                 dtor->isObjC(),
                                 S.addGenericEnvironmentRef(
                                                dtor->getGenericEnvironment()));
    writeInlinableBodyTextIfNeeded(dtor);
  }

  void visitTopLevelCodeDecl(const TopLevelCodeDecl *) {
    // Top-level code is ignored; external clients don't need to know about it.
  }

  void visitImportDecl(const ImportDecl *) {
    llvm_unreachable("import decls should not be serialized");
  }

  void visitIfConfigDecl(const IfConfigDecl *) {
    llvm_unreachable("#if block declarations should not be serialized");
  }

  void visitPoundDiagnosticDecl(const PoundDiagnosticDecl *) {
    llvm_unreachable("#warning/#error declarations should not be serialized");
  }

  void visitEnumCaseDecl(const EnumCaseDecl *) {
    llvm_unreachable("enum case decls should not be serialized");
  }

  void visitModuleDecl(const ModuleDecl *) {
    llvm_unreachable("module decls are not serialized");
  }

  void visitMissingMemberDecl(const MissingMemberDecl *) {
    llvm_unreachable("member placeholders shouldn't be serialized");
  }
};

void Serializer::writeDecl(const Decl *D) {
  using namespace decls_block;

  PrettyStackTraceDecl trace("serializing", D);

  auto id = DeclAndTypeIDs[D];
  assert(id != 0 && "decl or type not referenced properly");
  (void)id;

  assert((id - 1) == DeclOffsets.size());
  assert((TypeOffsets.empty() || TypeOffsets.back() != Out.GetCurrentBitNo()) &&
         "encoding Decl and Type to the same offset");
  DeclOffsets.push_back(Out.GetCurrentBitNo());
  SWIFT_DEFER {
    // This is important enough to leave on in Release builds.
    if (DeclOffsets.back() == Out.GetCurrentBitNo()) {
      llvm::PrettyStackTraceString message("failed to serialize anything");
      abort();
    }
  };

  assert(!D->isInvalid() && "cannot create a module with an invalid decl");
  if (isDeclXRef(D)) {
    writeCrossReference(D);
    return;
  }

  assert(!D->hasClangNode() && "imported decls should use cross-references");

  DeclSerializer(*this, id).visit(D);
}

#define SIMPLE_CASE(TYPENAME, VALUE) \
  case swift::TYPENAME::VALUE: return uint8_t(serialization::TYPENAME::VALUE);

/// Translate from the AST function representation enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableFunctionTypeRepresentation(
                                       swift::FunctionType::Representation cc) {
  switch (cc) {
  SIMPLE_CASE(FunctionTypeRepresentation, Swift)
  SIMPLE_CASE(FunctionTypeRepresentation, Block)
  SIMPLE_CASE(FunctionTypeRepresentation, Thin)
  SIMPLE_CASE(FunctionTypeRepresentation, CFunctionPointer)
  }
  llvm_unreachable("bad calling convention");
}

/// Translate from the AST function representation enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableSILFunctionTypeRepresentation(
                                    swift::SILFunctionType::Representation cc) {
  switch (cc) {
  SIMPLE_CASE(SILFunctionTypeRepresentation, Thick)
  SIMPLE_CASE(SILFunctionTypeRepresentation, Block)
  SIMPLE_CASE(SILFunctionTypeRepresentation, Thin)
  SIMPLE_CASE(SILFunctionTypeRepresentation, CFunctionPointer)
  SIMPLE_CASE(SILFunctionTypeRepresentation, Method)
  SIMPLE_CASE(SILFunctionTypeRepresentation, ObjCMethod)
  SIMPLE_CASE(SILFunctionTypeRepresentation, WitnessMethod)
  SIMPLE_CASE(SILFunctionTypeRepresentation, Closure)
  }
  llvm_unreachable("bad calling convention");
}

/// Translate from the AST coroutine-kind enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableSILCoroutineKind(
                                    swift::SILCoroutineKind kind) {
  switch (kind) {
  SIMPLE_CASE(SILCoroutineKind, None)
  SIMPLE_CASE(SILCoroutineKind, YieldOnce)
  SIMPLE_CASE(SILCoroutineKind, YieldMany)
  }
  llvm_unreachable("bad kind");
}

/// Translate from the AST ownership enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t
getRawStableReferenceOwnership(swift::ReferenceOwnership ownership) {
  switch (ownership) {
  SIMPLE_CASE(ReferenceOwnership, Strong)
#define REF_STORAGE(Name, ...) \
  SIMPLE_CASE(ReferenceOwnership, Name)
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("bad ownership kind");
}
/// Translate from the AST ownership enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableValueOwnership(swift::ValueOwnership ownership) {
  switch (ownership) {
  SIMPLE_CASE(ValueOwnership, Default)
  SIMPLE_CASE(ValueOwnership, InOut)
  SIMPLE_CASE(ValueOwnership, Shared)
  SIMPLE_CASE(ValueOwnership, Owned)
  }
  llvm_unreachable("bad ownership kind");
}

/// Translate from the AST ParameterConvention enum to the
/// Serialization enum values, which are guaranteed to be stable.
static uint8_t getRawStableParameterConvention(swift::ParameterConvention pc) {
  switch (pc) {
  SIMPLE_CASE(ParameterConvention, Indirect_In)
  SIMPLE_CASE(ParameterConvention, Indirect_In_Constant)
  SIMPLE_CASE(ParameterConvention, Indirect_In_Guaranteed)
  SIMPLE_CASE(ParameterConvention, Indirect_Inout)
  SIMPLE_CASE(ParameterConvention, Indirect_InoutAliasable)
  SIMPLE_CASE(ParameterConvention, Direct_Owned)
  SIMPLE_CASE(ParameterConvention, Direct_Unowned)
  SIMPLE_CASE(ParameterConvention, Direct_Guaranteed)
  }
  llvm_unreachable("bad parameter convention kind");
}

/// Translate from the AST ResultConvention enum to the
/// Serialization enum values, which are guaranteed to be stable.
static uint8_t getRawStableResultConvention(swift::ResultConvention rc) {
  switch (rc) {
  SIMPLE_CASE(ResultConvention, Indirect)
  SIMPLE_CASE(ResultConvention, Owned)
  SIMPLE_CASE(ResultConvention, Unowned)
  SIMPLE_CASE(ResultConvention, UnownedInnerPointer)
  SIMPLE_CASE(ResultConvention, Autoreleased)
  }
  llvm_unreachable("bad result convention kind");
}

#undef SIMPLE_CASE

/// Find the typealias given a builtin type.
static TypeAliasDecl *findTypeAliasForBuiltin(ASTContext &Ctx, Type T) {
  /// Get the type name by chopping off "Builtin.".
  llvm::SmallString<32> FullName;
  llvm::raw_svector_ostream OS(FullName);
  T->print(OS);
  assert(FullName.startswith(BUILTIN_TYPE_NAME_PREFIX));
  StringRef TypeName = FullName.substr(8);

  SmallVector<ValueDecl*, 4> CurModuleResults;
  Ctx.TheBuiltinModule->lookupValue(ModuleDecl::AccessPathTy(),
                                    Ctx.getIdentifier(TypeName),
                                    NLKind::QualifiedLookup,
                                    CurModuleResults);
  assert(CurModuleResults.size() == 1);
  return cast<TypeAliasDecl>(CurModuleResults[0]);
}

class Serializer::TypeSerializer : public TypeVisitor<TypeSerializer> {
  Serializer &S;

public:
  explicit TypeSerializer(Serializer &S) : S(S) {}

  /// If this gets referenced, we forgot to handle a type.
  void visitType(const TypeBase *) = delete;

  void visitErrorType(const ErrorType *) {
    llvm_unreachable("should not serialize an invalid type");
  }

  void visitUnresolvedType(const UnresolvedType *) {
    llvm_unreachable("should not serialize an invalid type");
  }

  void visitModuleType(const ModuleType *) {
    llvm_unreachable("modules are currently not first-class values");
  }

  void visitInOutType(const InOutType *) {
    llvm_unreachable("inout types are only used in function type parameters");
  }

  void visitLValueType(const LValueType *) {
    llvm_unreachable("lvalue types are only used in function bodies");
  }

  void visitTypeVariableType(const TypeVariableType *) {
    llvm_unreachable("type variables should not escape the type checker");
  }

  void visitBuiltinTypeImpl(Type ty) {
    using namespace decls_block;
    TypeAliasDecl *typeAlias =
      findTypeAliasForBuiltin(S.M->getASTContext(), ty);

    unsigned abbrCode = S.DeclTypeAbbrCodes[BuiltinAliasTypeLayout::Code];
    BuiltinAliasTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                       S.addDeclRef(typeAlias,
                                                    /*allowTypeAliasXRef*/true),
                                       TypeID());
  }

  void visitBuiltinType(BuiltinType *ty) {
    visitBuiltinTypeImpl(ty);
  }

  void visitSILTokenType(SILTokenType *ty) {
    // This is serialized like a BuiltinType, even though it isn't one.
    visitBuiltinTypeImpl(ty);
  }

  void visitTypeAliasType(const TypeAliasType *alias) {
    using namespace decls_block;
    const TypeAliasDecl *typeAlias = alias->getDecl();
    auto underlyingType = typeAlias->getUnderlyingTypeLoc().getType();

    unsigned abbrCode = S.DeclTypeAbbrCodes[TypeAliasTypeLayout::Code];
    TypeAliasTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        S.addDeclRef(typeAlias, /*allowTypeAliasXRef*/true),
        S.addTypeRef(alias->getParent()),
        S.addTypeRef(underlyingType),
        S.addTypeRef(alias->getSinglyDesugaredType()),
        S.addSubstitutionMapRef(alias->getSubstitutionMap()));
  }

  template <typename Layout>
  void serializeSimpleWrapper(Type wrappedTy) {
    unsigned abbrCode = S.DeclTypeAbbrCodes[Layout::Code];
    Layout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                       S.addTypeRef(wrappedTy));
  }

  void visitParenType(const ParenType *parenTy) {
    using namespace decls_block;
    assert(parenTy->getParameterFlags().isNone());
    serializeSimpleWrapper<ParenTypeLayout>(parenTy->getUnderlyingType());
  }

  void visitTupleType(const TupleType *tupleTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[TupleTypeLayout::Code];
    TupleTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode);

    abbrCode = S.DeclTypeAbbrCodes[TupleTypeEltLayout::Code];
    for (auto &elt : tupleTy->getElements()) {
      assert(elt.getParameterFlags().isNone());
      TupleTypeEltLayout::emitRecord(
          S.Out, S.ScratchRecord, abbrCode,
          S.addDeclBaseNameRef(elt.getName()),
          S.addTypeRef(elt.getType()));
    }
  }

  void visitNominalType(const NominalType *nominalTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[NominalTypeLayout::Code];
    NominalTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                  S.addDeclRef(nominalTy->getDecl()),
                                  S.addTypeRef(nominalTy->getParent()));
  }

  template <typename Layout>
  void visitMetatypeImpl(const AnyMetatypeType *metatypeTy) {
    unsigned abbrCode = S.DeclTypeAbbrCodes[Layout::Code];

    // Map the metatype representation.
    auto repr = getRawStableMetatypeRepresentation(metatypeTy);
    Layout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                       S.addTypeRef(metatypeTy->getInstanceType()),
                       static_cast<uint8_t>(repr));
  }

  void visitExistentialMetatypeType(const ExistentialMetatypeType *metatypeTy) {
    using namespace decls_block;
    visitMetatypeImpl<ExistentialMetatypeTypeLayout>(metatypeTy);
  }

  void visitMetatypeType(const MetatypeType *metatypeTy) {
    using namespace decls_block;
    visitMetatypeImpl<MetatypeTypeLayout>(metatypeTy);
  }

  void visitDynamicSelfType(const DynamicSelfType *dynamicSelfTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[DynamicSelfTypeLayout::Code];
    DynamicSelfTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        S.addTypeRef(dynamicSelfTy->getSelfType()));
  }

  void visitPrimaryArchetypeType(const PrimaryArchetypeType *archetypeTy) {
    using namespace decls_block;
    auto env = archetypeTy->getGenericEnvironment();

    GenericEnvironmentID envID = S.addGenericEnvironmentRef(env);
    auto interfaceType = archetypeTy->getInterfaceType()
      ->castTo<GenericTypeParamType>();

    unsigned abbrCode = S.DeclTypeAbbrCodes[PrimaryArchetypeTypeLayout::Code];
    PrimaryArchetypeTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                           envID,
                                           interfaceType->getDepth(),
                                           interfaceType->getIndex());
  }

  void visitOpenedArchetypeType(const OpenedArchetypeType *archetypeTy) {
    using namespace decls_block;
    serializeSimpleWrapper<OpenedArchetypeTypeLayout>(
        archetypeTy->getOpenedExistentialType());
  }

  void
  visitOpaqueTypeArchetypeType(const OpaqueTypeArchetypeType *archetypeTy) {
    using namespace decls_block;
    auto declID = S.addDeclRef(archetypeTy->getDecl());
    auto substMapID = S.addSubstitutionMapRef(archetypeTy->getSubstitutions());
    unsigned abbrCode = S.DeclTypeAbbrCodes[OpaqueArchetypeTypeLayout::Code];
    OpaqueArchetypeTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                          declID, substMapID);
  }

  void visitNestedArchetypeType(const NestedArchetypeType *archetypeTy) {
    using namespace decls_block;
    auto rootTypeID = S.addTypeRef(archetypeTy->getRoot());
    auto interfaceTypeID = S.addTypeRef(archetypeTy->getInterfaceType());
    unsigned abbrCode = S.DeclTypeAbbrCodes[NestedArchetypeTypeLayout::Code];
    NestedArchetypeTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                          rootTypeID, interfaceTypeID);
  }

  void visitGenericTypeParamType(const GenericTypeParamType *genericParam) {
    using namespace decls_block;

    unsigned abbrCode = S.DeclTypeAbbrCodes[GenericTypeParamTypeLayout::Code];
    DeclID declIDOrDepth;
    unsigned indexPlusOne;
    if (genericParam->getDecl() &&
        !(genericParam->getDecl()->getDeclContext()->isModuleScopeContext() &&
          S.isDeclXRef(genericParam->getDecl()))) {
      declIDOrDepth = S.addDeclRef(genericParam->getDecl());
      indexPlusOne = 0;
    } else {
      declIDOrDepth = genericParam->getDepth();
      indexPlusOne = genericParam->getIndex() + 1;
    }
    GenericTypeParamTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                           declIDOrDepth, indexPlusOne);
  }

  void visitDependentMemberType(const DependentMemberType *dependent) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[DependentMemberTypeLayout::Code];
    assert(dependent->getAssocType() && "Unchecked dependent member type");
    DependentMemberTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        S.addTypeRef(dependent->getBase()),
        S.addDeclRef(dependent->getAssocType()));
  }

  void serializeFunctionTypeParams(const AnyFunctionType *fnTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[FunctionParamLayout::Code];
    for (auto &param : fnTy->getParams()) {
      auto paramFlags = param.getParameterFlags();
      auto rawOwnership =
          getRawStableValueOwnership(paramFlags.getValueOwnership());
      FunctionParamLayout::emitRecord(
          S.Out, S.ScratchRecord, abbrCode,
          S.addDeclBaseNameRef(param.getLabel()),
          S.addTypeRef(param.getPlainType()), paramFlags.isVariadic(),
          paramFlags.isAutoClosure(), rawOwnership);
    }
  }

  void visitFunctionType(const FunctionType *fnTy) {
    using namespace decls_block;

    unsigned abbrCode = S.DeclTypeAbbrCodes[FunctionTypeLayout::Code];
    FunctionTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
        S.addTypeRef(fnTy->getResult()),
        getRawStableFunctionTypeRepresentation(fnTy->getRepresentation()),
        fnTy->isNoEscape(),
        fnTy->throws());

    serializeFunctionTypeParams(fnTy);
  }

  void visitGenericFunctionType(const GenericFunctionType *fnTy) {
    using namespace decls_block;
    assert(!fnTy->isNoEscape());

    auto *genericSig = fnTy->getGenericSignature();
    unsigned abbrCode = S.DeclTypeAbbrCodes[GenericFunctionTypeLayout::Code];
    GenericFunctionTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
        S.addTypeRef(fnTy->getResult()),
        getRawStableFunctionTypeRepresentation(fnTy->getRepresentation()),
        fnTy->throws(),
        S.addGenericSignatureRef(genericSig));

    serializeFunctionTypeParams(fnTy);
  }

  void visitSILBlockStorageType(const SILBlockStorageType *storageTy) {
    using namespace decls_block;
    serializeSimpleWrapper<SILBlockStorageTypeLayout>(
        storageTy->getCaptureType());
  }

  void visitSILBoxType(const SILBoxType *boxTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[SILBoxTypeLayout::Code];
    SILLayoutID layoutRef = S.addSILLayoutRef(boxTy->getLayout());

    SILBoxTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode, layoutRef,
        S.addSubstitutionMapRef(boxTy->getSubstitutions()));
  }

  void visitSILFunctionType(const SILFunctionType *fnTy) {
    using namespace decls_block;

    auto representation = fnTy->getRepresentation();
    auto stableRepresentation =
      getRawStableSILFunctionTypeRepresentation(representation);

    SmallVector<TypeID, 8> variableData;
    for (auto param : fnTy->getParameters()) {
      variableData.push_back(S.addTypeRef(param.getType()));
      unsigned conv = getRawStableParameterConvention(param.getConvention());
      variableData.push_back(TypeID(conv));
    }
    for (auto yield : fnTy->getYields()) {
      variableData.push_back(S.addTypeRef(yield.getType()));
      unsigned conv = getRawStableParameterConvention(yield.getConvention());
      variableData.push_back(TypeID(conv));
    }
    for (auto result : fnTy->getResults()) {
      variableData.push_back(S.addTypeRef(result.getType()));
      unsigned conv = getRawStableResultConvention(result.getConvention());
      variableData.push_back(TypeID(conv));
    }
    if (fnTy->hasErrorResult()) {
      auto abResult = fnTy->getErrorResult();
      variableData.push_back(S.addTypeRef(abResult.getType()));
      unsigned conv = getRawStableResultConvention(abResult.getConvention());
      variableData.push_back(TypeID(conv));
    }

    auto sig = fnTy->getGenericSignature();

    auto stableCoroutineKind =
      getRawStableSILCoroutineKind(fnTy->getCoroutineKind());

    auto stableCalleeConvention =
      getRawStableParameterConvention(fnTy->getCalleeConvention());

    unsigned abbrCode = S.DeclTypeAbbrCodes[SILFunctionTypeLayout::Code];
    SILFunctionTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        stableCoroutineKind, stableCalleeConvention,
        stableRepresentation, fnTy->isPseudogeneric(), fnTy->isNoEscape(),
        fnTy->hasErrorResult(), fnTy->getParameters().size(),
        fnTy->getNumYields(), fnTy->getNumResults(),
        S.addGenericSignatureRef(sig), variableData);

    if (auto conformance = fnTy->getWitnessMethodConformanceOrNone())
      S.writeConformance(*conformance, S.DeclTypeAbbrCodes);
  }

  void visitArraySliceType(const ArraySliceType *sliceTy) {
    using namespace decls_block;
    serializeSimpleWrapper<ArraySliceTypeLayout>(sliceTy->getBaseType());
  }

  void visitDictionaryType(const DictionaryType *dictTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[DictionaryTypeLayout::Code];
    DictionaryTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                     S.addTypeRef(dictTy->getKeyType()),
                                     S.addTypeRef(dictTy->getValueType()));
  }

  void visitOptionalType(const OptionalType *optionalTy) {
    using namespace decls_block;
    serializeSimpleWrapper<OptionalTypeLayout>(optionalTy->getBaseType());
  }

  void
  visitProtocolCompositionType(const ProtocolCompositionType *composition) {
    using namespace decls_block;

    SmallVector<TypeID, 4> protocols;
    for (auto proto : composition->getMembers())
      protocols.push_back(S.addTypeRef(proto));

    unsigned abbrCode =
        S.DeclTypeAbbrCodes[ProtocolCompositionTypeLayout::Code];
    ProtocolCompositionTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        composition->hasExplicitAnyObject(),
        protocols);
  }

  void visitReferenceStorageType(const ReferenceStorageType *refTy) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[ReferenceStorageTypeLayout::Code];
    auto stableOwnership =
        getRawStableReferenceOwnership(refTy->getOwnership());
    ReferenceStorageTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        stableOwnership,
        S.addTypeRef(refTy->getReferentType()));
  }

  void visitUnboundGenericType(const UnboundGenericType *generic) {
    using namespace decls_block;
    unsigned abbrCode = S.DeclTypeAbbrCodes[UnboundGenericTypeLayout::Code];
    UnboundGenericTypeLayout::emitRecord(
        S.Out, S.ScratchRecord, abbrCode,
        S.addDeclRef(generic->getDecl(), /*allowTypeAliasXRef*/true),
        S.addTypeRef(generic->getParent()));
  }

  void visitBoundGenericType(const BoundGenericType *generic) {
    using namespace decls_block;
    SmallVector<TypeID, 8> genericArgIDs;

    for (auto next : generic->getGenericArgs())
      genericArgIDs.push_back(S.addTypeRef(next));

    unsigned abbrCode = S.DeclTypeAbbrCodes[BoundGenericTypeLayout::Code];
    BoundGenericTypeLayout::emitRecord(S.Out, S.ScratchRecord, abbrCode,
                                       S.addDeclRef(generic->getDecl()),
                                       S.addTypeRef(generic->getParent()),
                                       genericArgIDs);
  }
};

void Serializer::writeType(Type ty) {
  using namespace decls_block;
  PrettyStackTraceType traceRAII(ty->getASTContext(), "serializing", ty);

  auto id = DeclAndTypeIDs[ty];
  assert(id != 0 && "type not referenced properly");
  (void)id;

  assert((id - 1) == TypeOffsets.size());
  assert((DeclOffsets.empty() || DeclOffsets.back() != Out.GetCurrentBitNo()) &&
         "encoding Decl and Type to the same offset");
  TypeOffsets.push_back(Out.GetCurrentBitNo());
  SWIFT_DEFER {
    // This is important enough to leave on in Release builds.
    if (TypeOffsets.back() == Out.GetCurrentBitNo()) {
      llvm::PrettyStackTraceString message("failed to serialize anything");
      abort();
    }
  };

  TypeSerializer(*this).visit(ty);
}

void Serializer::writeAllDeclsAndTypes() {
  BCBlockRAII restoreBlock(Out, DECLS_AND_TYPES_BLOCK_ID, 8);
  using namespace decls_block;
  registerDeclTypeAbbr<BuiltinAliasTypeLayout>();
  registerDeclTypeAbbr<TypeAliasTypeLayout>();
  registerDeclTypeAbbr<GenericTypeParamDeclLayout>();
  registerDeclTypeAbbr<AssociatedTypeDeclLayout>();
  registerDeclTypeAbbr<NominalTypeLayout>();
  registerDeclTypeAbbr<ParenTypeLayout>();
  registerDeclTypeAbbr<TupleTypeLayout>();
  registerDeclTypeAbbr<TupleTypeEltLayout>();
  registerDeclTypeAbbr<FunctionTypeLayout>();
  registerDeclTypeAbbr<FunctionParamLayout>();
  registerDeclTypeAbbr<MetatypeTypeLayout>();
  registerDeclTypeAbbr<ExistentialMetatypeTypeLayout>();
  registerDeclTypeAbbr<PrimaryArchetypeTypeLayout>();
  registerDeclTypeAbbr<OpenedArchetypeTypeLayout>();
  registerDeclTypeAbbr<OpaqueArchetypeTypeLayout>();
  registerDeclTypeAbbr<NestedArchetypeTypeLayout>();
  registerDeclTypeAbbr<ProtocolCompositionTypeLayout>();
  registerDeclTypeAbbr<BoundGenericTypeLayout>();
  registerDeclTypeAbbr<GenericFunctionTypeLayout>();
  registerDeclTypeAbbr<SILBlockStorageTypeLayout>();
  registerDeclTypeAbbr<SILBoxTypeLayout>();
  registerDeclTypeAbbr<SILFunctionTypeLayout>();
  registerDeclTypeAbbr<ArraySliceTypeLayout>();
  registerDeclTypeAbbr<DictionaryTypeLayout>();
  registerDeclTypeAbbr<ReferenceStorageTypeLayout>();
  registerDeclTypeAbbr<UnboundGenericTypeLayout>();
  registerDeclTypeAbbr<OptionalTypeLayout>();
  registerDeclTypeAbbr<DynamicSelfTypeLayout>();

  registerDeclTypeAbbr<TypeAliasLayout>();
  registerDeclTypeAbbr<GenericTypeParamTypeLayout>();
  registerDeclTypeAbbr<DependentMemberTypeLayout>();
  registerDeclTypeAbbr<StructLayout>();
  registerDeclTypeAbbr<ConstructorLayout>();
  registerDeclTypeAbbr<VarLayout>();
  registerDeclTypeAbbr<ParamLayout>();
  registerDeclTypeAbbr<FuncLayout>();
  registerDeclTypeAbbr<AccessorLayout>();
  registerDeclTypeAbbr<OpaqueTypeLayout>();
  registerDeclTypeAbbr<PatternBindingLayout>();
  registerDeclTypeAbbr<ProtocolLayout>();
  registerDeclTypeAbbr<DefaultWitnessTableLayout>();
  registerDeclTypeAbbr<PrefixOperatorLayout>();
  registerDeclTypeAbbr<PostfixOperatorLayout>();
  registerDeclTypeAbbr<InfixOperatorLayout>();
  registerDeclTypeAbbr<PrecedenceGroupLayout>();
  registerDeclTypeAbbr<ClassLayout>();
  registerDeclTypeAbbr<EnumLayout>();
  registerDeclTypeAbbr<EnumElementLayout>();
  registerDeclTypeAbbr<SubscriptLayout>();
  registerDeclTypeAbbr<ExtensionLayout>();
  registerDeclTypeAbbr<DestructorLayout>();

  registerDeclTypeAbbr<ParameterListLayout>();

  registerDeclTypeAbbr<ParenPatternLayout>();
  registerDeclTypeAbbr<TuplePatternLayout>();
  registerDeclTypeAbbr<TuplePatternEltLayout>();
  registerDeclTypeAbbr<NamedPatternLayout>();
  registerDeclTypeAbbr<VarPatternLayout>();
  registerDeclTypeAbbr<AnyPatternLayout>();
  registerDeclTypeAbbr<TypedPatternLayout>();
  registerDeclTypeAbbr<InlinableBodyTextLayout>();
  registerDeclTypeAbbr<GenericParamListLayout>();
  registerDeclTypeAbbr<GenericSignatureLayout>();
  registerDeclTypeAbbr<GenericRequirementLayout>();
  registerDeclTypeAbbr<LayoutRequirementLayout>();
  registerDeclTypeAbbr<SILGenericEnvironmentLayout>();
  registerDeclTypeAbbr<SubstitutionMapLayout>();

  registerDeclTypeAbbr<ForeignErrorConventionLayout>();
  registerDeclTypeAbbr<DeclContextLayout>();
  registerDeclTypeAbbr<AbstractClosureExprLayout>();
  registerDeclTypeAbbr<PatternBindingInitializerLayout>();
  registerDeclTypeAbbr<DefaultArgumentInitializerLayout>();
  registerDeclTypeAbbr<TopLevelCodeDeclContextLayout>();

  registerDeclTypeAbbr<XRefTypePathPieceLayout>();
  registerDeclTypeAbbr<XRefOpaqueReturnTypePathPieceLayout>();
  registerDeclTypeAbbr<XRefValuePathPieceLayout>();
  registerDeclTypeAbbr<XRefExtensionPathPieceLayout>();
  registerDeclTypeAbbr<XRefOperatorOrAccessorPathPieceLayout>();
  registerDeclTypeAbbr<XRefGenericParamPathPieceLayout>();
  registerDeclTypeAbbr<XRefInitializerPathPieceLayout>();

  registerDeclTypeAbbr<AbstractProtocolConformanceLayout>();
  registerDeclTypeAbbr<NormalProtocolConformanceLayout>();
  registerDeclTypeAbbr<SelfProtocolConformanceLayout>();
  registerDeclTypeAbbr<SpecializedProtocolConformanceLayout>();
  registerDeclTypeAbbr<InheritedProtocolConformanceLayout>();
  registerDeclTypeAbbr<InvalidProtocolConformanceLayout>();
  registerDeclTypeAbbr<NormalProtocolConformanceIdLayout>();
  registerDeclTypeAbbr<ProtocolConformanceXrefLayout>();

  registerDeclTypeAbbr<SILLayoutLayout>();

  registerDeclTypeAbbr<LocalDiscriminatorLayout>();
  registerDeclTypeAbbr<PrivateDiscriminatorLayout>();
  registerDeclTypeAbbr<FilenameForPrivateLayout>();
  registerDeclTypeAbbr<MembersLayout>();
  registerDeclTypeAbbr<XRefLayout>();

#define DECL_ATTR(X, NAME, ...) \
  registerDeclTypeAbbr<NAME##DeclAttrLayout>();
#include "swift/AST/Attr.def"

  do {
    // Each of these loops can trigger the others to execute again, so repeat
    // until /all/ of the pending lists are empty.
    while (!DeclsAndTypesToWrite.empty()) {
      auto next = DeclsAndTypesToWrite.front();
      DeclsAndTypesToWrite.pop();

      if (next.isDecl())
        writeDecl(next.getDecl());
      else
        writeType(next.getType());
    }

    while (!LocalDeclContextsToWrite.empty()) {
      auto next = LocalDeclContextsToWrite.front();
      LocalDeclContextsToWrite.pop();
      writeLocalDeclContext(next);
    }

    while (!DeclContextsToWrite.empty()) {
      auto next = DeclContextsToWrite.front();
      DeclContextsToWrite.pop();
      writeDeclContext(next);
    }

    while (!GenericSignaturesToWrite.empty()) {
      auto next = GenericSignaturesToWrite.front();
      GenericSignaturesToWrite.pop();
      writeGenericSignature(next);
    }

    while (!GenericEnvironmentsToWrite.empty()) {
      auto next = GenericEnvironmentsToWrite.front();
      GenericEnvironmentsToWrite.pop();
      writeGenericEnvironment(next);
    }

    while (!SubstitutionMapsToWrite.empty()) {
      auto next = SubstitutionMapsToWrite.front();
      SubstitutionMapsToWrite.pop();
      writeSubstitutionMap(next);
    }

    while (!NormalConformancesToWrite.empty()) {
      auto next = NormalConformancesToWrite.front();
      NormalConformancesToWrite.pop();
      writeNormalConformance(next);
    }
    
    while (!SILLayoutsToWrite.empty()) {
      auto next = SILLayoutsToWrite.front();
      SILLayoutsToWrite.pop();
      writeSILLayout(next);
    }
  } while (!DeclsAndTypesToWrite.empty() ||
           !LocalDeclContextsToWrite.empty() ||
           !DeclContextsToWrite.empty() ||
           !SILLayoutsToWrite.empty() ||
           !GenericSignaturesToWrite.empty() ||
           !GenericEnvironmentsToWrite.empty() ||
           !SubstitutionMapsToWrite.empty() ||
           !NormalConformancesToWrite.empty());
}

void Serializer::writeAllIdentifiers() {
  BCBlockRAII restoreBlock(Out, IDENTIFIER_DATA_BLOCK_ID, 3);
  identifier_block::IdentifierDataLayout IdentifierData(Out);

  llvm::SmallString<4096> stringData;

  // Make sure no identifier has an offset of 0.
  stringData.push_back('\0');

  for (StringRef str : StringsToWrite) {
    IdentifierOffsets.push_back(stringData.size());
    stringData.append(str);
    stringData.push_back('\0');
  }

  IdentifierData.emit(ScratchRecord, stringData.str());
}

void Serializer::writeOffsets(const index_block::OffsetsLayout &Offsets,
                              const std::vector<BitOffset> &values) {
  Offsets.emit(ScratchRecord, getOffsetRecordCode(values), values);
}

/// Writes an in-memory decl table to an on-disk representation, using the
/// given layout.
static void writeDeclTable(const index_block::DeclListLayout &DeclList,
                           index_block::RecordKind kind,
                           const Serializer::DeclTable &table) {
  if (table.empty())
    return;

  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::OnDiskChainedHashTableGenerator<DeclTableInfo> generator;
    for (auto &entry : table)
      generator.insert(entry.first, entry.second);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream);
  }

  DeclList.emit(scratch, kind, tableOffset, hashTableBlob);
}

static void
writeExtensionTable(const index_block::ExtensionTableLayout &ExtensionTable,
                    const Serializer::ExtensionTable &table,
                    Serializer &serializer) {
  if (table.empty())
    return;

  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::OnDiskChainedHashTableGenerator<ExtensionTableInfo> generator;
    ExtensionTableInfo info{serializer};
    for (auto &entry : table) {
      generator.insert(entry.first, entry.second, info);
    }

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream, info);
  }

  ExtensionTable.emit(scratch, tableOffset, hashTableBlob);
}

static void writeLocalDeclTable(const index_block::DeclListLayout &DeclList,
                                index_block::RecordKind kind,
                                LocalTypeHashTableGenerator &generator) {
  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream);
  }

  DeclList.emit(scratch, kind, tableOffset, hashTableBlob);
}

static void
writeNestedTypeDeclsTable(const index_block::NestedTypeDeclsLayout &declList,
                          const Serializer::NestedTypeDeclsTable &table) {
  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::OnDiskChainedHashTableGenerator<NestedTypeDeclsTableInfo> generator;
    for (auto &entry : table)
      generator.insert(entry.first, entry.second);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream);
  }

  declList.emit(scratch, tableOffset, hashTableBlob);
}

static void
writeDeclMemberNamesTable(const index_block::DeclMemberNamesLayout &declNames,
                          const Serializer::DeclMemberNamesTable &table) {
  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::OnDiskChainedHashTableGenerator<DeclMemberNamesTableInfo> generator;
    // Emit the offsets of the sub-tables; the tables themselves have been
    // separately emitted into DECL_MEMBER_TABLES_BLOCK by now.
    for (auto &entry : table) {
      // Or they _should_ have been; check for nonzero offsets.
      assert(static_cast<unsigned>(entry.second.first) != 0);
      generator.insert(entry.first, entry.second.first);
    }

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream);
  }

  declNames.emit(scratch, tableOffset, hashTableBlob);
}

static void
writeDeclMembersTable(const decl_member_tables_block::DeclMembersLayout &mems,
                      const Serializer::DeclMembersTable &table) {
  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::OnDiskChainedHashTableGenerator<DeclMembersTableInfo> generator;
    for (auto &entry : table)
      generator.insert(entry.first, entry.second);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream);
  }

  mems.emit(scratch, tableOffset, hashTableBlob);
}

namespace {
  /// Used to serialize the on-disk Objective-C method hash table.
  class ObjCMethodTableInfo {
  public:
    using key_type = ObjCSelector;
    using key_type_ref = key_type;
    using data_type = Serializer::ObjCMethodTableData;
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    hash_value_type ComputeHash(key_type_ref key) {
      llvm::SmallString<32> scratch;
      // FIXME: DJB seed=0, audit whether the default seed could be used.
      return llvm::djbHash(key.getString(scratch), 0);
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      llvm::SmallString<32> scratch;
      auto keyLength = key.getString(scratch).size();
      assert(keyLength <= std::numeric_limits<uint16_t>::max() &&
             "selector too long");
      uint32_t dataLength = 0;
      for (const auto &entry : data) {
        dataLength += sizeof(uint32_t) + 1 + sizeof(uint32_t);
        dataLength += std::get<0>(entry).size();
      }

      endian::Writer writer(out, little);
      writer.write<uint16_t>(keyLength);
      writer.write<uint32_t>(dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
#ifndef NDEBUG
      uint64_t start = out.tell();
#endif
      out << key;
      assert((out.tell() - start == len) && "measured key length incorrectly");
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(declIDFitsIn32Bits(), "DeclID too large");
      endian::Writer writer(out, little);
      for (const auto &entry : data) {
        writer.write<uint32_t>(std::get<0>(entry).size());
        writer.write<uint8_t>(std::get<1>(entry));
        writer.write<uint32_t>(std::get<2>(entry));
        out.write(std::get<0>(entry).c_str(), std::get<0>(entry).size());
      }
    }
  };
} // end anonymous namespace

static void writeObjCMethodTable(const index_block::ObjCMethodTableLayout &out,
                                 Serializer::ObjCMethodTable &objcMethods) {
  // Collect all of the Objective-C selectors in the method table.
  std::vector<ObjCSelector> selectors;
  for (const auto &entry : objcMethods) {
    selectors.push_back(entry.first);
  }

  // Sort the Objective-C selectors so we emit them in a stable order.
  llvm::array_pod_sort(selectors.begin(), selectors.end());

  // Create the on-disk hash table.
  llvm::OnDiskChainedHashTableGenerator<ObjCMethodTableInfo> generator;
  llvm::SmallString<32> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::raw_svector_ostream blobStream(hashTableBlob);
    for (auto selector : selectors) {
      generator.insert(selector, objcMethods[selector]);
    }

    // Make sure that no bucket is at offset 0
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream);
  }

  SmallVector<uint64_t, 8> scratch;
  out.emit(scratch, tableOffset, hashTableBlob);
}

/// Recursively walks the members and derived global decls of any nominal types
/// to build up global tables.
template<typename Range>
static void collectInterestingNestedDeclarations(
    Serializer &S,
    Range members,
    Serializer::DeclTable &operatorMethodDecls,
    Serializer::ObjCMethodTable &objcMethods,
    Serializer::NestedTypeDeclsTable &nestedTypeDecls,
    bool isLocal = false) {
  const NominalTypeDecl *nominalParent = nullptr;

  for (const Decl *member : members) {
    // If there is a corresponding Objective-C method, record it.
    auto recordObjCMethod = [&] {
      if (isLocal)
        return;

      if (auto func = dyn_cast<AbstractFunctionDecl>(member)) {
        if (func->isObjC()) {
          if (auto owningClass = func->getDeclContext()->getSelfClassDecl()) {
            Mangle::ASTMangler mangler;
            std::string ownerName = mangler.mangleNominalType(owningClass);
            assert(!ownerName.empty() && "Mangled type came back empty!");

            objcMethods[func->getObjCSelector()].push_back(
              std::make_tuple(ownerName,
                              func->isObjCInstanceMethod(),
                              S.addDeclRef(func)));
          }
        }
      }
    };

    if (auto memberValue = dyn_cast<ValueDecl>(member)) {
      if (!memberValue->hasName()) {
        recordObjCMethod();
        continue;
      }

      if (memberValue->isOperator()) {
        // Add operator methods.
        // Note that we don't have to add operators that are already in the
        // top-level list.
        operatorMethodDecls[memberValue->getBaseName()].push_back({
          /*ignored*/0,
          S.addDeclRef(memberValue)
        });
      }
    }

    if (auto nestedType = dyn_cast<TypeDecl>(member)) {
      if (nestedType->getEffectiveAccess() > swift::AccessLevel::FilePrivate) {
        if (!nominalParent) {
          const DeclContext *DC = member->getDeclContext();
          nominalParent = DC->getSelfNominalTypeDecl();
          assert(nominalParent && "parent context is not a type or extension");
        }
        nestedTypeDecls[nestedType->getName()].push_back({
          S.addDeclRef(nominalParent),
          S.addDeclRef(nestedType)
        });
      }
    }

    // Recurse into nested declarations.
    if (auto iterable = dyn_cast<IterableDeclContext>(member)) {
      collectInterestingNestedDeclarations(S, iterable->getMembers(),
                                           operatorMethodDecls,
                                           objcMethods, nestedTypeDecls,
                                           isLocal);
    }

    // Record Objective-C methods.
    recordObjCMethod();
  }
}

void Serializer::writeAST(ModuleOrSourceFile DC,
                          bool enableNestedTypeLookupTable) {
  DeclTable topLevelDecls, operatorDecls, operatorMethodDecls;
  DeclTable precedenceGroupDecls;
  ObjCMethodTable objcMethods;
  NestedTypeDeclsTable nestedTypeDecls;
  LocalTypeHashTableGenerator localTypeGenerator, opaqueReturnTypeGenerator;
  ExtensionTable extensionDecls;
  bool hasLocalTypes = false;
  bool hasOpaqueReturnTypes = false;

  Optional<DeclID> entryPointClassID;
  SmallVector<DeclID, 16> orderedTopLevelDecls;

  ArrayRef<const FileUnit *> files;
  SmallVector<const FileUnit *, 1> Scratch;
  if (SF) {
    Scratch.push_back(SF);
    files = llvm::makeArrayRef(Scratch);
  } else {
    files = M->getFiles();
  }
  for (auto nextFile : files) {
    if (nextFile->hasEntryPoint())
      entryPointClassID = addDeclRef(nextFile->getMainClass());

    // FIXME: Switch to a visitor interface?
    SmallVector<Decl *, 32> fileDecls;
    nextFile->getTopLevelDecls(fileDecls);

    for (auto D : fileDecls) {
      if (isa<ImportDecl>(D) || isa<IfConfigDecl>(D) ||
          isa<PoundDiagnosticDecl>(D) || isa<TopLevelCodeDecl>(D)) {
        continue;
      }

      if (auto VD = dyn_cast<ValueDecl>(D)) {
        if (!VD->hasName())
          continue;
        topLevelDecls[VD->getBaseName()]
          .push_back({ getKindForTable(D), addDeclRef(D) });
      } else if (auto ED = dyn_cast<ExtensionDecl>(D)) {
        const NominalTypeDecl *extendedNominal = ED->getExtendedNominal();
        extensionDecls[extendedNominal->getName()]
          .push_back({ extendedNominal, addDeclRef(D) });
      } else if (auto OD = dyn_cast<OperatorDecl>(D)) {
        operatorDecls[OD->getName()]
          .push_back({ getStableFixity(OD->getKind()), addDeclRef(D) });
      } else if (auto PGD = dyn_cast<PrecedenceGroupDecl>(D)) {
        precedenceGroupDecls[PGD->getName()]
          .push_back({ decls_block::PRECEDENCE_GROUP_DECL, addDeclRef(D) });
      } else if (isa<PatternBindingDecl>(D)) {
        // No special handling needed.
      } else {
        llvm_unreachable("all top-level declaration kinds accounted for");
      }

      orderedTopLevelDecls.push_back(addDeclRef(D));

      // If this is a global variable, force the accessors to be
      // serialized.
      if (auto VD = dyn_cast<VarDecl>(D)) {
        if (VD->getGetter())
          addDeclRef(VD->getGetter());
        if (VD->getSetter())
          addDeclRef(VD->getSetter());
      }

      // If this nominal type has associated top-level decls for a
      // derived conformance (for example, ==), force them to be
      // serialized.
      if (auto IDC = dyn_cast<IterableDeclContext>(D)) {
        collectInterestingNestedDeclarations(*this, IDC->getMembers(),
                                             operatorMethodDecls, objcMethods,
                                             nestedTypeDecls);
      }
    }

    SmallVector<TypeDecl *, 16> localTypeDecls;
    nextFile->getLocalTypeDecls(localTypeDecls);
    SmallVector<OpaqueTypeDecl *, 16> opaqueReturnTypeDecls;
    nextFile->getOpaqueReturnTypeDecls(opaqueReturnTypeDecls);

    for (auto TD : localTypeDecls) {
      hasLocalTypes = true;
      Mangle::ASTMangler Mangler;
      std::string MangledName =
          evaluateOrDefault(M->getASTContext().evaluator,
                            MangleLocalTypeDeclRequest { TD },
                            std::string());
      assert(!MangledName.empty() && "Mangled type came back empty!");
      localTypeGenerator.insert(MangledName, addDeclRef(TD));

      if (auto IDC = dyn_cast<IterableDeclContext>(TD)) {
        collectInterestingNestedDeclarations(*this, IDC->getMembers(),
                                             operatorMethodDecls, objcMethods,
                                             nestedTypeDecls, /*isLocal=*/true);
      }
    }
    
    for (auto OTD : opaqueReturnTypeDecls) {
      hasOpaqueReturnTypes = true;
      Mangle::ASTMangler Mangler;
      auto MangledName = Mangler.mangleDeclAsUSR(OTD->getNamingDecl(),
                                                 MANGLING_PREFIX_STR);
      opaqueReturnTypeGenerator.insert(MangledName, addDeclRef(OTD));
    }
  }

  writeAllDeclsAndTypes();
  writeAllIdentifiers();

  {
    BCBlockRAII restoreBlock(Out, INDEX_BLOCK_ID, 4);

    index_block::OffsetsLayout Offsets(Out);
    writeOffsets(Offsets, DeclOffsets);
    writeOffsets(Offsets, TypeOffsets);
    writeOffsets(Offsets, IdentifierOffsets);
    writeOffsets(Offsets, DeclContextOffsets);
    writeOffsets(Offsets, LocalDeclContextOffsets);
    writeOffsets(Offsets, GenericSignatureOffsets);
    writeOffsets(Offsets, GenericEnvironmentOffsets);
    writeOffsets(Offsets, SubstitutionMapOffsets);
    writeOffsets(Offsets, NormalConformanceOffsets);
    writeOffsets(Offsets, SILLayoutOffsets);

    index_block::DeclListLayout DeclList(Out);
    writeDeclTable(DeclList, index_block::TOP_LEVEL_DECLS, topLevelDecls);
    writeDeclTable(DeclList, index_block::OPERATORS, operatorDecls);
    writeDeclTable(DeclList, index_block::PRECEDENCE_GROUPS, precedenceGroupDecls);
    writeDeclTable(DeclList, index_block::CLASS_MEMBERS_FOR_DYNAMIC_LOOKUP,
                   ClassMembersForDynamicLookup);
    writeDeclTable(DeclList, index_block::OPERATOR_METHODS, operatorMethodDecls);
    if (hasLocalTypes)
      writeLocalDeclTable(DeclList, index_block::LOCAL_TYPE_DECLS,
                          localTypeGenerator);
    if (hasOpaqueReturnTypes)
      writeLocalDeclTable(DeclList, index_block::OPAQUE_RETURN_TYPE_DECLS,
                          opaqueReturnTypeGenerator);

    if (!extensionDecls.empty()) {
      index_block::ExtensionTableLayout ExtensionTable(Out);
      writeExtensionTable(ExtensionTable, extensionDecls, *this);
    }

    index_block::OrderedDeclsLayout OrderedDecls(Out);
    OrderedDecls.emit(ScratchRecord, index_block::ORDERED_TOP_LEVEL_DECLS,
                      orderedTopLevelDecls);

    index_block::ObjCMethodTableLayout ObjCMethodTable(Out);
    writeObjCMethodTable(ObjCMethodTable, objcMethods);

    if (enableNestedTypeLookupTable &&
        !nestedTypeDecls.empty()) {
      index_block::NestedTypeDeclsLayout NestedTypeDeclsTable(Out);
      writeNestedTypeDeclsTable(NestedTypeDeclsTable, nestedTypeDecls);
    }

    if (entryPointClassID.hasValue()) {
      index_block::EntryPointLayout EntryPoint(Out);
      EntryPoint.emit(ScratchRecord, entryPointClassID.getValue());
    }

    {
      // Write sub-tables to a skippable sub-block.
      BCBlockRAII restoreBlock(Out, DECL_MEMBER_TABLES_BLOCK_ID, 4);
      decl_member_tables_block::DeclMembersLayout DeclMembersTable(Out);
      for (auto &entry : DeclMemberNames) {
        // Save BitOffset we're writing sub-table to.
        static_assert(bitOffsetFitsIn32Bits(), "BitOffset too large");
        assert(Out.GetCurrentBitNo() < (1ull << 32));
        entry.second.first = Out.GetCurrentBitNo();
        // Write sub-table.
        writeDeclMembersTable(DeclMembersTable, *entry.second.second);
      }
    }
    // Write top-level table mapping names to sub-tables.
    index_block::DeclMemberNamesLayout DeclMemberNamesTable(Out);
    writeDeclMemberNamesTable(DeclMemberNamesTable, DeclMemberNames);
  }
}

void SerializerBase::writeToStream(raw_ostream &os) {
  os.write(Buffer.data(), Buffer.size());
  os.flush();
}

SerializerBase::SerializerBase(ArrayRef<unsigned char> signature,
                               ModuleOrSourceFile DC) {
  for (unsigned char byte : signature)
    Out.Emit(byte, 8);

  this->M = getModule(DC);
  this->SF = DC.dyn_cast<SourceFile *>();
}

void Serializer::writeToStream(raw_ostream &os, ModuleOrSourceFile DC,
                               const SILModule *SILMod,
                               const SerializationOptions &options) {
  Serializer S{SWIFTMODULE_SIGNATURE, DC};

  // FIXME: This is only really needed for debugging. We don't actually use it.
  S.writeBlockInfoBlock();

  {
    BCBlockRAII moduleBlock(S.Out, MODULE_BLOCK_ID, 2);
    S.writeHeader(options);
    S.writeInputBlock(options);
    S.writeSIL(SILMod, options.SerializeAllSIL);
    S.writeAST(DC, options.EnableNestedTypeLookupTable);
  }

  S.writeToStream(os);
}

void swift::serializeToBuffers(
  ModuleOrSourceFile DC, const SerializationOptions &options,
  std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
  std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
  const SILModule *M) {

  assert(options.OutputPath && options.OutputPath[0] != '\0');
  {
    SharedTimer timer("Serialization, swiftmodule, to buffer");
    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream stream(buf);
    Serializer::writeToStream(stream, DC, M, options);
    bool hadError = withOutputFile(getContext(DC).Diags,
                                   options.OutputPath,
                                   [&](raw_ostream &out) {
      out << stream.str();
      return false;
    });
    if (hadError)
      return;
    if (moduleBuffer)
      *moduleBuffer = llvm::make_unique<llvm::SmallVectorMemoryBuffer>(
                        std::move(buf), options.OutputPath);
  }

  if (options.DocOutputPath && options.DocOutputPath[0] != '\0') {
    SharedTimer timer("Serialization, swiftdoc, to buffer");
    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream stream(buf);
    writeDocToStream(stream, DC, options.GroupInfoPath);
    (void)withOutputFile(getContext(DC).Diags,
                         options.DocOutputPath,
                         [&](raw_ostream &out) {
      out << stream.str();
      return false;
    });
    if (moduleDocBuffer)
      *moduleDocBuffer = llvm::make_unique<llvm::SmallVectorMemoryBuffer>(
                           std::move(buf), options.DocOutputPath);
  }
}

void swift::serialize(ModuleOrSourceFile DC,
                      const SerializationOptions &options,
                      const SILModule *M) {
  assert(options.OutputPath && options.OutputPath[0] != '\0');

  if (strcmp("-", options.OutputPath) == 0) {
    // Special-case writing to stdout.
    Serializer::writeToStream(llvm::outs(), DC, M, options);
    assert(!options.DocOutputPath || options.DocOutputPath[0] == '\0');
    return;
  }

  bool hadError = withOutputFile(getContext(DC).Diags,
                                 options.OutputPath,
                                 [&](raw_ostream &out) {
    SharedTimer timer("Serialization, swiftmodule");
    Serializer::writeToStream(out, DC, M, options);
    return false;
  });
  if (hadError)
    return;

  if (options.DocOutputPath && options.DocOutputPath[0] != '\0') {
    (void)withOutputFile(getContext(DC).Diags,
                         options.DocOutputPath,
                         [&](raw_ostream &out) {
      SharedTimer timer("Serialization, swiftdoc");
      writeDocToStream(out, DC, options.GroupInfoPath);
      return false;
    });
  }
}
