//===--- ASTDumper.cpp - Swift Language AST Dumper ------------------------===//
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
//
//  This file implements dumping for the Swift ASTs.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/QuotedString.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

struct TerminalColor {
  llvm::raw_ostream::Colors Color;
  bool Bold;
};

#define DEF_COLOR(NAME, COLOR, BOLD) \
static const TerminalColor NAME##Color = { llvm::raw_ostream::COLOR, BOLD };

DEF_COLOR(Func, YELLOW, false)
DEF_COLOR(Range, YELLOW, false)
DEF_COLOR(AccessLevel, YELLOW, false)
DEF_COLOR(ASTNode, YELLOW, true)
DEF_COLOR(Parameter, YELLOW, false)
DEF_COLOR(Extension, MAGENTA, false)
DEF_COLOR(Pattern, RED, true)
DEF_COLOR(Override, RED, false)
DEF_COLOR(Stmt, RED, true)
DEF_COLOR(Captures, RED, false)
DEF_COLOR(Arguments, RED, false)
DEF_COLOR(TypeRepr, GREEN, false)
DEF_COLOR(LiteralValue, GREEN, false)
DEF_COLOR(Decl, GREEN, true)
DEF_COLOR(Parenthesis, BLUE, false)
DEF_COLOR(Type, BLUE, false)
DEF_COLOR(Discriminator, BLUE, false)
DEF_COLOR(InterfaceType, GREEN, false)
DEF_COLOR(Identifier, GREEN, false)
DEF_COLOR(Expr, MAGENTA, true)
DEF_COLOR(ExprModifier, CYAN, false)
DEF_COLOR(DeclModifier, CYAN, false)
DEF_COLOR(ClosureModifier, CYAN, false)
DEF_COLOR(TypeField, CYAN, false)
DEF_COLOR(Location, CYAN, false)

#undef DEF_COLOR

namespace {
  /// RAII object that prints with the given color, if color is supported on the
  /// given stream.
  class PrintWithColorRAII {
    raw_ostream &OS;
    bool ShowColors;

  public:
    PrintWithColorRAII(raw_ostream &os, TerminalColor color)
    : OS(os), ShowColors(false)
    {
      ShowColors = os.has_colors();

      if (ShowColors)
        OS.changeColor(color.Color, color.Bold);
    }

    ~PrintWithColorRAII() {
      if (ShowColors) {
        OS.resetColor();
      }
    }

    raw_ostream &getOS() const { return OS; }

    template<typename T>
    friend raw_ostream &operator<<(PrintWithColorRAII &&printer,
                                   const T &value){
      printer.OS << value;
      return printer.OS;
    }

  };
} // end anonymous namespace

//===----------------------------------------------------------------------===//
//  Generic param list printing.
//===----------------------------------------------------------------------===//

void RequirementRepr::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void RequirementRepr::printImpl(ASTPrinter &out, bool AsWritten) const {
  auto printTy = [&](const TypeLoc &TyLoc) {
    if (AsWritten && TyLoc.getTypeRepr()) {
      TyLoc.getTypeRepr()->print(out, PrintOptions());
    } else {
      TyLoc.getType().print(out, PrintOptions());
    }
  };

  auto printLayoutConstraint =
      [&](const LayoutConstraintLoc &LayoutConstraintLoc) {
        LayoutConstraintLoc.getLayoutConstraint()->print(out, PrintOptions());
      };

  switch (getKind()) {
  case RequirementReprKind::LayoutConstraint:
    printTy(getSubjectLoc());
    out << " : ";
    printLayoutConstraint(getLayoutConstraintLoc());
    break;

  case RequirementReprKind::TypeConstraint:
    printTy(getSubjectLoc());
    out << " : ";
    printTy(getConstraintLoc());
    break;

  case RequirementReprKind::SameType:
    printTy(getFirstTypeLoc());
    out << " == ";
    printTy(getSecondTypeLoc());
    break;
  }
}

void RequirementRepr::print(raw_ostream &out) const {
  StreamPrinter printer(out);
  printImpl(printer, /*AsWritten=*/true);
}
void RequirementRepr::print(ASTPrinter &out) const {
  printImpl(out, /*AsWritten=*/true);
}

void GenericParamList::print(llvm::raw_ostream &OS) {
  OS << '<';
  interleave(*this,
             [&](const GenericTypeParamDecl *P) {
               OS << P->getName();
               if (!P->getInherited().empty()) {
                 OS << " : ";
                 P->getInherited()[0].getType().print(OS);
               }
             },
             [&] { OS << ", "; });

  if (!getRequirements().empty()) {
    OS << " where ";
    interleave(getRequirements(),
               [&](const RequirementRepr &req) {
                 req.print(OS);
               },
               [&] { OS << ", "; });
  }
  OS << '>';
}

void GenericParamList::dump() {
  print(llvm::errs());
  llvm::errs() << '\n';
}

static void printGenericParameters(raw_ostream &OS, GenericParamList *Params) {
  if (!Params)
    return;
  OS << ' ';
  Params->print(OS);
}


static StringRef
getSILFunctionTypeRepresentationString(SILFunctionType::Representation value) {
  switch (value) {
  case SILFunctionType::Representation::Thick: return "thick";
  case SILFunctionType::Representation::Block: return "block";
  case SILFunctionType::Representation::CFunctionPointer: return "c";
  case SILFunctionType::Representation::Thin: return "thin";
  case SILFunctionType::Representation::Method: return "method";
  case SILFunctionType::Representation::ObjCMethod: return "objc_method";
  case SILFunctionType::Representation::WitnessMethod: return "witness_method";
  case SILFunctionType::Representation::Closure: return "closure";
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

StringRef swift::getReadImplKindName(ReadImplKind kind) {
  switch (kind) {
  case ReadImplKind::Stored:
    return "stored";
  case ReadImplKind::Inherited:
    return "inherited";
  case ReadImplKind::Get:
    return "getter";
  case ReadImplKind::Address:
    return "addressor";
  case ReadImplKind::Read:
    return "read_coroutine";
  }
  llvm_unreachable("bad kind");
}

StringRef swift::getWriteImplKindName(WriteImplKind kind) {
  switch (kind) {
  case WriteImplKind::Immutable:
    return "immutable";
  case WriteImplKind::Stored:
    return "stored";
  case WriteImplKind::StoredWithObservers:
    return "stored_with_observers";
  case WriteImplKind::InheritedWithObservers:
    return "inherited_with_observers";
  case WriteImplKind::Set:
    return "setter";
  case WriteImplKind::MutableAddress:
    return "mutable_addressor";
  case WriteImplKind::Modify:
    return "modify_coroutine";
  }
  llvm_unreachable("bad kind");
}

StringRef swift::getReadWriteImplKindName(ReadWriteImplKind kind) {
  switch (kind) {
  case ReadWriteImplKind::Immutable:
    return "immutable";
  case ReadWriteImplKind::Stored:
    return "stored";
  case ReadWriteImplKind::MutableAddress:
    return "mutable_addressor";
  case ReadWriteImplKind::MaterializeToTemporary:
    return "materialize_to_temporary";
  case ReadWriteImplKind::Modify:
    return "modify_coroutine";
  }
  llvm_unreachable("bad kind");
}

static StringRef getImportKindString(ImportKind value) {
  switch (value) {
  case ImportKind::Module: return "module";
  case ImportKind::Type: return "type";
  case ImportKind::Struct: return "struct";
  case ImportKind::Class: return "class";
  case ImportKind::Enum: return "enum";
  case ImportKind::Protocol: return "protocol";
  case ImportKind::Var: return "var";
  case ImportKind::Func: return "func";
  }
  
  llvm_unreachable("Unhandled ImportKind in switch.");
}

static StringRef
getForeignErrorConventionKindString(ForeignErrorConvention::Kind value) {
  switch (value) {
  case ForeignErrorConvention::ZeroResult: return "ZeroResult";
  case ForeignErrorConvention::NonZeroResult: return "NonZeroResult";
  case ForeignErrorConvention::ZeroPreservedResult: return "ZeroPreservedResult";
  case ForeignErrorConvention::NilResult: return "NilResult";
  case ForeignErrorConvention::NonNilError: return "NonNilError";
  }

  llvm_unreachable("Unhandled ForeignErrorConvention in switch.");
}
static StringRef getDefaultArgumentKindString(DefaultArgumentKind value) {
  switch (value) {
    case DefaultArgumentKind::None: return "none";
    case DefaultArgumentKind::Column: return "#column";
    case DefaultArgumentKind::DSOHandle: return "#dsohandle";
    case DefaultArgumentKind::File: return "#file";
    case DefaultArgumentKind::Function: return "#function";
    case DefaultArgumentKind::Inherited: return "inherited";
    case DefaultArgumentKind::Line: return "#line";
    case DefaultArgumentKind::NilLiteral: return "nil";
    case DefaultArgumentKind::EmptyArray: return "[]";
    case DefaultArgumentKind::EmptyDictionary: return "[:]";
    case DefaultArgumentKind::Normal: return "normal";
    case DefaultArgumentKind::StoredProperty: return "stored property";
  }

  llvm_unreachable("Unhandled DefaultArgumentKind in switch.");
}
static StringRef
getMagicIdentifierLiteralExprKindString(MagicIdentifierLiteralExpr::Kind value) {
  switch (value) {
    case MagicIdentifierLiteralExpr::File: return "#file";
    case MagicIdentifierLiteralExpr::Function: return "#function";
    case MagicIdentifierLiteralExpr::Line: return "#line";
    case MagicIdentifierLiteralExpr::Column: return "#column";
    case MagicIdentifierLiteralExpr::DSOHandle: return "#dsohandle";
  }

  llvm_unreachable("Unhandled MagicIdentifierLiteralExpr in switch.");
}
static StringRef
getObjCSelectorExprKindString(ObjCSelectorExpr::ObjCSelectorKind value) {
  switch (value) {
    case ObjCSelectorExpr::Method: return "method";
    case ObjCSelectorExpr::Getter: return "getter";
    case ObjCSelectorExpr::Setter: return "setter";
  }

  llvm_unreachable("Unhandled ObjCSelectorExpr in switch.");
}
static StringRef getAccessSemanticsString(AccessSemantics value) {
  switch (value) {
    case AccessSemantics::Ordinary: return "ordinary";
    case AccessSemantics::DirectToStorage: return "direct_to_storage";
    case AccessSemantics::DirectToImplementation: return "direct_to_impl";
  }

  llvm_unreachable("Unhandled AccessSemantics in switch.");
}
static StringRef getMetatypeRepresentationString(MetatypeRepresentation value) {
  switch (value) {
    case MetatypeRepresentation::Thin: return "thin";
    case MetatypeRepresentation::Thick: return "thick";
    case MetatypeRepresentation::ObjC: return "@objc";
  }

  llvm_unreachable("Unhandled MetatypeRepresentation in switch.");
}
static StringRef
getStringLiteralExprEncodingString(StringLiteralExpr::Encoding value) {
  switch (value) {
    case StringLiteralExpr::UTF8: return "utf8";
    case StringLiteralExpr::UTF16: return "utf16";
    case StringLiteralExpr::OneUnicodeScalar: return "unicodeScalar";
  }

  llvm_unreachable("Unhandled StringLiteral in switch.");
}
static StringRef getCtorInitializerKindString(CtorInitializerKind value) {
  switch (value) {
    case CtorInitializerKind::Designated: return "designated";
    case CtorInitializerKind::Convenience: return "convenience";
    case CtorInitializerKind::ConvenienceFactory: return "convenience_factory";
    case CtorInitializerKind::Factory: return "factory";
  }

  llvm_unreachable("Unhandled CtorInitializerKind in switch.");
}
static StringRef getOptionalTypeKindString(OptionalTypeKind value) {
  switch (value) {
    case OTK_None: return "none";
    case OTK_Optional: return "Optional";
    case OTK_ImplicitlyUnwrappedOptional: return "ImplicitlyUnwrappedOptional";
  }

  llvm_unreachable("Unhandled OptionalTypeKind in switch.");
}
static StringRef getAssociativityString(Associativity value) {
  switch (value) {
    case Associativity::None: return "none";
    case Associativity::Left: return "left";
    case Associativity::Right: return "right";
  }

  llvm_unreachable("Unhandled Associativity in switch.");
}

//===----------------------------------------------------------------------===//
//  Decl printing.
//===----------------------------------------------------------------------===//

// Print a name.
static void printName(raw_ostream &os, DeclName name) {
  if (!name)
    os << "<anonymous>";
  else
    os << name;
}

static void dumpSubstitutionMapRec(
    SubstitutionMap map, llvm::raw_ostream &out,
    SubstitutionMap::DumpStyle style, unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited);

namespace {
  class PrintPattern : public PatternVisitor<PrintPattern> {
  public:
    raw_ostream &OS;
    unsigned Indent;

    explicit PrintPattern(raw_ostream &os, unsigned indent = 0)
      : OS(os), Indent(indent) { }

    void printRec(Decl *D) { D->dump(OS, Indent + 2); }
    void printRec(Expr *E) { E->dump(OS, Indent + 2); }
    void printRec(Stmt *S, const ASTContext &Ctx) { S->dump(OS, &Ctx, Indent + 2); }
    void printRec(TypeRepr *T);
    void printRec(const Pattern *P) {
      PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
    }

    raw_ostream &printCommon(Pattern *P, const char *Name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, PatternColor) << Name;

      if (P->isImplicit())
        PrintWithColorRAII(OS, ExprModifierColor) << " implicit";

      if (P->hasType()) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        P->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        PrintWithColorRAII(OS, TypeColor) << "'";
      }
      return OS;
    }

    void visitParenPattern(ParenPattern *P) {
      printCommon(P, "pattern_paren") << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitTuplePattern(TuplePattern *P) {
      printCommon(P, "pattern_tuple");

      OS << " names=";
      interleave(P->getElements(),
                 [&](const TuplePatternElt &elt) {
                   auto name = elt.getLabel();
                   OS << (name.empty() ? "''" : name.str());
                 },
                 [&] { OS << ","; });

      for (auto &elt : P->getElements()) {
        OS << '\n';
        printRec(elt.getPattern());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitNamedPattern(NamedPattern *P) {
      printCommon(P, "pattern_named");
      PrintWithColorRAII(OS, IdentifierColor) << " '" << P->getNameStr() << "'";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitAnyPattern(AnyPattern *P) {
      printCommon(P, "pattern_any");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitTypedPattern(TypedPattern *P) {
      printCommon(P, "pattern_typed") << '\n';
      printRec(P->getSubPattern());
      if (P->getTypeLoc().getTypeRepr()) {
        OS << '\n';
        printRec(P->getTypeLoc().getTypeRepr());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitIsPattern(IsPattern *P) {
      printCommon(P, "pattern_is")
        << ' ' << getCheckedCastKindName(P->getCastKind()) << ' ';
      P->getCastTypeLoc().getType().print(OS);
      if (auto sub = P->getSubPattern()) {
        OS << '\n';
        printRec(sub);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitExprPattern(ExprPattern *P) {
      printCommon(P, "pattern_expr");
      OS << '\n';
      if (auto m = P->getMatchExpr())
        printRec(m);
      else
        printRec(P->getSubExpr());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitVarPattern(VarPattern *P) {
      printCommon(P, P->isLet() ? "pattern_let" : "pattern_var");
      OS << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitEnumElementPattern(EnumElementPattern *P) {
      printCommon(P, "pattern_enum_element");
      OS << ' ';
      P->getParentType().getType().print(
        PrintWithColorRAII(OS, TypeColor).getOS());
      PrintWithColorRAII(OS, IdentifierColor) << '.' << P->getName();
      if (P->hasSubPattern()) {
        OS << '\n';
        printRec(P->getSubPattern());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOptionalSomePattern(OptionalSomePattern *P) {
      printCommon(P, "pattern_optional_some");
      OS << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitBoolPattern(BoolPattern *P) {
      printCommon(P, "pattern_bool");
      OS << (P->getValue() ? " true)" : " false)");
    }

  };

  /// PrintDecl - Visitor implementation of Decl::print.
  class PrintDecl : public DeclVisitor<PrintDecl> {
  public:
    raw_ostream &OS;
    unsigned Indent;

    explicit PrintDecl(raw_ostream &os, unsigned indent = 0)
      : OS(os), Indent(indent) { }
    
    void printRec(Decl *D) { PrintDecl(OS, Indent + 2).visit(D); }
    void printRec(Expr *E) { E->dump(OS, Indent+2); }
    void printRec(Stmt *S, const ASTContext &Ctx) { S->dump(OS, &Ctx, Indent+2); }
    void printRec(Pattern *P) { PrintPattern(OS, Indent+2).visit(P); }
    void printRec(TypeRepr *T);

    // Print a field with a value.
    template<typename T>
    raw_ostream &printField(StringRef name, const T &value) {
      OS << " ";
      PrintWithColorRAII(OS, TypeFieldColor) << name;
      OS << "=" << value;
      return OS;
    }

    void printCommon(Decl *D, const char *Name,
                     TerminalColor Color = DeclColor) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, Color) << Name;

      if (D->isImplicit())
        PrintWithColorRAII(OS, DeclModifierColor) << " implicit";

      auto R = D->getSourceRange();
      if (R.isValid()) {
        PrintWithColorRAII(OS, RangeColor) << " range=";
        R.print(PrintWithColorRAII(OS, RangeColor).getOS(),
                D->getASTContext().SourceMgr, /*PrintText=*/false);
      }

      if (D->TrailingSemiLoc.isValid())
        PrintWithColorRAII(OS, DeclModifierColor) << " trailing_semi";
    }

    void printInherited(ArrayRef<TypeLoc> Inherited) {
      if (Inherited.empty())
        return;
      OS << " inherits: ";
      interleave(Inherited, [&](TypeLoc Super) { Super.getType().print(OS); },
                 [&] { OS << ", "; });
    }

    void visitImportDecl(ImportDecl *ID) {
      printCommon(ID, "import_decl");

      if (ID->isExported())
        OS << " exported";

      if (ID->getImportKind() != ImportKind::Module)
        OS << " kind=" << getImportKindString(ID->getImportKind());

      OS << " '";
      interleave(ID->getFullAccessPath(),
                 [&](const ImportDecl::AccessPathElement &Elem) {
                   OS << Elem.first;
                 },
                 [&] { OS << '.'; });
      OS << "')";
    }

    void visitExtensionDecl(ExtensionDecl *ED) {
      printCommon(ED, "extension_decl", ExtensionColor);
      OS << ' ';
      ED->getExtendedType().print(OS);
      printInherited(ED->getInherited());
      for (Decl *Member : ED->getMembers()) {
        OS << '\n';
        printRec(Member);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printDeclName(const ValueDecl *D) {
      if (D->getFullName()) {
        PrintWithColorRAII(OS, IdentifierColor)
          << '\"' << D->getFullName() << '\"';
      } else {
        PrintWithColorRAII(OS, IdentifierColor)
          << "'anonname=" << (const void*)D << '\'';
      }
    }

    void visitTypeAliasDecl(TypeAliasDecl *TAD) {
      printCommon(TAD, "typealias");
      PrintWithColorRAII(OS, TypeColor) << " type='";
      if (TAD->getUnderlyingTypeLoc().getType()) {
        PrintWithColorRAII(OS, TypeColor)
          << TAD->getUnderlyingTypeLoc().getType().getString();
      } else {
        PrintWithColorRAII(OS, TypeColor) << "<<<unresolved>>>";
      }
      printInherited(TAD->getInherited());
      OS << "')";
    }
    
    void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD) {
      printCommon(OTD, "opaque_type");
      OS << " naming_decl=";
      printDeclName(OTD->getNamingDecl());
      PrintWithColorRAII(OS, TypeColor) << " opaque_interface="
        << Type(OTD->getUnderlyingInterfaceType()).getString();
      OS << " in "
         << OTD->getOpaqueInterfaceGenericSignature()->getAsString();
      if (auto underlyingSubs = OTD->getUnderlyingTypeSubstitutions()) {
        OS << " underlying:\n";
        SmallPtrSet<const ProtocolConformance *, 4> Dumped;
        dumpSubstitutionMapRec(*underlyingSubs, OS,
                               SubstitutionMap::DumpStyle::Full,
                               Indent + 2, Dumped);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAbstractTypeParamCommon(AbstractTypeParamDecl *decl,
                                      const char *name) {
      printCommon(decl, name);
      if (decl->getDeclContext()->getGenericEnvironmentOfContext()) {
        if (auto superclassTy = decl->getSuperclass()) {
          OS << " superclass='" << superclassTy->getString() << "'";
        }
      }
    }

    void visitGenericTypeParamDecl(GenericTypeParamDecl *decl) {
      printAbstractTypeParamCommon(decl, "generic_type_param");
      OS << " depth=" << decl->getDepth() << " index=" << decl->getIndex();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitAssociatedTypeDecl(AssociatedTypeDecl *decl) {
      printAbstractTypeParamCommon(decl, "associated_type_decl");
      if (auto defaultDef = decl->getDefaultDefinitionType()) {
        OS << " default=";
        defaultDef.print(OS);
      }
      if (auto whereClause = decl->getTrailingWhereClause()) {
        OS << " where requirements: ";
        interleave(whereClause->getRequirements(),
                   [&](const RequirementRepr &req) { req.print(OS); },
                   [&] { OS << ", "; });
      }
      if (decl->overriddenDeclsComputed()) {
        OS << " overridden=";
        interleave(decl->getOverriddenDecls(),
                   [&](AssociatedTypeDecl *overridden) {
                     OS << overridden->getProtocol()->getName();
                   }, [&]() {
                     OS << ", ";
                   });
      }

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitProtocolDecl(ProtocolDecl *PD) {
      printCommon(PD, "protocol");

      OS << " requirement signature=";
      if (PD->isRequirementSignatureComputed()) {
        OS << GenericSignature::get({PD->getProtocolSelfType()} ,
                                    PD->getRequirementSignature())
                ->getAsString();
      } else {
        OS << "<null>";
      }
      printInherited(PD->getInherited());
      if (auto whereClause = PD->getTrailingWhereClause()) {
        OS << " where requirements: ";
        interleave(whereClause->getRequirements(),
                   [&](const RequirementRepr &req) { req.print(OS); },
                   [&] { OS << ", "; });
      }

      for (auto VD : PD->getMembers()) {
        OS << '\n';
        printRec(VD);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printCommon(ValueDecl *VD, const char *Name,
                     TerminalColor Color = DeclColor) {
      printCommon((Decl*)VD, Name, Color);

      OS << ' ';
      printDeclName(VD);
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD))
        printGenericParameters(OS, AFD->getGenericParams());
      if (auto *GTD = dyn_cast<GenericTypeDecl>(VD))
        printGenericParameters(OS, GTD->getGenericParams());

      if (auto *var = dyn_cast<VarDecl>(VD)) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        if (var->hasType())
          var->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        else
          PrintWithColorRAII(OS, TypeColor) << "<null type>";
        PrintWithColorRAII(OS, TypeColor) << "'";
      }

      if (VD->hasInterfaceType()) {
        PrintWithColorRAII(OS, InterfaceTypeColor) << " interface type='";
        VD->getInterfaceType()->print(
            PrintWithColorRAII(OS, InterfaceTypeColor).getOS());
        PrintWithColorRAII(OS, InterfaceTypeColor) << "'";
      }

      if (VD->hasAccess()) {
        PrintWithColorRAII(OS, AccessLevelColor) << " access="
          << getAccessLevelSpelling(VD->getFormalAccess());
      }

      if (VD->overriddenDeclsComputed()) {
        auto overridden = VD->getOverriddenDecls();
        if (!overridden.empty()) {
          PrintWithColorRAII(OS, OverrideColor) << " override=";
          interleave(overridden,
                     [&](ValueDecl *overridden) {
                       overridden->dumpRef(
                                PrintWithColorRAII(OS, OverrideColor).getOS());
                     }, [&]() {
                       OS << ", ";
                     });
        }
      }

      auto VarD = dyn_cast<VarDecl>(VD);
      if (VD->isFinal() && !(VarD && VarD->isLet()))
        OS << " final";
      if (VD->isObjC())
        OS << " @objc";
      if (VD->isDynamic())
        OS << " dynamic";
      if (auto *attr =
              VD->getAttrs().getAttribute<DynamicReplacementAttr>()) {
        OS << " @_dynamicReplacement(for: \"";
        OS << attr->getReplacedFunctionName();
        OS << "\")";
      }
    }

    void printCommon(NominalTypeDecl *NTD, const char *Name,
                     TerminalColor Color = DeclColor) {
      printCommon((ValueDecl *)NTD, Name, Color);

      if (NTD->hasInterfaceType()) {
        if (NTD->isResilient())
          OS << " resilient";
        else
          OS << " non-resilient";
      }
    }

    void visitSourceFile(const SourceFile &SF) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ASTNodeColor) << "source_file ";
      PrintWithColorRAII(OS, LocationColor) << '\"' << SF.getFilename() << '\"';
      
      for (Decl *D : SF.Decls) {
        if (D->isImplicit())
          continue;

        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitVarDecl(VarDecl *VD) {
      printCommon(VD, "var_decl");
      if (VD->isLet())
        PrintWithColorRAII(OS, DeclModifierColor) << " let";
      if (VD->hasNonPatternBindingInit())
        PrintWithColorRAII(OS, DeclModifierColor) << " non_pattern_init";
      if (VD->getAttrs().hasAttribute<LazyAttr>())
        PrintWithColorRAII(OS, DeclModifierColor) << " lazy";
      printStorageImpl(VD);
      printAccessors(VD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printStorageImpl(AbstractStorageDecl *D) {
      if (D->isStatic())
        PrintWithColorRAII(OS, DeclModifierColor) << " type";

      auto impl = D->getImplInfo();
      PrintWithColorRAII(OS, DeclModifierColor)
        << " readImpl="
        << getReadImplKindName(impl.getReadImpl());
      if (!impl.supportsMutation()) {
        PrintWithColorRAII(OS, DeclModifierColor)
          << " immutable";
      } else {
        PrintWithColorRAII(OS, DeclModifierColor)
          << " writeImpl="
          << getWriteImplKindName(impl.getWriteImpl());
        PrintWithColorRAII(OS, DeclModifierColor)
          << " readWriteImpl="
          << getReadWriteImplKindName(impl.getReadWriteImpl());
      }
    }

    void printAccessors(AbstractStorageDecl *D) {
      for (auto accessor : D->getAllAccessors()) {
        OS << "\n";
        printRec(accessor);
      }
    }

    void visitParamDecl(ParamDecl *PD) {
      printParameter(PD);
    }

    void visitEnumCaseDecl(EnumCaseDecl *ECD) {
      printCommon(ECD, "enum_case_decl");
      for (EnumElementDecl *D : ECD->getElements()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitEnumDecl(EnumDecl *ED) {
      printCommon(ED, "enum_decl");
      printInherited(ED->getInherited());
      for (Decl *D : ED->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitEnumElementDecl(EnumElementDecl *EED) {
      printCommon(EED, "enum_element_decl");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitStructDecl(StructDecl *SD) {
      printCommon(SD, "struct_decl");
      printInherited(SD->getInherited());
      for (Decl *D : SD->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitClassDecl(ClassDecl *CD) {
      printCommon(CD, "class_decl");
      if (CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>())
        OS << " @_staticInitializeObjCMetadata";
      printInherited(CD->getInherited());
      for (Decl *D : CD->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPatternBindingDecl(PatternBindingDecl *PBD) {
      printCommon(PBD, "pattern_binding_decl");

      for (auto entry : PBD->getPatternList()) {
        OS << '\n';
        printRec(entry.getPattern());
        if (entry.getInit()) {
          OS << '\n';
          printRec(entry.getInit());
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSubscriptDecl(SubscriptDecl *SD) {
      printCommon(SD, "subscript_decl");
      printStorageImpl(SD);
      printAccessors(SD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printCommonAFD(AbstractFunctionDecl *D, const char *Type) {
      printCommon(D, Type, FuncColor);
      if (!D->getCaptureInfo().isTrivial()) {
        OS << " ";
        D->getCaptureInfo().print(OS);
      }

      if (auto fec = D->getForeignErrorConvention()) {
        OS << " foreign_error=";
        OS << getForeignErrorConventionKindString(fec->getKind());
        bool wantResultType = (
          fec->getKind() == ForeignErrorConvention::ZeroResult ||
          fec->getKind() == ForeignErrorConvention::NonZeroResult);

        OS << ((fec->isErrorOwned() == ForeignErrorConvention::IsOwned)
                ? ",owned"
                : ",unowned");
        OS << ",param=" << llvm::utostr(fec->getErrorParameterIndex());
        OS << ",paramtype=" << fec->getErrorParameterType().getString();
        if (wantResultType)
          OS << ",resulttype=" << fec->getResultType().getString();
      }
    }

    void printParameter(const ParamDecl *P) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ParameterColor) << "parameter ";
      printDeclName(P);
      if (!P->getArgumentName().empty())
        PrintWithColorRAII(OS, IdentifierColor)
          << " apiName=" << P->getArgumentName();

      if (P->hasType()) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        P->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        PrintWithColorRAII(OS, TypeColor) << "'";
      }

      if (P->hasInterfaceType()) {
        PrintWithColorRAII(OS, InterfaceTypeColor) << " interface type='";
        P->getInterfaceType().print(
            PrintWithColorRAII(OS, InterfaceTypeColor).getOS());
        PrintWithColorRAII(OS, InterfaceTypeColor) << "'";
      }

      switch (P->getSpecifier()) {
      case VarDecl::Specifier::Let:
        /* nothing */
        break;
      case VarDecl::Specifier::Var:
        OS << " mutable";
        break;
      case VarDecl::Specifier::InOut:
        OS << " inout";
        break;
      case VarDecl::Specifier::Shared:
        OS << " shared";
        break;
      case VarDecl::Specifier::Owned:
        OS << " owned";
        break;
      }

      if (P->isVariadic())
        OS << " variadic";

      if (P->isAutoClosure())
        OS << " autoclosure";

      if (P->getDefaultArgumentKind() != DefaultArgumentKind::None)
        printField("default_arg",
                   getDefaultArgumentKindString(P->getDefaultArgumentKind()));

      if (auto init = P->getDefaultValue()) {
        OS << " expression=\n";
        printRec(init);
      }

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printParameterList(const ParameterList *params, const ASTContext *ctx = nullptr) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ParameterColor) << "parameter_list";
      Indent += 2;
      for (auto P : *params) {
        OS << '\n';
        printParameter(P);
      }

      if (!ctx && params->size() != 0 && params->get(0))
        ctx = &params->get(0)->getASTContext();

      if (ctx) {
        auto R = params->getSourceRange();
        if (R.isValid()) {
          PrintWithColorRAII(OS, RangeColor) << " range=";
          R.print(PrintWithColorRAII(OS, RangeColor).getOS(),
                  ctx->SourceMgr, /*PrintText=*/false);
        }
      }

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      Indent -= 2;
    }

    void printAbstractFunctionDecl(AbstractFunctionDecl *D) {
      Indent += 2;
      if (auto *P = D->getImplicitSelfDecl()) {
        OS << '\n';
        printParameter(P);
      }

      OS << '\n';
      printParameterList(D->getParameters(), &D->getASTContext());
      Indent -= 2;

      if (auto FD = dyn_cast<FuncDecl>(D)) {
        if (FD->getBodyResultTypeLoc().getTypeRepr()) {
          OS << '\n';
          Indent += 2;
          OS.indent(Indent);
          PrintWithColorRAII(OS, ParenthesisColor) << '(';
          OS << "result\n";
          printRec(FD->getBodyResultTypeLoc().getTypeRepr());
          PrintWithColorRAII(OS, ParenthesisColor) << ')';
          if (auto opaque = FD->getOpaqueResultTypeDecl()) {
            OS << '\n';
            OS.indent(Indent);
            PrintWithColorRAII(OS, ParenthesisColor) << '(';
            OS << "opaque_result_decl\n";
            printRec(opaque);
            PrintWithColorRAII(OS, ParenthesisColor) << ')';
          }
          Indent -= 2;
        }
      }
      if (D->hasSingleExpressionBody()) {
        OS << '\n';
        printRec(D->getSingleExpressionBody());
      } else if (auto Body = D->getBody(/*canSynthesize=*/false)) {
        OS << '\n';
        printRec(Body, D->getASTContext());
      }
    }

    void printCommonFD(FuncDecl *FD, const char *type) {
      printCommonAFD(FD, type);
      if (FD->isStatic())
        OS << " type";
    }

    void visitFuncDecl(FuncDecl *FD) {
      printCommonFD(FD, "func_decl");
      printAbstractFunctionDecl(FD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitAccessorDecl(AccessorDecl *AD) {
      printCommonFD(AD, "accessor_decl");
      OS << " " << getAccessorKindString(AD->getAccessorKind());
      OS << "_for=" << AD->getStorage()->getFullName();
      printAbstractFunctionDecl(AD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitConstructorDecl(ConstructorDecl *CD) {
      printCommonAFD(CD, "constructor_decl");
      if (CD->isRequired())
        PrintWithColorRAII(OS, DeclModifierColor) << " required";
      PrintWithColorRAII(OS, DeclModifierColor) << " "
        << getCtorInitializerKindString(CD->getInitKind());
      if (CD->getFailability() != OTK_None)
        PrintWithColorRAII(OS, DeclModifierColor) << " failable="
          << getOptionalTypeKindString(CD->getFailability());
      printAbstractFunctionDecl(CD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDestructorDecl(DestructorDecl *DD) {
      printCommonAFD(DD, "destructor_decl");
      printAbstractFunctionDecl(DD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
      printCommon(TLCD, "top_level_code_decl");
      if (TLCD->getBody()) {
        OS << "\n";
        printRec(TLCD->getBody(), static_cast<Decl *>(TLCD)->getASTContext());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    
    void printASTNodes(const ArrayRef<ASTNode> &Elements, const ASTContext &Ctx, StringRef Name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << "(";
      PrintWithColorRAII(OS, ASTNodeColor) << Name;
      for (auto Elt : Elements) {
        OS << '\n';
        if (auto *SubExpr = Elt.dyn_cast<Expr*>())
          printRec(SubExpr);
        else if (auto *SubStmt = Elt.dyn_cast<Stmt*>())
          printRec(SubStmt, Ctx);
        else
          printRec(Elt.get<Decl*>());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitIfConfigDecl(IfConfigDecl *ICD) {
      printCommon(ICD, "if_config_decl");
      Indent += 2;
      for (auto &Clause : ICD->getClauses()) {
        OS << '\n';
        OS.indent(Indent);
        PrintWithColorRAII(OS, StmtColor) << (Clause.Cond ? "#if:" : "#else:");
        if (Clause.isActive)
          PrintWithColorRAII(OS, DeclModifierColor) << " active";
        if (Clause.Cond) {
          OS << "\n";
          printRec(Clause.Cond);
        }

        OS << '\n';
        Indent += 2;
        printASTNodes(Clause.Elements, ICD->getASTContext(), "elements");
        Indent -= 2;
      }

      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD) {
      printCommon(PDD, "pound_diagnostic_decl");
      auto kind = PDD->isError() ? "error" : "warning";
      OS << " kind=" << kind << "\n";
      Indent += 2;
      printRec(PDD->getMessage());
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD) {
      printCommon(PGD, "precedence_group_decl ");
      OS << PGD->getName() << "\n";

      OS.indent(Indent+2);
      OS << "associativity "
         << getAssociativityString(PGD->getAssociativity()) << "\n";

      OS.indent(Indent+2);
      OS << "assignment " << (PGD->isAssignment() ? "true" : "false");

      auto printRelations =
          [&](StringRef label, ArrayRef<PrecedenceGroupDecl::Relation> rels) {
        if (rels.empty()) return;
        OS << '\n';
        OS.indent(Indent+2);
        OS << label << ' ' << rels[0].Name;
        for (auto &rel : rels.slice(1))
          OS << ", " << rel.Name;
      };
      printRelations("higherThan", PGD->getHigherThan());
      printRelations("lowerThan", PGD->getLowerThan());

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printOperatorIdentifiers(OperatorDecl *OD) {
      auto identifiers = OD->getIdentifiers();
      for (auto index : indices(identifiers)) {
        OS.indent(Indent + 2);
        OS << "identifier #" << index << " " << identifiers[index];
        if (index != identifiers.size() - 1)
          OS << "\n";
      }
    }

    void visitInfixOperatorDecl(InfixOperatorDecl *IOD) {
      printCommon(IOD, "infix_operator_decl");
      OS << " " << IOD->getName();
      if (!IOD->getIdentifiers().empty()) {
        OS << "\n";
        printOperatorIdentifiers(IOD);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPrefixOperatorDecl(PrefixOperatorDecl *POD) {
      printCommon(POD, "prefix_operator_decl");
      OS << " " << POD->getName();
      if (!POD->getIdentifiers().empty()) {
        OS << "\n";
        printOperatorIdentifiers(POD);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPostfixOperatorDecl(PostfixOperatorDecl *POD) {
      printCommon(POD, "postfix_operator_decl");
      OS << " " << POD->getName();
      if (!POD->getIdentifiers().empty()) {
        OS << "\n";
        printOperatorIdentifiers(POD);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitModuleDecl(ModuleDecl *MD) {
      printCommon(MD, "module");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMissingMemberDecl(MissingMemberDecl *MMD) {
      printCommon(MMD, "missing_member_decl ");
      PrintWithColorRAII(OS, IdentifierColor)
          << '\"' << MMD->getFullName() << '\"';
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
  };
} // end anonymous namespace

void ParameterList::dump() const {
  dump(llvm::errs(), 0);
}

void ParameterList::dump(raw_ostream &OS, unsigned Indent) const {
  llvm::Optional<llvm::SaveAndRestore<bool>> X;
  
  // Make sure to print type variables if we can get to ASTContext.
  if (size() != 0 && get(0)) {
    auto &ctx = get(0)->getASTContext();
    X.emplace(llvm::SaveAndRestore<bool>(ctx.LangOpts.DebugConstraintSolver,
                                         true));
  }
  
  PrintDecl(OS, Indent).printParameterList(this);
  llvm::errs() << '\n';
}



void Decl::dump() const {
  dump(llvm::errs(), 0);
}

void Decl::dump(const char *filename) const {
  std::error_code ec;
  llvm::raw_fd_ostream stream(filename, ec, llvm::sys::fs::FA_Read |
                              llvm::sys::fs::FA_Write);
  // In assert builds, we blow up. Otherwise, we just return.
  assert(!ec && "Failed to open file for dumping?!");
  if (ec)
    return;
  dump(stream, 0);
}

void Decl::dump(raw_ostream &OS, unsigned Indent) const {
  // Make sure to print type variables.
  llvm::SaveAndRestore<bool> X(getASTContext().LangOpts.DebugConstraintSolver,
                               true);
  PrintDecl(OS, Indent).visit(const_cast<Decl *>(this));
  OS << '\n';
}

/// Print the given declaration context (with its parents).
void swift::printContext(raw_ostream &os, DeclContext *dc) {
  if (auto parent = dc->getParent()) {
    printContext(os, parent);
    os << '.';
  }

  switch (dc->getContextKind()) {
  case DeclContextKind::Module:
    printName(os, cast<ModuleDecl>(dc)->getName());
    break;

  case DeclContextKind::FileUnit:
    // FIXME: print the file's basename?
    os << "(file)";
    break;

  case DeclContextKind::SerializedLocal:
    os << "local context";
    break;

  case DeclContextKind::AbstractClosureExpr: {
    auto *ACE = cast<AbstractClosureExpr>(dc);
    if (isa<ClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "explicit closure discriminator=";
    }
    if (isa<AutoClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "autoclosure discriminator=";
    }
    PrintWithColorRAII(os, DiscriminatorColor) << ACE->getDiscriminator();
    break;
  }

  case DeclContextKind::GenericTypeDecl:
    printName(os, cast<GenericTypeDecl>(dc)->getName());
    break;

  case DeclContextKind::ExtensionDecl:
    if (auto extendedNominal = cast<ExtensionDecl>(dc)->getExtendedNominal()) {
      printName(os, extendedNominal->getName());
    }
    os << " extension";
    break;

  case DeclContextKind::Initializer:
    switch (cast<Initializer>(dc)->getInitializerKind()) {
    case InitializerKind::PatternBinding:
      os << "pattern binding initializer";
      break;
    case InitializerKind::DefaultArgument:
      os << "default argument initializer";
      break;
    }
    break;

  case DeclContextKind::TopLevelCodeDecl:
    os << "top-level code";
    break;

  case DeclContextKind::AbstractFunctionDecl:
    printName(os, cast<AbstractFunctionDecl>(dc)->getFullName());
    break;

  case DeclContextKind::SubscriptDecl:
    printName(os, cast<SubscriptDecl>(dc)->getFullName());
    break;

  case DeclContextKind::EnumElementDecl:
    printName(os, cast<EnumElementDecl>(dc)->getFullName());
    break;
  }
}

std::string ValueDecl::printRef() const {
  std::string result;
  llvm::raw_string_ostream os(result);
  dumpRef(os);
  return os.str();
}

void ValueDecl::dumpRef(raw_ostream &os) const {
  // Print the context.
  printContext(os, getDeclContext());
  os << ".";

  // Print name.
  getFullName().printPretty(os);

  // Print location.
  auto &srcMgr = getASTContext().SourceMgr;
  if (getLoc().isValid()) {
    os << '@';
    getLoc().print(os, srcMgr);
  }
}

void LLVM_ATTRIBUTE_USED ValueDecl::dumpRef() const {
  dumpRef(llvm::errs());
}

void SourceFile::dump() const {
  dump(llvm::errs());
}

void SourceFile::dump(llvm::raw_ostream &OS) const {
  llvm::SaveAndRestore<bool> X(getASTContext().LangOpts.DebugConstraintSolver,
                               true);
  PrintDecl(OS).visitSourceFile(*this);
  llvm::errs() << '\n';
}

void Pattern::dump() const {
  PrintPattern(llvm::errs()).visit(const_cast<Pattern*>(this));
  llvm::errs() << '\n';
}

//===----------------------------------------------------------------------===//
// Printing for Stmt and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintStmt - Visitor implementation of Stmt::dump.
class PrintStmt : public StmtVisitor<PrintStmt> {
public:
  raw_ostream &OS;
  const ASTContext *Ctx;
  unsigned Indent;

  PrintStmt(raw_ostream &os, const ASTContext *ctx, unsigned indent)
    : OS(os), Ctx(ctx), Indent(indent) {
  }

  void printRec(Stmt *S) {
    Indent += 2;
    if (S)
      visit(S);
    else
      OS.indent(Indent) << "(**NULL STATEMENT**)";
    Indent -= 2;
  }

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Expr *E) { E->dump(OS, Indent + 2); }
  void printRec(const Pattern *P) {
    PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
  }

  void printRec(StmtConditionElement C) {
    switch (C.getKind()) {
    case StmtConditionElement::CK_Boolean:
      return printRec(C.getBoolean());
    case StmtConditionElement::CK_PatternBinding:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, PatternColor) << "pattern\n";

      printRec(C.getPattern());
      OS << "\n";
      printRec(C.getInitializer());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      Indent -= 2;
      break;
    case StmtConditionElement::CK_Availability:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      OS << "#available\n";
      for (auto *Query : C.getAvailability()->getQueries()) {
        OS << '\n';
        switch (Query->getKind()) {
        case AvailabilitySpecKind::PlatformVersionConstraint:
          cast<PlatformVersionConstraintAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        case AvailabilitySpecKind::LanguageVersionConstraint:
        case AvailabilitySpecKind::PackageDescriptionVersionConstraint:
          cast<PlatformVersionConstraintAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        case AvailabilitySpecKind::OtherPlatform:
          cast<OtherPlatformAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ")";
      Indent -= 2;
      break;
    }
  }

  raw_ostream &printCommon(Stmt *S, const char *Name) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << Name;

    if (S->isImplicit())
      OS << " implicit";

    if (Ctx) {
      auto R = S->getSourceRange();
      if (R.isValid()) {
        PrintWithColorRAII(OS, RangeColor) << " range=";
        R.print(PrintWithColorRAII(OS, RangeColor).getOS(),
                Ctx->SourceMgr, /*PrintText=*/false);
      }
    }

    if (S->TrailingSemiLoc.isValid())
      OS << " trailing_semi";

    return OS;
  }

  void visitBraceStmt(BraceStmt *S) {
    printCommon(S, "brace_stmt");
    printASTNodes(S->getElements());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printASTNodes(const ArrayRef<ASTNode> &Elements) {
    for (auto Elt : Elements) {
      OS << '\n';
      if (auto *SubExpr = Elt.dyn_cast<Expr*>())
        printRec(SubExpr);
      else if (auto *SubStmt = Elt.dyn_cast<Stmt*>())
        printRec(SubStmt);
      else
        printRec(Elt.get<Decl*>());
    }
  }

  void visitReturnStmt(ReturnStmt *S) {
    printCommon(S, "return_stmt");
    if (S->hasResult()) {
      OS << '\n';
      printRec(S->getResult());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitYieldStmt(YieldStmt *S) {
    printCommon(S, "yield_stmt");
    for (auto yield : S->getYields()) {
      OS << '\n';
      printRec(yield);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDeferStmt(DeferStmt *S) {
    printCommon(S, "defer_stmt") << '\n';
    printRec(S->getTempDecl());
    OS << '\n';
    printRec(S->getCallExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIfStmt(IfStmt *S) {
    printCommon(S, "if_stmt") << '\n';
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getThenStmt());
    if (S->getElseStmt()) {
      OS << '\n';
      printRec(S->getElseStmt());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitGuardStmt(GuardStmt *S) {
    printCommon(S, "guard_stmt") << '\n';
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDoStmt(DoStmt *S) {
    printCommon(S, "do_stmt") << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitWhileStmt(WhileStmt *S) {
    printCommon(S, "while_stmt") << '\n';
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitRepeatWhileStmt(RepeatWhileStmt *S) {
    printCommon(S, "repeat_while_stmt") << '\n';
    printRec(S->getBody());
    OS << '\n';
    printRec(S->getCond());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForEachStmt(ForEachStmt *S) {
    printCommon(S, "for_each_stmt") << '\n';
    printRec(S->getPattern());
    OS << '\n';
    if (S->getWhere()) {
      Indent += 2;
      OS.indent(Indent) << "(where\n";
      printRec(S->getWhere());
      OS << ")\n";
      Indent -= 2;
    }
    printRec(S->getPattern());
    OS << '\n';
    printRec(S->getSequence());
    OS << '\n';
    if (S->getIterator()) {
      printRec(S->getIterator());
      OS << '\n';
    }
    if (S->getIteratorNext()) {
      printRec(S->getIteratorNext());
      OS << '\n';
    }
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBreakStmt(BreakStmt *S) {
    printCommon(S, "break_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitContinueStmt(ContinueStmt *S) {
    printCommon(S, "continue_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFallthroughStmt(FallthroughStmt *S) {
    printCommon(S, "fallthrough_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSwitchStmt(SwitchStmt *S) {
    printCommon(S, "switch_stmt") << '\n';
    printRec(S->getSubjectExpr());
    for (auto N : S->getRawCases()) {
      OS << '\n';
      if (N.is<Stmt*>())
        printRec(N.get<Stmt*>());
      else
        printRec(N.get<Decl*>());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCaseStmt(CaseStmt *S) {
    printCommon(S, "case_stmt");
    if (S->hasUnknownAttr())
      OS << " @unknown";

    if (S->hasCaseBodyVariables()) {
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, StmtColor) << "case_body_variables";
      OS << '\n';
      for (auto *vd : S->getCaseBodyVariables()) {
        OS.indent(2);
        // TODO: Printing a var decl does an Indent ... dump(vd) ... '\n'. We
        // should see if we can factor this dumping so that the caller of
        // printRec(VarDecl) has more control over the printing.
        printRec(vd);
      }
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    for (const auto &LabelItem : S->getCaseLabelItems()) {
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, StmtColor) << "case_label_item";
      if (LabelItem.isDefault())
        OS << " default";
      if (auto *CasePattern = LabelItem.getPattern()) {
        OS << '\n';
        printRec(CasePattern);
      }
      if (auto *Guard = LabelItem.getGuardExpr()) {
        OS << '\n';
        Guard->dump(OS, Indent+4);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFailStmt(FailStmt *S) {
    printCommon(S, "fail_stmt");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitThrowStmt(ThrowStmt *S) {
    printCommon(S, "throw_stmt") << '\n';
    printRec(S->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitPoundAssertStmt(PoundAssertStmt *S) {
    printCommon(S, "pound_assert");
    OS << " message=" << QuotedString(S->getMessage()) << "\n";
    printRec(S->getCondition());
    OS << ")";
  }

  void visitDoCatchStmt(DoCatchStmt *S) {
    printCommon(S, "do_catch_stmt") << '\n';
    printRec(S->getBody());
    OS << '\n';
    Indent += 2;
    visitCatches(S->getCatches());
    Indent -= 2;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCatches(ArrayRef<CatchStmt*> clauses) {
    for (auto clause : clauses) {
      visitCatchStmt(clause);
    }
  }
  void visitCatchStmt(CatchStmt *clause) {
    printCommon(clause, "catch") << '\n';
    printRec(clause->getErrorPattern());
    if (auto guard = clause->getGuardExpr()) {
      OS << '\n';
      printRec(guard);
    }
    OS << '\n';
    printRec(clause->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
};

} // end anonymous namespace

void Stmt::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void Stmt::dump(raw_ostream &OS, const ASTContext *Ctx, unsigned Indent) const {
  PrintStmt(OS, Ctx, Indent).visit(const_cast<Stmt*>(this));
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::dump.
class PrintExpr : public ExprVisitor<PrintExpr> {
public:
  raw_ostream &OS;
  llvm::function_ref<Type(const Expr *)> GetTypeOfExpr;
  llvm::function_ref<Type(const TypeLoc &)> GetTypeOfTypeLoc;
  llvm::function_ref<Type(const KeyPathExpr *E, unsigned index)> GetTypeOfKeyPathComponent;
  unsigned Indent;

  PrintExpr(raw_ostream &os,
            llvm::function_ref<Type(const Expr *)> getTypeOfExpr,
            llvm::function_ref<Type(const TypeLoc &)> getTypeOfTypeLoc,
            llvm::function_ref<Type(const KeyPathExpr *E, unsigned index)> getTypeOfKeyPathComponent,
            unsigned indent)
      : OS(os), GetTypeOfExpr(getTypeOfExpr),
        GetTypeOfTypeLoc(getTypeOfTypeLoc),
        GetTypeOfKeyPathComponent(getTypeOfKeyPathComponent), Indent(indent) {}

  void printRec(Expr *E) {
    Indent += 2;
    if (E)
      visit(E);
    else
      OS.indent(Indent) << "(**NULL EXPRESSION**)";
    Indent -= 2;
  }

  void printRecLabeled(Expr *E, StringRef label) {
    Indent += 2;
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << label;
    OS << '\n';
    printRec(E);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;
  }

  /// FIXME: This should use ExprWalker to print children.

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Stmt *S, const ASTContext &Ctx) { S->dump(OS, &Ctx, Indent + 2); }
  void printRec(const Pattern *P) {
    PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
  }
  void printRec(TypeRepr *T);
  void printRec(ProtocolConformanceRef conf) {
    conf.dump(OS, Indent + 2);
  }

  void printDeclRef(ConcreteDeclRef declRef) {
    declRef.dump(PrintWithColorRAII(OS, DeclColor).getOS());
  }

  raw_ostream &printCommon(Expr *E, const char *C) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << C;

    if (E->isImplicit())
      PrintWithColorRAII(OS, ExprModifierColor) << " implicit";
    PrintWithColorRAII(OS, TypeColor) << " type='" << GetTypeOfExpr(E) << '\'';

    // If we have a source range and an ASTContext, print the source range.
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      auto L = E->getLoc();
      if (L.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " location=";
        L.print(PrintWithColorRAII(OS, LocationColor).getOS(), Ctx.SourceMgr);
      }

      auto R = E->getSourceRange();
      if (R.isValid()) {
        PrintWithColorRAII(OS, RangeColor) << " range=";
        R.print(PrintWithColorRAII(OS, RangeColor).getOS(),
                Ctx.SourceMgr, /*PrintText=*/false);
      }
    }

    if (E->TrailingSemiLoc.isValid())
      OS << " trailing_semi";

    return OS;
  }
  
  void printSemanticExpr(Expr * semanticExpr) {
    if (semanticExpr == nullptr) {
      return;
    }
    
    OS << '\n';
    printRecLabeled(semanticExpr, "semantic_expr");
  }

  void visitErrorExpr(ErrorExpr *E) {
    printCommon(E, "error_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCodeCompletionExpr(CodeCompletionExpr *E) {
    printCommon(E, "code_completion_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitNilLiteralExpr(NilLiteralExpr *E) {
    printCommon(E, "nil_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    printCommon(E, "integer_literal_expr");
    if (E->isNegative())
      PrintWithColorRAII(OS, LiteralValueColor) << " negative";
    PrintWithColorRAII(OS, LiteralValueColor) << " value=";
    Type T = GetTypeOfExpr(E);
    if (T.isNull() || !T->is<BuiltinIntegerType>())
      PrintWithColorRAII(OS, LiteralValueColor) << E->getDigitsText();
    else
      PrintWithColorRAII(OS, LiteralValueColor) << E->getValue();
    PrintWithColorRAII(OS, LiteralValueColor) << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
        PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFloatLiteralExpr(FloatLiteralExpr *E) {
    printCommon(E, "float_literal_expr");
    if (E->isNegative())
      PrintWithColorRAII(OS, LiteralValueColor) << " negative";
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << E->getDigitsText();
    PrintWithColorRAII(OS, LiteralValueColor) << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
        PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    if (!E->getBuiltinType().isNull()) {
      PrintWithColorRAII(OS, TypeColor) << " builtin_type='";
      E->getBuiltinType().print(PrintWithColorRAII(OS, TypeColor).getOS());
      PrintWithColorRAII(OS, TypeColor) << "'";
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitBooleanLiteralExpr(BooleanLiteralExpr *E) {
    printCommon(E, "boolean_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << (E->getValue() ? "true" : "false")
      << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
      PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitStringLiteralExpr(StringLiteralExpr *E) {
    printCommon(E, "string_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " encoding="
      << getStringLiteralExprEncodingString(E->getEncoding())
      << " value=" << QuotedString(E->getValue())
      << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
      PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E) {
    printCommon(E, "interpolated_string_literal_expr");
    
    // Print the trailing quote location
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      auto TQL = E->getTrailingQuoteLoc();
      if (TQL.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " trailing_quote_loc=";
        TQL.print(PrintWithColorRAII(OS, LocationColor).getOS(),
                  Ctx.SourceMgr);
      }
    }
    PrintWithColorRAII(OS, LiteralValueColor)
      << " literal_capacity="
      << E->getLiteralCapacity() << " interpolation_count="
      << E->getInterpolationCount() << '\n';
    printRec(E->getAppendingExpr());
    printSemanticExpr(E->getSemanticExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *E) {
    printCommon(E, "magic_identifier_literal_expr")
      << " kind=" << getMagicIdentifierLiteralExprKindString(E->getKind());

    if (E->isString()) {
      OS << " encoding="
         << getStringLiteralExprEncodingString(E->getStringEncoding());
    }
    OS << " builtin_initializer=";
    E->getBuiltinInitializer().dump(OS);
    OS << " initializer=";
    E->getInitializer().dump(OS);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitObjectLiteralExpr(ObjectLiteralExpr *E) {
    printCommon(E, "object_literal") 
      << " kind='" << E->getLiteralKindPlainName() << "'";
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    printArgumentLabels(E->getArgumentLabels());
    OS << "\n";
    printRec(E->getArg());
    printSemanticExpr(E->getSemanticExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDiscardAssignmentExpr(DiscardAssignmentExpr *E) {
    printCommon(E, "discard_assignment_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDeclRefExpr(DeclRefExpr *E) {
    printCommon(E, "declref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getDeclRef());
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      PrintWithColorRAII(OS, AccessLevelColor)
        << " " << getAccessSemanticsString(E->getAccessSemantics());
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSuperRefExpr(SuperRefExpr *E) {
    printCommon(E, "super_ref_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTypeExpr(TypeExpr *E) {
    printCommon(E, "type_expr");
    PrintWithColorRAII(OS, TypeReprColor) << " typerepr='";
    if (E->getTypeRepr())
      E->getTypeRepr()->print(PrintWithColorRAII(OS, TypeReprColor).getOS());
    else
      PrintWithColorRAII(OS, TypeReprColor) << "<<NULL>>";
    PrintWithColorRAII(OS, TypeReprColor) << "'";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E) {
    printCommon(E, "other_constructor_ref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getDeclRef());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E) {
    printCommon(E, "overloaded_decl_ref_expr");
    PrintWithColorRAII(OS, IdentifierColor) << " name="
      << E->getDecls()[0]->getBaseName();
    PrintWithColorRAII(OS, ExprModifierColor)
      << " number_of_decls=" << E->getDecls().size()
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind())
      << " decls=[\n";
    interleave(E->getDecls(),
               [&](ValueDecl *D) {
                 OS.indent(Indent + 2);
                 D->dumpRef(PrintWithColorRAII(OS, DeclModifierColor).getOS());
               },
               [&] { PrintWithColorRAII(OS, DeclModifierColor) << ",\n"; });
    PrintWithColorRAII(OS, ExprModifierColor) << "]";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    printCommon(E, "unresolved_decl_ref_expr");
    PrintWithColorRAII(OS, IdentifierColor) << " name=" << E->getName();
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *E) {
    printCommon(E, "unresolved_specialize_expr") << '\n';
    printRec(E->getSubExpr());
    for (TypeLoc T : E->getUnresolvedParams()) {
      OS << '\n';
      printRec(T.getTypeRepr());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMemberRefExpr(MemberRefExpr *E) {
    printCommon(E, "member_ref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getMember());
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      PrintWithColorRAII(OS, AccessLevelColor)
        << " " << getAccessSemanticsString(E->getAccessSemantics());
    if (E->isSuper())
      OS << " super";

    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDynamicMemberRefExpr(DynamicMemberRefExpr *E) {
    printCommon(E, "dynamic_member_ref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    E->getMember().dump(OS);
    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E) {
    printCommon(E, "unresolved_member_expr")
      << " name='" << E->getName() << "'";
    printArgumentLabels(E->getArgumentLabels());
    if (E->getArgument()) {
      OS << '\n';
      printRec(E->getArgument());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDotSelfExpr(DotSelfExpr *E) {
    printCommon(E, "dot_self_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitParenExpr(ParenExpr *E) {
    printCommon(E, "paren_expr");
    if (E->hasTrailingClosure())
      OS << " trailing-closure";
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleExpr(TupleExpr *E) {
    printCommon(E, "tuple_expr");
    if (E->hasTrailingClosure())
      OS << " trailing-closure";

    if (E->hasElementNames()) {
      PrintWithColorRAII(OS, IdentifierColor) << " names=";

      interleave(E->getElementNames(),
                 [&](Identifier name) {
                   PrintWithColorRAII(OS, IdentifierColor)
                     << (name.empty()?"''":name.str());
                 },
                 [&] { PrintWithColorRAII(OS, IdentifierColor) << ","; });
    }

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      if (E->getElement(i))
        printRec(E->getElement(i));
      else
        OS.indent(Indent+2) << "<<tuple element default value>>";
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArrayExpr(ArrayExpr *E) {
    printCommon(E, "array_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    for (auto elt : E->getElements()) {
      OS << '\n';
      printRec(elt);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDictionaryExpr(DictionaryExpr *E) {
    printCommon(E, "dictionary_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    for (auto elt : E->getElements()) {
      OS << '\n';
      printRec(elt);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSubscriptExpr(SubscriptExpr *E) {
    printCommon(E, "subscript_expr");
    if (E->getAccessSemantics() != AccessSemantics::Ordinary)
      PrintWithColorRAII(OS, AccessLevelColor)
        << " " << getAccessSemanticsString(E->getAccessSemantics());
    if (E->isSuper())
      OS << " super";
    if (E->hasDecl()) {
      PrintWithColorRAII(OS, DeclColor) << " decl=";
      printDeclRef(E->getDecl());
    }
    printArgumentLabels(E->getArgumentLabels());
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitKeyPathApplicationExpr(KeyPathApplicationExpr *E) {
    printCommon(E, "keypath_application_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getKeyPath());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDynamicSubscriptExpr(DynamicSubscriptExpr *E) {
    printCommon(E, "dynamic_subscript_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getMember());
    printArgumentLabels(E->getArgumentLabels());
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedDotExpr(UnresolvedDotExpr *E) {
    printCommon(E, "unresolved_dot_expr")
      << " field '" << E->getName() << "'";
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    if (E->getBase()) {
      OS << '\n';
      printRec(E->getBase());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleElementExpr(TupleElementExpr *E) {
    printCommon(E, "tuple_element_expr")
      << " field #" << E->getFieldNumber() << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDestructureTupleExpr(DestructureTupleExpr *E) {
    printCommon(E, "destructure_tuple_expr");
    OS << " destructured=";
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    Indent += 2;
    for (auto *elt : E->getDestructuredElements()) {
      OS << "\n";
      printRec(elt);
    }
    Indent -= 2;
    PrintWithColorRAII(OS, ParenthesisColor) << ")\n";
    printRec(E->getSubExpr());
    OS << "\n";
    printRec(E->getResultExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedTypeConversionExpr(UnresolvedTypeConversionExpr *E) {
    printCommon(E, "unresolvedtype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFunctionConversionExpr(FunctionConversionExpr *E) {
    printCommon(E, "function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *E){
    printCommon(E, "covariant_function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCovariantReturnConversionExpr(CovariantReturnConversionExpr *E){
    printCommon(E, "covariant_return_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitImplicitlyUnwrappedFunctionConversionExpr(
      ImplicitlyUnwrappedFunctionConversionExpr *E) {
    printCommon(E, "implicitly_unwrapped_function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnderlyingToOpaqueExpr(UnderlyingToOpaqueExpr *E){
    printCommon(E, "underlying_to_opaque_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitErasureExpr(ErasureExpr *E) {
    printCommon(E, "erasure_expr") << '\n';
    for (auto conf : E->getConformances()) {
      printRec(conf);
      OS << '\n';
    }
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAnyHashableErasureExpr(AnyHashableErasureExpr *E) {
    printCommon(E, "any_hashable_erasure_expr") << '\n';
    printRec(E->getConformance());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitConditionalBridgeFromObjCExpr(ConditionalBridgeFromObjCExpr *E) {
    printCommon(E, "conditional_bridge_from_objc_expr") << " conversion=";
    printDeclRef(E->getConversion());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBridgeFromObjCExpr(BridgeFromObjCExpr *E) {
    printCommon(E, "bridge_from_objc_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBridgeToObjCExpr(BridgeToObjCExpr *E) {
    printCommon(E, "bridge_to_objc_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLoadExpr(LoadExpr *E) {
    printCommon(E, "load_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMetatypeConversionExpr(MetatypeConversionExpr *E) {
    printCommon(E, "metatype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCollectionUpcastConversionExpr(CollectionUpcastConversionExpr *E) {
    printCommon(E, "collection_upcast_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    if (auto keyConversion = E->getKeyConversion()) {
      OS << '\n';
      printRecLabeled(keyConversion.Conversion, "key_conversion");
    }
    if (auto valueConversion = E->getValueConversion()) {
      OS << '\n';
      printRecLabeled(valueConversion.Conversion, "value_conversion");
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDerivedToBaseExpr(DerivedToBaseExpr *E) {
    printCommon(E, "derived_to_base_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E) {
    printCommon(E, "archetype_to_super_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInjectIntoOptionalExpr(InjectIntoOptionalExpr *E) {
    printCommon(E, "inject_into_optional") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitClassMetatypeToObjectExpr(ClassMetatypeToObjectExpr *E) {
    printCommon(E, "class_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitExistentialMetatypeToObjectExpr(ExistentialMetatypeToObjectExpr *E) {
    printCommon(E, "existential_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitProtocolMetatypeToObjectExpr(ProtocolMetatypeToObjectExpr *E) {
    printCommon(E, "protocol_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInOutToPointerExpr(InOutToPointerExpr *E) {
    printCommon(E, "inout_to_pointer")
      << (E->isNonAccessing() ? " nonaccessing" : "") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArrayToPointerExpr(ArrayToPointerExpr *E) {
    printCommon(E, "array_to_pointer")
      << (E->isNonAccessing() ? " nonaccessing" : "") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitStringToPointerExpr(StringToPointerExpr *E) {
    printCommon(E, "string_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitPointerToPointerExpr(PointerToPointerExpr *E) {
    printCommon(E, "pointer_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForeignObjectConversionExpr(ForeignObjectConversionExpr *E) {
    printCommon(E, "foreign_object_conversion") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnevaluatedInstanceExpr(UnevaluatedInstanceExpr *E) {
    printCommon(E, "unevaluated_instance") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitInOutExpr(InOutExpr *E) {
    printCommon(E, "inout_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitVarargExpansionExpr(VarargExpansionExpr *E) {
    printCommon(E, "vararg_expansion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitForceTryExpr(ForceTryExpr *E) {
    printCommon(E, "force_try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOptionalTryExpr(OptionalTryExpr *E) {
    printCommon(E, "optional_try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTryExpr(TryExpr *E) {
    printCommon(E, "try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitSequenceExpr(SequenceExpr *E) {
    printCommon(E, "sequence_expr");
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      printRec(E->getElement(i));
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCaptureListExpr(CaptureListExpr *E) {
    printCommon(E, "capture_list");
    for (auto capture : E->getCaptureList()) {
      OS << '\n';
      Indent += 2;
      printRec(capture.Var);
      printRec(capture.Init);
      Indent -= 2;
    }
    printRec(E->getClosureBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  llvm::raw_ostream &printClosure(AbstractClosureExpr *E, char const *name) {
    printCommon(E, name);
    PrintWithColorRAII(OS, DiscriminatorColor)
      << " discriminator=" << E->getDiscriminator();
    if (!E->getCaptureInfo().isTrivial()) {
      OS << " ";
      E->getCaptureInfo().print(PrintWithColorRAII(OS, CapturesColor).getOS());
    }
    // Printing a function type doesn't indicate whether it's escaping because it doesn't 
    // matter in 99% of contexts. AbstractClosureExpr nodes are one of the only exceptions.
    if (auto Ty = GetTypeOfExpr(E))
      if (!Ty->getAs<AnyFunctionType>()->getExtInfo().isNoEscape())
        PrintWithColorRAII(OS, ClosureModifierColor) << " escaping";

    return OS;
  }

  void visitClosureExpr(ClosureExpr *E) {
    printClosure(E, "closure_expr");
    if (E->hasSingleExpressionBody())
      PrintWithColorRAII(OS, ClosureModifierColor) << " single-expression";

    if (E->getParameters()) {
      OS << '\n';
      PrintDecl(OS, Indent+2).printParameterList(E->getParameters(), &E->getASTContext());
    }

    OS << '\n';
    if (E->hasSingleExpressionBody()) {
      printRec(E->getSingleExpressionBody());
    } else {
      printRec(E->getBody(), E->getASTContext());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAutoClosureExpr(AutoClosureExpr *E) {
    printClosure(E, "autoclosure_expr") << '\n';

    if (E->getParameters()) {
      OS << '\n';
      PrintDecl(OS, Indent+2).printParameterList(E->getParameters(), &E->getASTContext());
    }

    OS << '\n';
    printRec(E->getSingleExpressionBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDynamicTypeExpr(DynamicTypeExpr *E) {
    printCommon(E, "metatype_expr");
    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOpaqueValueExpr(OpaqueValueExpr *E) {
    printCommon(E, "opaque_value_expr") << " @ " << (void*)E;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDefaultArgumentExpr(DefaultArgumentExpr *E) {
    printCommon(E, "default_argument_expr");
    OS << " default_args_owner=";
    E->getDefaultArgsOwner().dump(OS);
    OS << " param=" << E->getParamIndex();
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCallerDefaultArgumentExpr(CallerDefaultArgumentExpr *E) {
    printCommon(E, "caller_default_argument_expr");
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printArgumentLabels(ArrayRef<Identifier> argLabels) {
    PrintWithColorRAII(OS, ArgumentsColor) << " arg_labels=";
    for (auto label : argLabels) {
      PrintWithColorRAII(OS, ArgumentsColor)
        << (label.empty() ? "_" : label.str()) << ":";
    }
  }

  void printApplyExpr(ApplyExpr *E, const char *NodeName) {
    printCommon(E, NodeName);
    if (E->isSuper())
      PrintWithColorRAII(OS, ExprModifierColor) << " super";
    if (E->isThrowsSet()) {
      PrintWithColorRAII(OS, ExprModifierColor)
        << (E->throws() ? " throws" : " nothrow");
    }
    if (auto call = dyn_cast<CallExpr>(E))
      printArgumentLabels(call->getArgumentLabels());

    OS << '\n';
    printRec(E->getFn());
    OS << '\n';
    printRec(E->getArg());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCallExpr(CallExpr *E) {
    printApplyExpr(E, "call_expr");
  }
  void visitPrefixUnaryExpr(PrefixUnaryExpr *E) {
    printApplyExpr(E, "prefix_unary_expr");
  }
  void visitPostfixUnaryExpr(PostfixUnaryExpr *E) {
    printApplyExpr(E, "postfix_unary_expr");
  }
  void visitBinaryExpr(BinaryExpr *E) {
    printApplyExpr(E, "binary_expr");
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E) {
    printApplyExpr(E, "dot_syntax_call_expr");
  }
  void visitConstructorRefCallExpr(ConstructorRefCallExpr *E) {
    printApplyExpr(E, "constructor_ref_call_expr");
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E) {
    printCommon(E, "dot_syntax_base_ignored") << '\n';
    printRec(E->getLHS());
    OS << '\n';
    printRec(E->getRHS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printExplicitCastExpr(ExplicitCastExpr *E, const char *name) {
    printCommon(E, name) << ' ';
    if (auto checkedCast = dyn_cast<CheckedCastExpr>(E))
      OS << getCheckedCastKindName(checkedCast->getCastKind()) << ' ';
    OS << "writtenType='";
    GetTypeOfTypeLoc(E->getCastTypeLoc()).print(OS);
    OS << "'\n";
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForcedCheckedCastExpr(ForcedCheckedCastExpr *E) {
    printExplicitCastExpr(E, "forced_checked_cast_expr");
  }
  void visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *E) {
    printExplicitCastExpr(E, "conditional_checked_cast_expr");
  }
  void visitIsExpr(IsExpr *E) {
    printExplicitCastExpr(E, "is_subtype_expr");
  }
  void visitCoerceExpr(CoerceExpr *E) {
    printExplicitCastExpr(E, "coerce_expr");
  }
  void visitArrowExpr(ArrowExpr *E) {
    printCommon(E, "arrow") << '\n';
    printRec(E->getArgsExpr());
    OS << '\n';
    printRec(E->getResultExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E) {
    printCommon(E, "rebind_self_in_constructor_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitIfExpr(IfExpr *E) {
    printCommon(E, "if_expr") << '\n';
    printRec(E->getCondExpr());
    OS << '\n';
    printRec(E->getThenExpr());
    OS << '\n';
    printRec(E->getElseExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAssignExpr(AssignExpr *E) {
    printCommon(E, "assign_expr") << '\n';
    printRec(E->getDest());
    OS << '\n';
    printRec(E->getSrc());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitEnumIsCaseExpr(EnumIsCaseExpr *E) {
    printCommon(E, "enum_is_case_expr") << ' ' <<
      E->getEnumElement()->getName() << "\n";
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedPatternExpr(UnresolvedPatternExpr *E) {
    printCommon(E, "unresolved_pattern_expr") << '\n';
    printRec(E->getSubPattern());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBindOptionalExpr(BindOptionalExpr *E) {
    printCommon(E, "bind_optional_expr")
      << " depth=" << E->getDepth() << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOptionalEvaluationExpr(OptionalEvaluationExpr *E) {
    printCommon(E, "optional_evaluation_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForceValueExpr(ForceValueExpr *E) {
    printCommon(E, "force_value_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOpenExistentialExpr(OpenExistentialExpr *E) {
    printCommon(E, "open_existential_expr") << '\n';
    printRec(E->getOpaqueValue());
    OS << '\n';
    printRec(E->getExistentialValue());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *E) {
    printCommon(E, "make_temporarily_escapable_expr") << '\n';
    printRec(E->getOpaqueValue());
    OS << '\n';
    printRec(E->getNonescapingClosureValue());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitEditorPlaceholderExpr(EditorPlaceholderExpr *E) {
    printCommon(E, "editor_placeholder_expr") << ' ';

    // Print the trailing angle bracket location
    if (auto Ty = GetTypeOfExpr(E)) {
      auto &Ctx = Ty->getASTContext();
      auto TABL = E->getTrailingAngleBracketLoc();
      if (TABL.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " trailing_angle_bracket_loc=";
        TABL.print(PrintWithColorRAII(OS, LocationColor).getOS(),
                   Ctx.SourceMgr);
      }
    }
    OS << '\n';
    auto *TyR = E->getTypeLoc().getTypeRepr();
    auto *ExpTyR = E->getTypeForExpansion();
    if (TyR)
      printRec(TyR);
    if (ExpTyR && ExpTyR != TyR) {
      OS << '\n';
      printRec(ExpTyR);
    }
    printSemanticExpr(E->getSemanticExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLazyInitializerExpr(LazyInitializerExpr *E) {
    printCommon(E, "lazy_initializer_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitObjCSelectorExpr(ObjCSelectorExpr *E) {
    printCommon(E, "objc_selector_expr");
    OS << " kind=" << getObjCSelectorExprKindString(E->getSelectorKind());
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    printDeclRef(E->getMethod());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitKeyPathExpr(KeyPathExpr *E) {
    printCommon(E, "keypath_expr");
    if (E->isObjC())
      OS << " objc";

    OS << '\n';
    Indent += 2;
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << "components";
    OS.indent(Indent + 2);
    for (unsigned i : indices(E->getComponents())) {
      auto &component = E->getComponents()[i];
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      switch (component.getKind()) {
      case KeyPathExpr::Component::Kind::Invalid:
        PrintWithColorRAII(OS, ASTNodeColor) << "invalid";
        break;

      case KeyPathExpr::Component::Kind::OptionalChain:
        PrintWithColorRAII(OS, ASTNodeColor) << "optional_chain";
        break;
        
      case KeyPathExpr::Component::Kind::OptionalForce:
        PrintWithColorRAII(OS, ASTNodeColor) << "optional_force";
        break;
        
      case KeyPathExpr::Component::Kind::OptionalWrap:
        PrintWithColorRAII(OS, ASTNodeColor) << "optional_wrap";
        break;
        
      case KeyPathExpr::Component::Kind::Property:
        PrintWithColorRAII(OS, ASTNodeColor) << "property";
        PrintWithColorRAII(OS, DeclColor) << " decl=";
        printDeclRef(component.getDeclRef());
        break;
      
      case KeyPathExpr::Component::Kind::Subscript:
        PrintWithColorRAII(OS, ASTNodeColor) << "subscript";
        PrintWithColorRAII(OS, DeclColor) << " decl='";
        printDeclRef(component.getDeclRef());
        PrintWithColorRAII(OS, DeclColor) << "'";
        break;
      
      case KeyPathExpr::Component::Kind::UnresolvedProperty:
        PrintWithColorRAII(OS, ASTNodeColor) << "unresolved_property";
        PrintWithColorRAII(OS, IdentifierColor)
          << " decl_name='" << component.getUnresolvedDeclName() << "'";
        break;
        
      case KeyPathExpr::Component::Kind::UnresolvedSubscript:
        PrintWithColorRAII(OS, ASTNodeColor) << "unresolved_subscript";
        printArgumentLabels(component.getSubscriptLabels());
        break;
      case KeyPathExpr::Component::Kind::Identity:
        PrintWithColorRAII(OS, ASTNodeColor) << "identity";
        break;

      case KeyPathExpr::Component::Kind::TupleElement:
        PrintWithColorRAII(OS, ASTNodeColor) << "tuple_element ";
        PrintWithColorRAII(OS, DiscriminatorColor)
          << "#" << component.getTupleIndex();
        break;
      }
      PrintWithColorRAII(OS, TypeColor)
        << " type='" << GetTypeOfKeyPathComponent(E, i) << "'";
      if (auto indexExpr = component.getIndexExpr()) {
        OS << '\n';
        Indent += 2;
        printRec(indexExpr);
        Indent -= 2;
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;

    if (auto stringLiteral = E->getObjCStringLiteralExpr()) {
      OS << '\n';
      printRecLabeled(stringLiteral, "objc_string_literal");
    }
    if (!E->isObjC()) {
      if (auto root = E->getParsedRoot()) {
        OS << "\n";
        printRecLabeled(root, "parsed_root");
      }
      if (auto path = E->getParsedPath()) {
        OS << "\n";
        printRecLabeled(path, "parsed_path");
      }
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitKeyPathDotExpr(KeyPathDotExpr *E) {
    printCommon(E, "key_path_dot_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTapExpr(TapExpr *E) {
    printCommon(E, "tap_expr");
    PrintWithColorRAII(OS, DeclColor) << " var=";
    printDeclRef(E->getVar());
    OS << '\n';

    printRec(E->getSubExpr());
    OS << '\n';

    printRec(E->getBody(), E->getVar()->getDeclContext()->getASTContext());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
};

} // end anonymous namespace

void Expr::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

void Expr::dump(raw_ostream &OS,
                 llvm::function_ref<Type(const Expr *)> getTypeOfExpr,
                 llvm::function_ref<Type(const TypeLoc &)> getTypeOfTypeLoc,
                 llvm::function_ref<Type(const KeyPathExpr *E, unsigned index)> getTypeOfKeyPathComponent,
                 unsigned Indent) const {
  PrintExpr(OS, getTypeOfExpr, getTypeOfTypeLoc, getTypeOfKeyPathComponent, Indent)
      .visit(const_cast<Expr *>(this));
}

void Expr::dump(raw_ostream &OS, unsigned Indent) const {
  auto getTypeOfExpr = [](const Expr *E) -> Type { return E->getType(); };
  auto getTypeOfTypeLoc = [](const TypeLoc &TL) -> Type {
    return TL.getType();
  };
  auto getTypeOfKeyPathComponent = [](const KeyPathExpr *E, unsigned index) -> Type {
    return E->getComponents()[index].getComponentType();
  };
  dump(OS, getTypeOfExpr, getTypeOfTypeLoc, getTypeOfKeyPathComponent, Indent);
}

void Expr::print(ASTPrinter &Printer, const PrintOptions &Opts) const {
  // FIXME: Fully use the ASTPrinter.
  llvm::SmallString<128> Str;
  llvm::raw_svector_ostream OS(Str);
  dump(OS);
  Printer << OS.str();
}

//===----------------------------------------------------------------------===//
// Printing for TypeRepr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
class PrintTypeRepr : public TypeReprVisitor<PrintTypeRepr> {
public:
  raw_ostream &OS;
  unsigned Indent;

  PrintTypeRepr(raw_ostream &os, unsigned indent)
    : OS(os), Indent(indent) { }

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Expr *E) { E->dump(OS, Indent + 2); }
  void printRec(TypeRepr *T) { PrintTypeRepr(OS, Indent + 2).visit(T); }

  raw_ostream &printCommon(const char *Name) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, TypeReprColor) << Name;
    return OS;
  }

  void visitErrorTypeRepr(ErrorTypeRepr *T) {
    printCommon("type_error");
  }

  void visitAttributedTypeRepr(AttributedTypeRepr *T) {
    printCommon("type_attributed") << " attrs=";
    T->printAttrs(OS);
    OS << '\n';
    printRec(T->getTypeRepr());
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    printCommon("type_ident");
    Indent += 2;
    for (auto comp : T->getComponentRange()) {
      OS << '\n';
      printCommon("component");
      PrintWithColorRAII(OS, IdentifierColor)
        << " id='" << comp->getIdentifier() << '\'';
      OS << " bind=";
      if (comp->isBound())
        comp->getBoundDecl()->dumpRef(OS);
      else OS << "none";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      if (auto GenIdT = dyn_cast<GenericIdentTypeRepr>(comp)) {
        for (auto genArg : GenIdT->getGenericArgs()) {
          OS << '\n';
          printRec(genArg);
        }
      }
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;
  }

  void visitFunctionTypeRepr(FunctionTypeRepr *T) {
    printCommon("type_function");
    OS << '\n'; printRec(T->getArgsTypeRepr());
    if (T->throws())
      OS << " throws ";
    OS << '\n'; printRec(T->getResultTypeRepr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitArrayTypeRepr(ArrayTypeRepr *T) {
    printCommon("type_array") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDictionaryTypeRepr(DictionaryTypeRepr *T) {
    printCommon("type_dictionary") << '\n';
    printRec(T->getKey());
    OS << '\n';
    printRec(T->getValue());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTupleTypeRepr(TupleTypeRepr *T) {
    printCommon("type_tuple");

    if (T->hasElementNames()) {
      OS << " names=";
      for (unsigned i = 0, end = T->getNumElements(); i != end; ++i) {
        if (i) OS << ",";
        auto name = T->getElementName(i);
        if (T->isNamedParameter(i))
          OS << (name.empty() ? "_" : "_ " + name.str());
        else
          OS << (name.empty() ? "''" : name.str());
      }
    }

    for (auto elem : T->getElements()) {
      OS << '\n';
      printRec(elem.Type);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCompositionTypeRepr(CompositionTypeRepr *T) {
    printCommon("type_composite");
    for (auto elem : T->getTypes()) {
      OS << '\n';
      printRec(elem);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMetatypeTypeRepr(MetatypeTypeRepr *T) {
    printCommon("type_metatype") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitProtocolTypeRepr(ProtocolTypeRepr *T) {
    printCommon("type_protocol") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitInOutTypeRepr(InOutTypeRepr *T) {
    printCommon("type_inout") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  
  void visitSharedTypeRepr(SharedTypeRepr *T) {
    printCommon("type_shared") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOwnedTypeRepr(OwnedTypeRepr *T) {
    printCommon("type_owned") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
};

} // end anonymous namespace

void PrintDecl::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void PrintExpr::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void PrintPattern::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void TypeRepr::dump() const {
  PrintTypeRepr(llvm::errs(), 0).visit(const_cast<TypeRepr*>(this));
  llvm::errs() << '\n';
}

// Recursive helpers to avoid infinite recursion for recursive protocol
// conformances.
static void dumpProtocolConformanceRec(
    const ProtocolConformance *conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited);

static void dumpProtocolConformanceRefRec(
    const ProtocolConformanceRef conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  if (conformance.isInvalid()) {
    out.indent(indent) << "(invalid_conformance)";
  } else if (conformance.isConcrete()) {
    dumpProtocolConformanceRec(conformance.getConcrete(), out, indent, visited);
  } else {
    out.indent(indent) << "(abstract_conformance protocol="
                       << conformance.getAbstract()->getName();
    PrintWithColorRAII(out, ParenthesisColor) << ')';
  }
}

static void dumpProtocolConformanceRec(
    const ProtocolConformance *conformance, llvm::raw_ostream &out,
    unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  // A recursive conformance shouldn't have its contents printed, or there's
  // infinite recursion. (This also avoids printing things that occur multiple
  // times in a conformance hierarchy.)
  auto shouldPrintDetails = visited.insert(conformance).second;

  auto printCommon = [&](StringRef kind) {
    out.indent(indent);
    PrintWithColorRAII(out, ParenthesisColor) << '(';
    out << kind << "_conformance type=" << conformance->getType()
        << " protocol=" << conformance->getProtocol()->getName();

    if (!shouldPrintDetails)
      out << " (details printed above)";
  };

  switch (conformance->getKind()) {
  case ProtocolConformanceKind::Normal: {
    auto normal = cast<NormalProtocolConformance>(conformance);

    printCommon("normal");
    if (!shouldPrintDetails)
      break;

    // Maybe print information about the conforming context?
    if (normal->isLazilyLoaded()) {
      out << " lazy";
    } else {
      normal->forEachTypeWitness(
          nullptr,
          [&](const AssociatedTypeDecl *req, Type ty,
              const TypeDecl *) -> bool {
            out << '\n';
            out.indent(indent + 2);
            PrintWithColorRAII(out, ParenthesisColor) << '(';
            out << "assoc_type req=" << req->getName() << " type=";
            PrintWithColorRAII(out, TypeColor) << Type(ty->getDesugaredType());
            PrintWithColorRAII(out, ParenthesisColor) << ')';
            return false;
          });
      normal->forEachValueWitness(nullptr, [&](const ValueDecl *req,
                                               Witness witness) {
        out << '\n';
        out.indent(indent + 2);
        PrintWithColorRAII(out, ParenthesisColor) << '(';
        out << "value req=" << req->getFullName() << " witness=";
        if (!witness) {
          out << "(none)";
        } else if (witness.getDecl() == req) {
          out << "(dynamic)";
        } else {
          witness.getDecl()->dumpRef(out);
        }
        PrintWithColorRAII(out, ParenthesisColor) << ')';
      });

      for (auto sigConf : normal->getSignatureConformances()) {
        out << '\n';
        dumpProtocolConformanceRefRec(sigConf, out, indent + 2, visited);
      }
    }

    if (auto condReqs = normal->getConditionalRequirementsIfAvailableOrCached(
            /*computeIfPossible=*/false)) {
      for (auto requirement : *condReqs) {
        out << '\n';
        out.indent(indent + 2);
        requirement.dump(out);
      }
    } else {
      out << '\n';
      out.indent(indent + 2);
      out << "(conditional requirements unable to be computed)";
    }
    break;
  }

  case ProtocolConformanceKind::Self: {
    printCommon("self");
    break;
  }

  case ProtocolConformanceKind::Inherited: {
    auto conf = cast<InheritedProtocolConformance>(conformance);
    printCommon("inherited");
    if (!shouldPrintDetails)
      break;

    out << '\n';
    dumpProtocolConformanceRec(conf->getInheritedConformance(), out, indent + 2,
                               visited);
    break;
  }

  case ProtocolConformanceKind::Specialized: {
    auto conf = cast<SpecializedProtocolConformance>(conformance);
    printCommon("specialized");
    if (!shouldPrintDetails)
      break;

    out << '\n';
    dumpSubstitutionMapRec(conf->getSubstitutionMap(), out,
                           SubstitutionMap::DumpStyle::Full, indent + 2,
                           visited);
    out << '\n';
    if (auto condReqs = conf->getConditionalRequirementsIfAvailableOrCached(
            /*computeIfPossible=*/false)) {
      for (auto subReq : *condReqs) {
        out.indent(indent + 2);
        subReq.dump(out);
        out << '\n';
      }
    } else {
      out.indent(indent + 2);
      out << "(conditional requirements unable to be computed)\n";
    }
    dumpProtocolConformanceRec(conf->getGenericConformance(), out, indent + 2,
                               visited);
    break;
  }
  }

  PrintWithColorRAII(out, ParenthesisColor) << ')';
}

static void dumpSubstitutionMapRec(
    SubstitutionMap map, llvm::raw_ostream &out,
    SubstitutionMap::DumpStyle style, unsigned indent,
    llvm::SmallPtrSetImpl<const ProtocolConformance *> &visited) {
  auto *genericSig = map.getGenericSignature();
  out.indent(indent);

  auto printParen = [&](char p) {
    PrintWithColorRAII(out, ParenthesisColor) << p;
  };
  printParen('(');
  SWIFT_DEFER { printParen(')'); };
  out << "substitution_map generic_signature=";
  if (genericSig == nullptr) {
    out << "<nullptr>";
    return;
  }

  genericSig->print(out);
  auto genericParams = genericSig->getGenericParams();
  auto replacementTypes =
      static_cast<const SubstitutionMap &>(map).getReplacementTypesBuffer();
  for (unsigned i : indices(genericParams)) {
    if (style == SubstitutionMap::DumpStyle::Minimal) {
      out << " ";
    } else {
      out << "\n";
      out.indent(indent + 2);
    }
    printParen('(');
    out << "substitution ";
    genericParams[i]->print(out);
    out << " -> ";
    if (replacementTypes[i])
      replacementTypes[i]->print(out);
    else
      out << "<<unresolved concrete type>>";
    printParen(')');
  }
  // A minimal dump doesn't need the details about the conformances, a lot of
  // that info can be inferred from the signature.
  if (style == SubstitutionMap::DumpStyle::Minimal)
    return;

  auto conformances = map.getConformances();
  for (const auto &req : genericSig->getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;

    out << "\n";
    out.indent(indent + 2);
    printParen('(');
    out << "conformance type=";
    req.getFirstType()->print(out);
    out << "\n";
    dumpProtocolConformanceRefRec(conformances.front(), out, indent + 4,
                                  visited);

    printParen(')');
    conformances = conformances.slice(1);
  }
}

void ProtocolConformanceRef::dump() const {
  dump(llvm::errs());
  llvm::errs() << '\n';
}

void ProtocolConformanceRef::dump(llvm::raw_ostream &out,
                                  unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRefRec(*this, out, indent, visited);
}
void ProtocolConformance::dump() const {
  auto &out = llvm::errs();
  dump(out);
  out << '\n';
}

void ProtocolConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpProtocolConformanceRec(this, out, indent, visited);
}

void SubstitutionMap::dump(llvm::raw_ostream &out, DumpStyle style,
                           unsigned indent) const {
  llvm::SmallPtrSet<const ProtocolConformance *, 8> visited;
  dumpSubstitutionMapRec(*this, out, style, indent, visited);
}

void SubstitutionMap::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

//===----------------------------------------------------------------------===//
// Dumping for Types.
//===----------------------------------------------------------------------===//

namespace {
  class PrintType : public TypeVisitor<PrintType, void, StringRef> {
    raw_ostream &OS;
    unsigned Indent;

    raw_ostream &printCommon(StringRef label, StringRef name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      if (!label.empty()) {
        PrintWithColorRAII(OS, TypeFieldColor) << label;
        OS << "=";
      }

      PrintWithColorRAII(OS, TypeColor) << name;
      return OS;
    }

    // Print a single flag.
    raw_ostream &printFlag(StringRef name) {
      PrintWithColorRAII(OS, TypeFieldColor) << " " << name;
      return OS;
    }

    // Print a single flag if it is set.
    raw_ostream &printFlag(bool isSet, StringRef name) {
      if (isSet)
        printFlag(name);

      return OS;
    }

    // Print a field with a value.
    template<typename T>
    raw_ostream &printField(StringRef name, const T &value) {
      OS << " ";
      PrintWithColorRAII(OS, TypeFieldColor) << name;
      OS << "=" << value;
      return OS;
    }

    void dumpParameterFlags(ParameterTypeFlags paramFlags) {
      printFlag(paramFlags.isVariadic(), "vararg");
      printFlag(paramFlags.isAutoClosure(), "autoclosure");
      switch (paramFlags.getValueOwnership()) {
      case ValueOwnership::Default: break;
      case ValueOwnership::Owned: printFlag("owned"); break;
      case ValueOwnership::Shared: printFlag("shared"); break;
      case ValueOwnership::InOut: printFlag("inout"); break;
      }
    }

  public:
    PrintType(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) { }

    void printRec(Type type) {
      printRec("", type);
    }

    void printRec(StringRef label, Type type) {
      OS << "\n";

      if (type.isNull())
        OS << "<<null>>";
      else {
        Indent += 2;
        visit(type, label);
        Indent -=2;
      }
    }

#define TRIVIAL_TYPE_PRINTER(Class,Name)                        \
    void visit##Class##Type(Class##Type *T, StringRef label) {  \
      printCommon(label, #Name "_type") << ")";              \
    }

    void visitErrorType(ErrorType *T, StringRef label) {
      printCommon(label, "error_type");
      if (auto originalType = T->getOriginalType())
        printRec("original_type", originalType);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    TRIVIAL_TYPE_PRINTER(Unresolved, unresolved)

    void visitBuiltinIntegerType(BuiltinIntegerType *T, StringRef label) {
      printCommon(label, "builtin_integer_type");
      if (T->isFixedWidth())
        printField("bit_width", T->getFixedWidth());
      else
        printFlag("word_sized");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBuiltinFloatType(BuiltinFloatType *T, StringRef label) {
      printCommon(label, "builtin_float_type");
      printField("bit_width", T->getBitWidth());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    TRIVIAL_TYPE_PRINTER(BuiltinIntegerLiteral, builtin_integer_literal)
    TRIVIAL_TYPE_PRINTER(BuiltinRawPointer, builtin_raw_pointer)
    TRIVIAL_TYPE_PRINTER(BuiltinNativeObject, builtin_native_object)
    TRIVIAL_TYPE_PRINTER(BuiltinBridgeObject, builtin_bridge_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnknownObject, builtin_unknown_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnsafeValueBuffer, builtin_unsafe_value_buffer)
    TRIVIAL_TYPE_PRINTER(SILToken, sil_token)

    void visitBuiltinVectorType(BuiltinVectorType *T, StringRef label) {
      printCommon(label, "builtin_vector_type");
      printField("num_elements", T->getNumElements());
      printRec(T->getElementType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTypeAliasType(TypeAliasType *T, StringRef label) {
      printCommon(label, "type_alias_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());

      for (auto arg : T->getInnermostGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitParenType(ParenType *T, StringRef label) {
      printCommon(label, "paren_type");
      dumpParameterFlags(T->getParameterFlags());
      printRec(T->getUnderlyingType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTupleType(TupleType *T, StringRef label) {
      printCommon(label, "tuple_type");
      printField("num_elements", T->getNumElements());
      Indent += 2;
      for (const auto &elt : T->getElements()) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "tuple_type_elt";
        if (elt.hasName())
          printField("name", elt.getName().str());
        dumpParameterFlags(elt.getParameterFlags());
        printRec(elt.getType());
        OS << ")";
      }
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

#define REF_STORAGE(Name, name, ...) \
    void visit##Name##StorageType(Name##StorageType *T, StringRef label) { \
      printCommon(label, #name "_storage_type"); \
      printRec(T->getReferentType()); \
      PrintWithColorRAII(OS, ParenthesisColor) << ')'; \
    }
#include "swift/AST/ReferenceStorage.def"

    void visitEnumType(EnumType *T, StringRef label) {
      printCommon(label, "enum_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitStructType(StructType *T, StringRef label) {
      printCommon(label, "struct_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitClassType(ClassType *T, StringRef label) {
      printCommon(label, "class_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitProtocolType(ProtocolType *T, StringRef label) {
      printCommon(label, "protocol_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitMetatypeType(MetatypeType *T, StringRef label) {
      printCommon(label, "metatype_type");
      if (T->hasRepresentation())
        OS << " " << getMetatypeRepresentationString(T->getRepresentation());
      printRec(T->getInstanceType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitExistentialMetatypeType(ExistentialMetatypeType *T,
                                      StringRef label) {
      printCommon(label, "existential_metatype_type");
      if (T->hasRepresentation())
        OS << " " << getMetatypeRepresentationString(T->getRepresentation());
      printRec(T->getInstanceType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitModuleType(ModuleType *T, StringRef label) {
      printCommon(label, "module_type");
      printField("module", T->getModule()->getName());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDynamicSelfType(DynamicSelfType *T, StringRef label) {
      printCommon(label, "dynamic_self_type");
      printRec(T->getSelfType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    
    void printArchetypeCommon(ArchetypeType *T,
                              StringRef className,
                              StringRef label) {
      printCommon(label, className);
      printField("address", static_cast<void *>(T));
      printFlag(T->requiresClass(), "class");
      if (auto layout = T->getLayoutConstraint()) {
        OS << " layout=";
        layout->print(OS);
      }
      for (auto proto : T->getConformsTo())
        printField("conforms_to", proto->printRef());
      if (auto superclass = T->getSuperclass())
        printRec("superclass", superclass);
    }
    
    void printArchetypeNestedTypes(ArchetypeType *T) {
      Indent += 2;
      for (auto nestedType : T->getKnownNestedTypes()) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "nested_type";
        OS << "=";
        OS << nestedType.first.str() << " ";
        if (!nestedType.second) {
          PrintWithColorRAII(OS, TypeColor) << "<<unresolved>>";
        } else {
          PrintWithColorRAII(OS, TypeColor);
          OS << "=" << nestedType.second.getString();
        }
        OS << ")";
      }
      Indent -= 2;
    }

    void visitPrimaryArchetypeType(PrimaryArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "primary_archetype_type", label);
      printField("name", T->getFullName());
      OS << "\n";
      auto genericEnv = T->getGenericEnvironment();
      if (auto owningDC = genericEnv->getOwningDeclContext()) {
        owningDC->printContext(OS, Indent + 2);
      }
      printArchetypeNestedTypes(T);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitNestedArchetypeType(NestedArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "nested_archetype_type", label);
      printField("name", T->getFullName());
      printField("parent", T->getParent());
      printField("assoc_type", T->getAssocType()->printRef());
      printArchetypeNestedTypes(T);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOpenedArchetypeType(OpenedArchetypeType *T, StringRef label) {
      printArchetypeCommon(T, "opened_archetype_type", label);
      printRec("opened_existential", T->getOpenedExistentialType());
      printField("opened_existential_id", T->getOpenedExistentialID());
      printArchetypeNestedTypes(T);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOpaqueTypeArchetypeType(OpaqueTypeArchetypeType *T,
                                      StringRef label) {
      printArchetypeCommon(T, "opaque_type", label);
      printField("decl", T->getDecl()->getNamingDecl()->printRef());
      if (!T->getSubstitutions().empty()) {
        OS << '\n';
        SmallPtrSet<const ProtocolConformance *, 4> Dumped;
        dumpSubstitutionMapRec(T->getSubstitutions(), OS,
                               SubstitutionMap::DumpStyle::Full,
                               Indent + 2, Dumped);
      }
      printArchetypeNestedTypes(T);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitGenericTypeParamType(GenericTypeParamType *T, StringRef label) {
      printCommon(label, "generic_type_param_type");
      printField("depth", T->getDepth());
      printField("index", T->getIndex());
      if (auto decl = T->getDecl())
        printField("decl", decl->printRef());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDependentMemberType(DependentMemberType *T, StringRef label) {
      printCommon(label, "dependent_member_type");
      if (auto assocType = T->getAssocType()) {
        printField("assoc_type", assocType->printRef());
      } else {
        printField("name", T->getName().str());
      }
      printRec("base", T->getBase());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAnyFunctionParams(ArrayRef<AnyFunctionType::Param> params,
                                StringRef label) {
      printCommon(label, "function_params");
      printField("num_params", params.size());
      Indent += 2;
      for (const auto &param : params) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "param";
        if (param.hasLabel())
          printField("name", param.getLabel().str());
        dumpParameterFlags(param.getParameterFlags());
        printRec(param.getPlainType());
        OS << ")";
      }
      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAnyFunctionTypeCommon(AnyFunctionType *T, StringRef label,
                                    StringRef name) {
      printCommon(label, name);
      SILFunctionType::Representation representation =
        T->getExtInfo().getSILRepresentation();

      if (representation != SILFunctionType::Representation::Thick)
        printField("representation",
                   getSILFunctionTypeRepresentationString(representation));

      printFlag(!T->isNoEscape(), "escaping");
      printFlag(T->throws(), "throws");

      OS << "\n";
      Indent += 2;
      printAnyFunctionParams(T->getParams(), "input");
      Indent -=2;
      printRec("output", T->getResult());
    }

    void visitFunctionType(FunctionType *T, StringRef label) {
      printAnyFunctionTypeCommon(T, label, "function_type");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitGenericFunctionType(GenericFunctionType *T, StringRef label) {
      printAnyFunctionTypeCommon(T, label, "generic_function_type");
      // FIXME: generic signature dumping needs improvement
      OS << "\n";
      OS.indent(Indent + 2) << "(";
      printField("generic_sig", T->getGenericSignature()->getAsString());
      OS << ")";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILFunctionType(SILFunctionType *T, StringRef label) {
      printCommon(label, "sil_function_type");
      // FIXME: Print the structure of the type.
      printField("type", T->getString());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILBlockStorageType(SILBlockStorageType *T, StringRef label) {
      printCommon(label, "sil_block_storage_type");
      printRec(T->getCaptureType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSILBoxType(SILBoxType *T, StringRef label) {
      printCommon(label, "sil_box_type");
      // FIXME: Print the structure of the type.
      printField("type", T->getString());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitArraySliceType(ArraySliceType *T, StringRef label) {
      printCommon(label, "array_slice_type");
      printRec(T->getBaseType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitOptionalType(OptionalType *T, StringRef label) {
      printCommon(label, "optional_type");
      printRec(T->getBaseType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDictionaryType(DictionaryType *T, StringRef label) {
      printCommon(label, "dictionary_type");
      printRec("key", T->getKeyType());
      printRec("value", T->getValueType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitProtocolCompositionType(ProtocolCompositionType *T,
                                      StringRef label) {
      printCommon(label, "protocol_composition_type");
      if (T->hasExplicitAnyObject())
        OS << " any_object";
      for (auto proto : T->getMembers()) {
        printRec(proto);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitLValueType(LValueType *T, StringRef label) {
      printCommon(label, "lvalue_type");
      printRec(T->getObjectType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitInOutType(InOutType *T, StringRef label) {
      printCommon(label, "inout_type");
      printRec(T->getObjectType());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitUnboundGenericType(UnboundGenericType *T, StringRef label) {
      printCommon(label, "unbound_generic_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBoundGenericClassType(BoundGenericClassType *T, StringRef label) {
      printCommon(label, "bound_generic_class_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBoundGenericStructType(BoundGenericStructType *T,
                                     StringRef label) {
      printCommon(label, "bound_generic_struct_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitBoundGenericEnumType(BoundGenericEnumType *T, StringRef label) {
      printCommon(label, "bound_generic_enum_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTypeVariableType(TypeVariableType *T, StringRef label) {
      printCommon(label, "type_variable_type");
      printField("id", T->getID());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

#undef TRIVIAL_TYPE_PRINTER
  };
} // end anonymous namespace

void Type::dump() const {
  // Make sure to print type variables.
  dump(llvm::errs());
}

void Type::dump(raw_ostream &os, unsigned indent) const {
  // Make sure to print type variables.
  llvm::SaveAndRestore<bool> X(getPointer()->getASTContext().LangOpts.
                               DebugConstraintSolver, true);
  PrintType(os, indent).visit(*this, "");
  os << "\n";
}

void TypeBase::dump() const {
  // Make sure to print type variables.
  Type(const_cast<TypeBase *>(this)).dump();
}

void TypeBase::dump(raw_ostream &os, unsigned indent) const {
  auto &ctx = const_cast<TypeBase*>(this)->getASTContext();
  
  // Make sure to print type variables.
  llvm::SaveAndRestore<bool> X(ctx.LangOpts.DebugConstraintSolver, true);
  Type(const_cast<TypeBase *>(this)).dump(os, indent);
}

void GenericEnvironment::dump(raw_ostream &os) const {
  os << "Generic environment:\n";
  for (auto gp : getGenericParams()) {
    gp->dump(os);
    mapTypeIntoContext(gp)->dump(os);
  }
  os << "Generic parameters:\n";
  for (auto paramTy : getGenericParams())
    paramTy->dump(os);
}

void GenericEnvironment::dump() const {
  dump(llvm::errs());
}

StringRef swift::getAccessorKindString(AccessorKind value) {
  switch (value) {
#define ACCESSOR(ID)
#define SINGLETON_ACCESSOR(ID, KEYWORD) \
  case AccessorKind::ID: return #KEYWORD;
#include "swift/AST/AccessorKinds.def"
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}