//===--- ParseStmt.cpp - Swift Language Parser for Statements -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Statement Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Version.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Parse/SyntaxParsingContext.h"
#include "swift/Subsystems.h"
#include "swift/Syntax/TokenSyntax.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace swift::syntax;

/// isStartOfStmt - Return true if the current token starts a statement.
///
bool Parser::isStartOfStmt() {
  // This needs to be kept in sync with `Parser::parseStmt()`. If a new token
  // kind is accepted here as start of statement, it should also be handled in
  // `Parser::parseStmt()`.
  switch (Tok.getKind()) {
  default: return false;
  case tok::kw_return:
  case tok::kw_throw:
  case tok::kw_defer:
  case tok::kw_if:
  case tok::kw_guard:
  case tok::kw_while:
  case tok::kw_do:
  case tok::kw_repeat:
  case tok::kw_for:
  case tok::kw_break:
  case tok::kw_continue:
  case tok::kw_fallthrough:
  case tok::kw_switch:
  case tok::kw_case:
  case tok::kw_default:
  case tok::kw_yield:
  case tok::pound_assert:
  case tok::pound_if:
  case tok::pound_warning:
  case tok::pound_error:
  case tok::pound_sourceLocation:
    return true;

  case tok::pound_line:
    // #line at the start of a line is a directive, when within, it is an expr.
    return Tok.isAtStartOfLine();

  case tok::kw_try: {
    // "try" cannot actually start any statements, but we parse it there for
    // better recovery.
    Parser::BacktrackingScope backtrack(*this);
    consumeToken(tok::kw_try);
    return isStartOfStmt();
  }
      
  case tok::identifier: {
    // "identifier ':' for/while/do/switch" is a label on a loop/switch.
    if (!peekToken().is(tok::colon)) {
      // "yield" in the right context begins a yield statement.
      if (isContextualYieldKeyword()) {
        return true;
      }
      return false;
    }

    // To disambiguate other cases of "identifier :", which might be part of a
    // question colon expression or something else, we look ahead to the second
    // token.
    Parser::BacktrackingScope backtrack(*this);
    consumeToken(tok::identifier);
    consumeToken(tok::colon);
    // For better recovery, we just accept a label on any statement.  We reject
    // putting a label on something inappropriate in parseStmt().
    return isStartOfStmt();
  }

  case tok::at_sign: {
    // Might be a statement or case attribute. The only one of these we have
    // right now is `@unknown default`, so hardcode a check for an attribute
    // without any parens.
    if (!peekToken().is(tok::identifier))
      return false;
    Parser::BacktrackingScope backtrack(*this);
    consumeToken(tok::at_sign);
    consumeToken(tok::identifier);
    return isStartOfStmt();
  }
  }
}

ParserStatus Parser::parseExprOrStmt(ASTNode &Result) {
  if (Tok.is(tok::semi)) {
    SyntaxParsingContext ErrorCtxt(SyntaxContext, SyntaxContextKind::Stmt);
    diagnose(Tok, diag::illegal_semi_stmt)
      .fixItRemove(SourceRange(Tok.getLoc()));
    consumeToken();
    return makeParserError();
  }

  if (Tok.is(tok::pound) && Tok.isAtStartOfLine() &&
      peekToken().is(tok::code_complete)) {
    consumeToken();
    if (CodeCompletion)
      CodeCompletion->completeAfterPoundDirective();
    consumeToken(tok::code_complete);
    return makeParserCodeCompletionStatus();
  }

  if (isStartOfStmt()) {
    ParserResult<Stmt> Res = parseStmt();
    if (Res.isNonNull())
      Result = Res.get();
    return Res;
  }

  // Note that we're parsing a statement.
  StructureMarkerRAII ParsingStmt(*this, Tok.getLoc(),
                                  StructureMarkerKind::Statement);

  if (CodeCompletion)
    CodeCompletion->setExprBeginning(getParserPosition());

  if (Tok.is(tok::code_complete)) {
    auto *CCE = new (Context) CodeCompletionExpr(Tok.getLoc());
    Result = CCE;
    if (CodeCompletion)
      CodeCompletion->completeStmtOrExpr(CCE);
    SyntaxParsingContext ErrorCtxt(SyntaxContext, SyntaxContextKind::Stmt);
    consumeToken(tok::code_complete);
    return makeParserCodeCompletionStatus();
  }

  ParserResult<Expr> ResultExpr = parseExpr(diag::expected_expr);
  if (ResultExpr.isNonNull()) {
    Result = ResultExpr.get();
  } else if (!ResultExpr.hasCodeCompletion()) {
    // If we've consumed any tokens at all, build an error expression
    // covering the consumed range.
    SourceLoc startLoc = StructureMarkers.back().Loc;
    if (startLoc != Tok.getLoc()) {
      Result = new (Context) ErrorExpr(SourceRange(startLoc, PreviousLoc));
    }
  }

  if (ResultExpr.hasCodeCompletion() && CodeCompletion) {
    CodeCompletion->completeExpr();
  }

  return ResultExpr;
}

/// Returns whether the parser's current position is the start of a switch case,
/// given that we're in the middle of a switch already.
static bool isAtStartOfSwitchCase(Parser &parser,
                                  bool needsToBacktrack = true) {
  Optional<Parser::BacktrackingScope> backtrack;

  // Check for and consume attributes. The only valid attribute is `@unknown`
  // but that's a semantic restriction.
  while (parser.Tok.is(tok::at_sign)) {
    if (!parser.peekToken().is(tok::identifier))
      return false;

    if (needsToBacktrack && !backtrack)
      backtrack.emplace(parser);

    parser.consumeToken(tok::at_sign);
    parser.consumeIdentifier();
    if (parser.Tok.is(tok::l_paren))
      parser.skipSingle();
  }

  return parser.Tok.isAny(tok::kw_case, tok::kw_default);
}

bool Parser::isTerminatorForBraceItemListKind(BraceItemListKind Kind,
                                              ArrayRef<ASTNode> ParsedDecls) {
  switch (Kind) {
  case BraceItemListKind::Brace:
    return false;
  case BraceItemListKind::Case: {
    if (Tok.is(tok::pound_if)) {
      // Backtracking scopes are expensive, so avoid setting one up if possible.
      Parser::BacktrackingScope Backtrack(*this);
      // '#if' here could be to guard 'case:' or statements in cases.
      // If the next non-directive line starts with 'case' or 'default', it is
      // for 'case's.
      do {
        consumeToken();

        // just find the end of the line
        skipUntilTokenOrEndOfLine(tok::NUM_TOKENS);
      } while (Tok.isAny(tok::pound_if, tok::pound_elseif, tok::pound_else));
      return isAtStartOfSwitchCase(*this, /*needsToBacktrack*/false);
    }
    return isAtStartOfSwitchCase(*this);
  }
  case BraceItemListKind::TopLevelCode:
    // When parsing the top level executable code for a module, if we parsed
    // some executable code, then we're done.  We want to process (name bind,
    // type check, etc) decls one at a time to make sure that there are not
    // forward type references, etc.  There is an outer loop around the parser
    // that will reinvoke the parser at the top level on each statement until
    // EOF.  In contrast, it is ok to have forward references between classes,
    // functions, etc.
    for (auto I : ParsedDecls) {
      if (isa<TopLevelCodeDecl>(I.get<Decl*>()))
        // Only bail out if the next token is at the start of a line.  If we
        // don't, then we may accidentally allow things like "a = 1 b = 4".
        // FIXME: This is really dubious.  This will reject some things, but
        // allow other things we don't want.
        if (Tok.isAtStartOfLine())
          return true;
    }
    return false;
  case BraceItemListKind::TopLevelLibrary:
    return false;
  case BraceItemListKind::ActiveConditionalBlock:
  case BraceItemListKind::InactiveConditionalBlock:
    return Tok.isNot(tok::pound_else) && Tok.isNot(tok::pound_endif) &&
           Tok.isNot(tok::pound_elseif);
  }

  llvm_unreachable("Unhandled BraceItemListKind in switch.");
}

void Parser::consumeTopLevelDecl(ParserPosition BeginParserPosition,
                                 TopLevelCodeDecl *TLCD) {
  SourceLoc EndLoc = PreviousLoc;
  backtrackToPosition(BeginParserPosition);
  SourceLoc BeginLoc = Tok.getLoc();
  State->delayTopLevel(TLCD, {BeginLoc, EndLoc},
                       BeginParserPosition.PreviousLoc);

  // Skip the rest of the file to prevent the parser from constructing the AST
  // for it.  Forward references are not allowed at the top level.
  while (!Tok.is(tok::eof))
    consumeToken();
}

///   brace-item:
///     decl
///     expr
///     stmt
///   stmt:
///     ';'
///     stmt-assign
///     stmt-if
///     stmt-guard
///     stmt-for-c-style
///     stmt-for-each
///     stmt-switch
///     stmt-control-transfer
///  stmt-control-transfer:
///     stmt-return
///     stmt-break
///     stmt-continue
///     stmt-fallthrough
///   stmt-assign:
///     expr '=' expr
ParserStatus Parser::parseBraceItems(SmallVectorImpl<ASTNode> &Entries,
                                     BraceItemListKind Kind,
                                     BraceItemListKind ConditionalBlockKind) {
  bool isRootCtx = SyntaxContext->isRoot();
  SyntaxParsingContext ItemListContext(SyntaxContext,
                                       SyntaxKind::CodeBlockItemList);
  if (isRootCtx) {
    ItemListContext.setTransparent();
  }

  bool IsTopLevel = (Kind == BraceItemListKind::TopLevelCode) ||
                    (Kind == BraceItemListKind::TopLevelLibrary);
  bool isActiveConditionalBlock =
    ConditionalBlockKind == BraceItemListKind::ActiveConditionalBlock;
  bool isConditionalBlock = isActiveConditionalBlock ||
    ConditionalBlockKind == BraceItemListKind::InactiveConditionalBlock;

  // If we're not parsing an active #if block, form a new lexical scope.
  Optional<Scope> initScope;
  if (!isActiveConditionalBlock) {
    auto scopeKind =  IsTopLevel ? ScopeKind::TopLevel : ScopeKind::Brace;
    initScope.emplace(this, scopeKind,
                      ConditionalBlockKind ==
                        BraceItemListKind::InactiveConditionalBlock);
  }

  ParserStatus BraceItemsStatus;

  bool PreviousHadSemi = true;
  while ((IsTopLevel || Tok.isNot(tok::r_brace)) &&
         Tok.isNot(tok::pound_endif) &&
         Tok.isNot(tok::pound_elseif) &&
         Tok.isNot(tok::pound_else) &&
         Tok.isNot(tok::eof) &&
         Tok.isNot(tok::kw_sil) &&
         Tok.isNot(tok::kw_sil_scope) &&
         Tok.isNot(tok::kw_sil_stage) &&
         Tok.isNot(tok::kw_sil_vtable) &&
         Tok.isNot(tok::kw_sil_global) &&
         Tok.isNot(tok::kw_sil_witness_table) &&
         Tok.isNot(tok::kw_sil_default_witness_table) &&
         Tok.isNot(tok::kw_sil_property) &&
         (isConditionalBlock ||
          !isTerminatorForBraceItemListKind(Kind, Entries))) {

    SyntaxParsingContext NodeContext(SyntaxContext, SyntaxKind::CodeBlockItem);
    if (loadCurrentSyntaxNodeFromCache()) {
      continue;
    }

    if (Tok.is(tok::r_brace)) {
      SyntaxParsingContext ErrContext(SyntaxContext, SyntaxContextKind::Stmt);
      assert(IsTopLevel);
      diagnose(Tok, diag::extra_rbrace)
        .fixItRemove(Tok.getLoc());
      consumeToken();
      continue;
    }

    // Eat invalid tokens instead of allowing them to produce downstream errors.
    if (Tok.is(tok::unknown)) {
      SyntaxParsingContext ErrContext(SyntaxContext, SyntaxContextKind::Stmt);
      if (Tok.getText().startswith("\"\"\"")) {
        // This was due to unterminated multi-line string.
        IsInputIncomplete = true;
      }
      consumeToken();
      continue;
    }
           
    bool NeedParseErrorRecovery = false;
    ASTNode Result;

    // If the previous statement didn't have a semicolon and this new
    // statement doesn't start a line, complain.
    if (!PreviousHadSemi && !Tok.isAtStartOfLine()) {
      SourceLoc EndOfPreviousLoc = getEndOfPreviousLoc();
      diagnose(EndOfPreviousLoc, diag::statement_same_line_without_semi)
        .fixItInsert(EndOfPreviousLoc, ";");
      // FIXME: Add semicolon to the AST?
    }

    ParserPosition BeginParserPosition;
    if (isCodeCompletionFirstPass())
      BeginParserPosition = getParserPosition();

    // Parse the decl, stmt, or expression.
    PreviousHadSemi = false;
    if (Tok.is(tok::pound_if)) {
      auto IfConfigResult = parseIfConfig(
        [&](SmallVectorImpl<ASTNode> &Elements, bool IsActive) {
          parseBraceItems(Elements, Kind, IsActive
                            ? BraceItemListKind::ActiveConditionalBlock
                            : BraceItemListKind::InactiveConditionalBlock);
        });
      if (IfConfigResult.hasCodeCompletion() && isCodeCompletionFirstPass()) {
        consumeDecl(BeginParserPosition, None, IsTopLevel);
        return IfConfigResult;
      }
      BraceItemsStatus |= IfConfigResult;
      if (auto ICD = IfConfigResult.getPtrOrNull()) {
        Result = ICD;
        // Add the #if block itself
        Entries.push_back(ICD);

        for (auto &Entry : ICD->getActiveClauseElements()) {
          if (Entry.is<Decl *>() && isa<IfConfigDecl>(Entry.get<Decl *>()))
            // Don't hoist nested '#if'.
            continue;
          Entries.push_back(Entry);
          if (Entry.is<Decl *>())
            Entry.get<Decl *>()->setEscapedFromIfConfig(true);
        }
      } else {
        NeedParseErrorRecovery = true;
        continue;
      }
    } else if (Tok.is(tok::pound_line)) {
      ParserStatus Status = parseLineDirective(true);
      BraceItemsStatus |= Status;
      NeedParseErrorRecovery = Status.isError();
    } else if (Tok.is(tok::pound_sourceLocation)) {
      ParserStatus Status = parseLineDirective(false);
      BraceItemsStatus |= Status;
      NeedParseErrorRecovery = Status.isError();
    } else if (isStartOfDecl()) {
      SmallVector<Decl*, 8> TmpDecls;
      ParserResult<Decl> DeclResult = 
          parseDecl(IsTopLevel ? PD_AllowTopLevel : PD_Default,
                    [&](Decl *D) {TmpDecls.push_back(D);});
      BraceItemsStatus |= DeclResult;
      if (DeclResult.isParseError()) {
        NeedParseErrorRecovery = true;
        if (DeclResult.hasCodeCompletion() && IsTopLevel &&
            isCodeCompletionFirstPass()) {
          consumeDecl(BeginParserPosition, None, IsTopLevel);
          return DeclResult;
        }
      }
      Result = DeclResult.getPtrOrNull();
      Entries.append(TmpDecls.begin(), TmpDecls.end());
    } else if (IsTopLevel) {
      // If this is a statement or expression at the top level of the module,
      // Parse it as a child of a TopLevelCodeDecl.
      auto *TLCD = new (Context) TopLevelCodeDecl(CurDeclContext);
      ContextChange CC(*this, TLCD, &State->getTopLevelContext());
      SourceLoc StartLoc = Tok.getLoc();

      // Expressions can't begin with a closure literal at statement position.
      // This prevents potential ambiguities with trailing closure syntax.
      if (Tok.is(tok::l_brace)) {
        diagnose(Tok, diag::statement_begins_with_closure);
      }

      ParserStatus Status = parseExprOrStmt(Result);
      BraceItemsStatus |= Status;
      if (Status.hasCodeCompletion() && isCodeCompletionFirstPass()) {
        consumeTopLevelDecl(BeginParserPosition, TLCD);
        auto Brace = BraceStmt::create(Context, StartLoc, {}, PreviousLoc);
        TLCD->setBody(Brace);
        Entries.push_back(TLCD);
        return Status;
      }
      if (Status.isError())
        NeedParseErrorRecovery = true;
      else if (!allowTopLevelCode()) {
        diagnose(StartLoc,
                 Result.is<Stmt*>() ? diag::illegal_top_level_stmt
                                    : diag::illegal_top_level_expr);
      }

      if (!Result.isNull()) {
        // NOTE: this is a 'virtual' brace statement which does not have
        //       explicit '{' or '}', so the start and end locations should be
        //       the same as those of the result node
        auto Brace = BraceStmt::create(Context, Result.getStartLoc(),
                                       Result, Result.getEndLoc());
        TLCD->setBody(Brace);
        Entries.push_back(TLCD);
      }
    } else if (Tok.is(tok::kw_init) && isa<ConstructorDecl>(CurDeclContext)) {
      SourceLoc StartLoc = Tok.getLoc();
      auto CD = cast<ConstructorDecl>(CurDeclContext);
      // Hint at missing 'self.' or 'super.' then skip this statement.
      bool isSelf = !CD->isDesignatedInit() || !isa<ClassDecl>(CD->getParent());
      diagnose(StartLoc, diag::invalid_nested_init, isSelf)
        .fixItInsert(StartLoc, isSelf ? "self." : "super.");
      NeedParseErrorRecovery = true;
      BraceItemsStatus.setIsParseError();
    } else {
      ParserStatus ExprOrStmtStatus = parseExprOrStmt(Result);
      BraceItemsStatus |= ExprOrStmtStatus;
      if (ExprOrStmtStatus.isError())
        NeedParseErrorRecovery = true;
      if (!Result.isNull())
        Entries.push_back(Result);
    }

    if (!NeedParseErrorRecovery && Tok.is(tok::semi)) {
      PreviousHadSemi = true;
      if (auto *E = Result.dyn_cast<Expr*>())
        E->TrailingSemiLoc = consumeToken(tok::semi);
      else if (auto *S = Result.dyn_cast<Stmt*>())
        S->TrailingSemiLoc = consumeToken(tok::semi);
      else if (auto *D = Result.dyn_cast<Decl*>())
        D->TrailingSemiLoc = consumeToken(tok::semi);
      else
        assert(!Result && "Unsupported AST node");
    }

    if (NeedParseErrorRecovery) {
      SyntaxParsingContext TokenListCtxt(SyntaxContext,
                                         SyntaxKind::NonEmptyTokenList);
      // If we had a parse error, skip to the start of the next stmt, decl or
      // '{'.
      //
      // It would be ideal to stop at the start of the next expression (e.g.
      // "X = 4"), but distinguishing the start of an expression from the middle
      // of one is "hard".
      skipUntilDeclStmtRBrace(tok::l_brace);

      // If we have to recover, pretend that we had a semicolon; it's less
      // noisy that way.
      PreviousHadSemi = true;
    }
  }

  return BraceItemsStatus;
}

void Parser::parseTopLevelCodeDeclDelayed() {
  auto DelayedState = State->takeDelayedDeclState();
  assert(DelayedState.get() && "should have delayed state");

  auto BeginParserPosition = getParserPosition(DelayedState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(DelayedState->BodyEnd);

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeTokenWithoutFeedingReceiver();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that cannot go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to the beginning of the top-level code.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, DelayedState->takeScope());

  // Re-enter the top-level decl context.
  // FIXME: this can issue discriminators out-of-order?
  auto *TLCD = cast<TopLevelCodeDecl>(DelayedState->ParentContext);
  ContextChange CC(*this, TLCD, &State->getTopLevelContext());

  SourceLoc StartLoc = Tok.getLoc();
  ASTNode Result;

  // Expressions can't begin with a closure literal at statement position. This
  // prevents potential ambiguities with trailing closure syntax.
  if (Tok.is(tok::l_brace)) {
    diagnose(Tok, diag::statement_begins_with_closure);
  }

  parseExprOrStmt(Result);
  if (!Result.isNull()) {
    auto Brace = BraceStmt::create(Context, StartLoc, Result, Tok.getLoc());
    TLCD->setBody(Brace);
  }
}

/// Recover from a 'case' or 'default' outside of a 'switch' by consuming up to
/// the next ':'.
static ParserResult<Stmt> recoverFromInvalidCase(Parser &P) {
  assert(P.Tok.is(tok::kw_case) || P.Tok.is(tok::kw_default)
         && "not case or default?!");
  P.diagnose(P.Tok, diag::case_outside_of_switch, P.Tok.getText());
  P.skipUntil(tok::colon);
  // FIXME: Return an ErrorStmt?
  return nullptr;
}

ParserResult<Stmt> Parser::parseStmt() {
  AssertParserMadeProgressBeforeLeavingScopeRAII apmp(*this);

  SyntaxParsingContext LocalContext(SyntaxContext, SyntaxContextKind::Stmt);

  // Note that we're parsing a statement.
  StructureMarkerRAII ParsingStmt(*this, Tok.getLoc(),
                                  StructureMarkerKind::Statement);
  
  LabeledStmtInfo LabelInfo;
  
  // If this is a label on a loop/switch statement, consume it and pass it into
  // parsing logic below.
  if (Tok.is(tok::identifier) && peekToken().is(tok::colon)) {
    LabelInfo.Loc = consumeIdentifier(&LabelInfo.Name);
    consumeToken(tok::colon);
  }

  SourceLoc tryLoc;
  (void)consumeIf(tok::kw_try, tryLoc);

  // Claim contextual statement keywords now that we've committed
  // to parsing a statement.
  if (isContextualYieldKeyword()) {
    Tok.setKind(tok::kw_yield);
  }

  // This needs to handle everything that `Parser::isStartOfStmt()` accepts as
  // start of statement.
  switch (Tok.getKind()) {
  case tok::pound_line:
  case tok::pound_sourceLocation:
  case tok::pound_if:
  case tok::pound_error:
  case tok::pound_warning:
    assert((LabelInfo || tryLoc.isValid()) &&
           "unlabeled directives should be handled earlier");
    // Bailout, and let parseBraceItems() parse them.
    LLVM_FALLTHROUGH;
  default:
    diagnose(Tok, tryLoc.isValid() ? diag::expected_expr : diag::expected_stmt);
    if (Tok.is(tok::at_sign)) {
      // Recover from erroneously placed attribute.
      consumeToken(tok::at_sign);
      consumeIf(tok::identifier);
    }
    return nullptr;
  case tok::kw_return:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    return parseStmtReturn(tryLoc);
  case tok::kw_yield:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    return parseStmtYield(tryLoc);
  case tok::kw_throw:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    return parseStmtThrow(tryLoc);
  case tok::kw_defer:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtDefer();
  case tok::kw_if:
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtIf(LabelInfo);
  case tok::kw_guard:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtGuard();
  case tok::kw_while:
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtWhile(LabelInfo);
  case tok::kw_repeat:
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtRepeat(LabelInfo);
  case tok::kw_do:
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtDo(LabelInfo);
  case tok::kw_for:
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtForEach(LabelInfo);
  case tok::kw_switch:
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtSwitch(LabelInfo);
  /// 'case' and 'default' are only valid at the top level of a switch.
  case tok::kw_case:
  case tok::kw_default:
    return recoverFromInvalidCase(*this);
  case tok::kw_break:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtBreak();
  case tok::kw_continue:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtContinue();
  case tok::kw_fallthrough: {
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());

    SyntaxContext->setCreateSyntax(SyntaxKind::FallthroughStmt);
    return makeParserResult(
        new (Context) FallthroughStmt(consumeToken(tok::kw_fallthrough)));
  }
  case tok::pound_assert:
    if (LabelInfo) diagnose(LabelInfo.Loc, diag::invalid_label_on_stmt);
    if (tryLoc.isValid()) diagnose(tryLoc, diag::try_on_stmt, Tok.getText());
    return parseStmtPoundAssert();
  }
}

/// parseBraceItemList - A brace enclosed expression/statement/decl list.  For
/// example { 1; 4+5; } or { 1; 2 }.  Always occurs as part of some other stmt
/// or decl.
///
///   brace-item-list:
///     '{' brace-item* '}'
///
ParserResult<BraceStmt> Parser::parseBraceItemList(Diag<> ID) {
  if (Tok.isNot(tok::l_brace)) {
    diagnose(Tok, ID);

    // Attempt to recover by looking for a left brace on the same line
    if (!skipUntilTokenOrEndOfLine(tok::l_brace))
      return nullptr;
  }
  SyntaxParsingContext LocalContext(SyntaxContext, SyntaxKind::CodeBlock);
  SourceLoc LBLoc = consumeToken(tok::l_brace);

  SmallVector<ASTNode, 16> Entries;
  SourceLoc RBLoc;

  ParserStatus Status = parseBraceItems(Entries, BraceItemListKind::Brace,
                                        BraceItemListKind::Brace);
  if (parseMatchingToken(tok::r_brace, RBLoc,
                         diag::expected_rbrace_in_brace_stmt, LBLoc)) {
    // Synthesize a r-brace if the source doesn't have any.
    LocalContext.synthesize(tok::r_brace, LBLoc.getAdvancedLoc(1));
  }

  return makeParserResult(Status,
                          BraceStmt::create(Context, LBLoc, Entries, RBLoc));
}

/// parseStmtBreak
///
///   stmt-break:
///     'break' identifier?
///
ParserResult<Stmt> Parser::parseStmtBreak() {
  SyntaxContext->setCreateSyntax(SyntaxKind::BreakStmt);
  SourceLoc Loc = consumeToken(tok::kw_break);
  SourceLoc TargetLoc;
  Identifier Target;

  // If we have an identifier after this, which is not the start of another
  // stmt or decl, we assume it is the label to break to, unless there is a
  // line break.  There is ambiguity with expressions (e.g. "break x+y") but
  // since the expression after the break is dead, we don't feel bad eagerly
  // parsing this.
  if (Tok.is(tok::identifier) && !Tok.isAtStartOfLine() &&
      !isStartOfStmt() && !isStartOfDecl())
    TargetLoc = consumeIdentifier(&Target);

  return makeParserResult(new (Context) BreakStmt(Loc, Target, TargetLoc));
}

/// parseStmtContinue
///
///   stmt-continue:
///     'continue' identifier?
///
ParserResult<Stmt> Parser::parseStmtContinue() {
  SyntaxContext->setCreateSyntax(SyntaxKind::ContinueStmt);
  SourceLoc Loc = consumeToken(tok::kw_continue);
  SourceLoc TargetLoc;
  Identifier Target;

  // If we have an identifier after this, which is not the start of another
  // stmt or decl, we assume it is the label to continue to, unless there is a
  // line break.  There is ambiguity with expressions (e.g. "continue x+y") but
  // since the expression after the continue is dead, we don't feel bad eagerly
  // parsing this.
  if (Tok.is(tok::identifier) && !Tok.isAtStartOfLine() &&
      !isStartOfStmt() && !isStartOfDecl())
    TargetLoc = consumeIdentifier(&Target);

  return makeParserResult(new (Context) ContinueStmt(Loc, Target, TargetLoc));
}


/// parseStmtReturn
///
///   stmt-return:
///     'return' expr?
///   
ParserResult<Stmt> Parser::parseStmtReturn(SourceLoc tryLoc) {
  SyntaxContext->setCreateSyntax(SyntaxKind::ReturnStmt);
  SourceLoc ReturnLoc = consumeToken(tok::kw_return);

  if (Tok.is(tok::code_complete)) {
    auto CCE = new (Context) CodeCompletionExpr(SourceRange(Tok.getLoc()));
    auto Result = makeParserResult(new (Context) ReturnStmt(ReturnLoc, CCE));
    if (CodeCompletion) {
      CodeCompletion->completeReturnStmt(CCE);
    }
    Result.setHasCodeCompletion();
    consumeToken();
    return Result;
  }

  // Handle the ambiguity between consuming the expression and allowing the
  // enclosing stmt-brace to get it by eagerly eating it unless the return is
  // followed by a '}', ';', statement or decl start keyword sequence.
  if (Tok.isNot(tok::r_brace, tok::semi, tok::eof, tok::pound_if, 
                tok::pound_error, tok::pound_warning, tok::pound_endif,
                tok::pound_else, tok::pound_elseif) &&
      !isStartOfStmt() && !isStartOfDecl()) {
    SourceLoc ExprLoc = Tok.getLoc();

    // Issue a warning when the returned expression is on a different line than
    // the return keyword, but both have the same indentation.
    if (SourceMgr.getLineAndColumn(ReturnLoc).second ==
        SourceMgr.getLineAndColumn(ExprLoc).second) {
      diagnose(ExprLoc, diag::unindented_code_after_return);
      diagnose(ExprLoc, diag::indent_expression_to_silence);
    }

    ParserResult<Expr> Result = parseExpr(diag::expected_expr_return);
    if (Result.isNull()) {
      // Create an ErrorExpr to tell the type checker that this return
      // statement had an expression argument in the source.  This suppresses
      // the error about missing return value in a non-void function.
      Result = makeParserErrorResult(new (Context) ErrorExpr(ExprLoc));
    }

    if (tryLoc.isValid()) {
      diagnose(tryLoc, diag::try_on_return_throw_yield, /*return=*/0)
        .fixItInsert(ExprLoc, "try ")
        .fixItRemoveChars(tryLoc, ReturnLoc);

      // Note: We can't use tryLoc here because that's outside the ReturnStmt's
      // source range.
      if (Result.isNonNull() && !isa<ErrorExpr>(Result.get()))
        Result = makeParserResult(new (Context) TryExpr(ExprLoc, Result.get()));
    }

    return makeParserResult(
        Result, new (Context) ReturnStmt(ReturnLoc, Result.getPtrOrNull()));
  }

  if (tryLoc.isValid())
    diagnose(tryLoc, diag::try_on_stmt, "return");

  return makeParserResult(new (Context) ReturnStmt(ReturnLoc, nullptr));
}

/// parseStmtYield
///
///   stmt-yield:
///     'yield' expr
///     'yield' '(' expr-list ')'
///
/// Note that a parenthesis always starts the second (list) grammar.
ParserResult<Stmt> Parser::parseStmtYield(SourceLoc tryLoc) {
  SyntaxContext->setCreateSyntax(SyntaxKind::YieldStmt);
  SourceLoc yieldLoc = consumeToken(tok::kw_yield);

  if (Tok.is(tok::code_complete)) {
    auto cce = new (Context) CodeCompletionExpr(SourceRange(Tok.getLoc()));
    auto result = makeParserResult(
      YieldStmt::create(Context, yieldLoc, SourceLoc(), cce, SourceLoc()));
    if (CodeCompletion) {
      CodeCompletion->completeYieldStmt(cce, /*index=*/ None);
    }
    result.setHasCodeCompletion();
    consumeToken();
    return result;
  }

  ParserStatus status;
  SourceLoc lpLoc, rpLoc;
  SmallVector<Expr*, 4> yields;
  if (Tok.is(tok::l_paren)) {
    // If there was a 'try' on the yield, and there are multiple
    // yielded values, suggest just removing the try instead of
    // suggesting adding it to every yielded value.
    if (tryLoc.isValid()) {
      diagnose(tryLoc, diag::try_on_return_throw_yield, /*yield=*/2)
        .fixItRemoveChars(tryLoc, yieldLoc);
    }

    SyntaxParsingContext YieldsCtxt(SyntaxContext, SyntaxKind::YieldList);

    SmallVector<Identifier, 4> yieldLabels;
    SmallVector<SourceLoc, 4> yieldLabelLocs;
    Expr *trailingClosure = nullptr;

    status = parseExprList(tok::l_paren, tok::r_paren,
                           /*postfix (allow trailing closure)*/ false,
                           /*expr basic (irrelevant)*/ true,
                           lpLoc,
                           yields, yieldLabels, yieldLabelLocs,
                           rpLoc,
                           trailingClosure,
                           SyntaxKind::ExprList);
    assert(trailingClosure == nullptr);
    assert(yieldLabels.empty());
    assert(yieldLabelLocs.empty());
  } else {
    SourceLoc beginLoc = Tok.getLoc();

    // There's a single yielded value, so suggest moving 'try' before it.
    if (tryLoc.isValid()) {
      diagnose(tryLoc, diag::try_on_return_throw_yield, /*yield=*/2)
        .fixItInsert(beginLoc, "try ")
        .fixItRemoveChars(tryLoc, yieldLoc);
    }

    auto expr = parseExpr(diag::expected_expr_yield);
    if (expr.hasCodeCompletion())
      return makeParserCodeCompletionResult<Stmt>();
    if (expr.isParseError()) {
      auto endLoc = (Tok.getLoc() == beginLoc ? beginLoc : PreviousLoc);
      yields.push_back(
        new (Context) ErrorExpr(SourceRange(beginLoc, endLoc)));
    } else {
      yields.push_back(expr.get());
    }
  }

  return makeParserResult(
           status, YieldStmt::create(Context, yieldLoc, lpLoc, yields, rpLoc));
}

/// parseStmtThrow
///
/// stmt-throw
///   'throw' expr
///
ParserResult<Stmt> Parser::parseStmtThrow(SourceLoc tryLoc) {
  SyntaxContext->setCreateSyntax(SyntaxKind::ThrowStmt);
  SourceLoc throwLoc = consumeToken(tok::kw_throw);
  SourceLoc exprLoc;
  if (Tok.isNot(tok::eof))
    exprLoc = Tok.getLoc();

  ParserResult<Expr> Result = parseExpr(diag::expected_expr_throw);

  if (Result.hasCodeCompletion())
    return makeParserCodeCompletionResult<Stmt>();

  if (Result.isNull())
    Result = makeParserErrorResult(new (Context) ErrorExpr(throwLoc));

  if (tryLoc.isValid() && exprLoc.isValid()) {
    diagnose(tryLoc, diag::try_on_return_throw_yield, /*throw=*/1)
      .fixItInsert(exprLoc, "try ")
      .fixItRemoveChars(tryLoc, throwLoc);

    // Note: We can't use tryLoc here because that's outside the ThrowStmt's
    // source range.
    if (Result.isNonNull() && !isa<ErrorExpr>(Result.get()))
      Result = makeParserResult(new (Context) TryExpr(exprLoc, Result.get()));
  }

  return makeParserResult(Result,
              new (Context) ThrowStmt(throwLoc, Result.get()));
}

/// parseStmtDefer
///
///   stmt-defer:
///     'defer' brace-stmt
///
ParserResult<Stmt> Parser::parseStmtDefer() {
  SyntaxContext->setCreateSyntax(SyntaxKind::DeferStmt);
  SourceLoc DeferLoc = consumeToken(tok::kw_defer);
  
  // Macro expand out the defer into a closure and call, which we can typecheck
  // and emit where needed.
  //
  // The AST representation for a defer statement is a bit weird.  We retain the
  // brace statement that the user wrote, but actually model this as if they
  // wrote:
  //
  //    func tmpClosure() { body }
  //    tmpClosure()   // This is emitted on each path that needs to run this.
  //
  // As such, the body of the 'defer' is actually type checked within the
  // closure's DeclContext.
  auto params = ParameterList::createEmpty(Context);
  DeclName name(Context, Context.getIdentifier("$defer"), params);
  auto tempDecl
    = FuncDecl::create(Context,
                       /*StaticLoc=*/ SourceLoc(),
                       StaticSpellingKind::None,
                       /*FuncLoc=*/ SourceLoc(),
                       name,
                       /*NameLoc=*/ SourceLoc(),
                       /*Throws=*/ false, /*ThrowsLoc=*/ SourceLoc(),
                       /*generic params*/ nullptr,
                       params,
                       TypeLoc(),
                       CurDeclContext);
  tempDecl->setImplicit();
  setLocalDiscriminator(tempDecl);
  ParserStatus Status;
  {
    // Change the DeclContext for any variables declared in the defer to be within
    // the defer closure.
    ParseFunctionBody cc(*this, tempDecl);
    
    ParserResult<BraceStmt> Body =
      parseBraceItemList(diag::expected_lbrace_after_defer);
    if (Body.isNull())
      return nullptr;
    Status |= Body;
    tempDecl->setBody(Body.get());
  }
  
  SourceLoc loc = tempDecl->getBody()->getStartLoc();

  // Form the call, which will be emitted on any path that needs to run the
  // code.
  auto DRE = new (Context) DeclRefExpr(tempDecl, DeclNameLoc(loc),
                                       /*Implicit*/true,
                                       AccessSemantics::DirectToStorage);
  auto call = CallExpr::createImplicit(Context, DRE, { }, { });
  
  auto DS = new (Context) DeferStmt(DeferLoc, tempDecl, call);
  return makeParserResult(Status, DS);
}

namespace {
  struct GuardedPattern {
    Pattern *ThePattern = nullptr;
    SourceLoc WhereLoc;
    Expr *Guard = nullptr;
  };
  
  /// Contexts in which a guarded pattern can appear.
  enum class GuardedPatternContext {
    Case,
    Catch,
  };
} // unnamed namespace

static void parseWhereGuard(Parser &P, GuardedPattern &result,
                            ParserStatus &status,
                            GuardedPatternContext parsingContext,
                            bool isExprBasic) {
  if (P.Tok.is(tok::kw_where)) {
    SyntaxParsingContext WhereClauseCtxt(P.SyntaxContext,
                                         SyntaxKind::WhereClause);
    result.WhereLoc = P.consumeToken(tok::kw_where);
    SourceLoc startOfGuard = P.Tok.getLoc();

    auto diagKind = [=]() -> Diag<> {
      switch (parsingContext) {
      case GuardedPatternContext::Case:
        return diag::expected_case_where_expr;
      case GuardedPatternContext::Catch:
        return diag::expected_catch_where_expr;
      }
      llvm_unreachable("bad context");
    }();
    ParserResult<Expr> guardResult = P.parseExprImpl(diagKind, isExprBasic);
    status |= guardResult;

    // Use the parsed guard expression if possible.
    if (guardResult.isNonNull()) {
      result.Guard = guardResult.get();

      // Otherwise, fake up an ErrorExpr.
    } else {
      // If we didn't consume any tokens failing to parse the
      // expression, don't put in the source range of the ErrorExpr.
      SourceRange errorRange;
      if (startOfGuard == P.Tok.getLoc()) {
        errorRange = result.WhereLoc;
      } else {
        errorRange = SourceRange(startOfGuard, P.PreviousLoc);
      }
      result.Guard = new (P.Context) ErrorExpr(errorRange);
    }
  }
}

/// Parse a pattern-matching clause for a case or catch statement,
/// including the guard expression:
///
///    pattern 'where' expr
static void parseGuardedPattern(Parser &P, GuardedPattern &result,
                                ParserStatus &status,
                                SmallVectorImpl<VarDecl *> &boundDecls,
                                GuardedPatternContext parsingContext,
                                bool isFirstPattern) {
  ParserResult<Pattern> patternResult;

  bool isExprBasic = [&]() -> bool {
    switch (parsingContext) {
    // 'case' is terminated with a colon and so allows a trailing closure.
    case GuardedPatternContext::Case:
      return false;
    // 'catch' is terminated with a brace and so cannot.
    case GuardedPatternContext::Catch:
      return true;
    }
    llvm_unreachable("bad pattern context");
  }();

  // Do some special-case code completion for the start of the pattern.
  if (P.Tok.is(tok::code_complete)) {
    auto CCE = new (P.Context) CodeCompletionExpr(P.Tok.getLoc());
    result.ThePattern = new (P.Context) ExprPattern(CCE);
    if (P.CodeCompletion) {
      switch (parsingContext) {
      case GuardedPatternContext::Case:
        P.CodeCompletion->completeCaseStmtBeginning(CCE);
        break;
      case GuardedPatternContext::Catch:
        P.CodeCompletion->completePostfixExprBeginning(CCE);
        break;
      }
    }
    P.consumeToken(tok::code_complete);
    status.setHasCodeCompletion();
    return;
  }

  // If this is a 'catch' clause and we have "catch {" or "catch where...",
  // then we get an implicit "let error" pattern.
  if (parsingContext == GuardedPatternContext::Catch &&
      P.Tok.isAny(tok::l_brace, tok::kw_where)) {
    auto loc = P.Tok.getLoc();
    auto errorName = P.Context.Id_error;
    auto var = new (P.Context) VarDecl(/*IsStatic*/false,
                                       VarDecl::Specifier::Let,
                                       /*IsCaptureList*/false, loc, errorName,
                                       P.CurDeclContext);
    var->setImplicit();
    auto namePattern = new (P.Context) NamedPattern(var);
    auto varPattern = new (P.Context) VarPattern(loc, /*isLet*/true,
                                                 namePattern, /*implicit*/true);
    patternResult = makeParserResult(varPattern);
  }

  // Okay, if the special code-completion didn't kick in, parse a
  // matching pattern.
  if (patternResult.isNull()) {
    llvm::SaveAndRestore<decltype(P.InVarOrLetPattern)>
      T(P.InVarOrLetPattern, Parser::IVOLP_InMatchingPattern);
    patternResult = P.parseMatchingPattern(isExprBasic);
  }

  // If that didn't work, use a bogus pattern so that we can fill out
  // the AST.
  if (patternResult.isNull())
    patternResult =
      makeParserErrorResult(new (P.Context) AnyPattern(P.PreviousLoc));

  // Fill in the pattern.
  status |= patternResult;
  result.ThePattern = patternResult.get();

  if (isFirstPattern) {
    // Add variable bindings from the pattern to the case scope.  We have
    // to do this with a full AST walk, because the freshly parsed pattern
    // represents tuples and var patterns as tupleexprs and
    // unresolved_pattern_expr nodes, instead of as proper pattern nodes.
    patternResult.get()->forEachVariable([&](VarDecl *VD) {
      P.setLocalDiscriminator(VD);
      if (VD->hasName()) P.addToScope(VD);
      boundDecls.push_back(VD);
    });

    // Now that we have them, mark them as being initialized without a PBD.
    for (auto VD : boundDecls)
      VD->setHasNonPatternBindingInit();

    // Parse the optional 'where' guard.
    parseWhereGuard(P, result, status, parsingContext, isExprBasic);
  } else {
    // If boundDecls already contains variables, then we must match the
    // same number and same names in this pattern as were declared in a
    // previous pattern (and later we will make sure they have the same
    // types).
    Scope guardScope(&P, ScopeKind::CaseVars);
    SmallVector<VarDecl*, 4> repeatedDecls;
    patternResult.get()->forEachVariable([&](VarDecl *VD) {
      if (!VD->hasName())
        return;
      
      bool found = false;
      for (auto previous : boundDecls) {
        if (previous->hasName() && previous->getName() == VD->getName()) {
          found = true;
          break;
        }
      }
      if (!found) {
        // Diagnose a declaration that doesn't match a previous pattern.
        P.diagnose(VD->getLoc(), diag::extra_var_in_multiple_pattern_list, VD->getName());
        status.setIsParseError();
      }
      repeatedDecls.push_back(VD);
      P.setLocalDiscriminator(VD);
      if (VD->hasName())
        P.addToScope(VD);
    });
    
    for (auto previous : boundDecls) {
      bool found = false;
      for (auto repeat : repeatedDecls) {
        if (previous->hasName() && previous->getName() == repeat->getName()) {
          found = true;
          break;
        }
      }
      if (!found) {
        // Diagnose a previous declaration that is missing in this pattern.
        P.diagnose(previous->getLoc(), diag::extra_var_in_multiple_pattern_list, previous->getName());
        status.setIsParseError();
      }
    }
    
    for (auto VD : repeatedDecls) {
      VD->setHasNonPatternBindingInit();
    }

    // Parse the optional 'where' guard, with this particular pattern's bound
    // vars in scope.
    parseWhereGuard(P, result, status, parsingContext, isExprBasic);
  }
}

/// Validate availability spec list, emitting diagnostics if necessary and removing
/// specs for unrecognized platforms.
static void validateAvailabilitySpecList(Parser &P,
    SmallVectorImpl<AvailabilitySpec *> &Specs) {
  llvm::SmallSet<PlatformKind, 4> Platforms;
  bool HasOtherPlatformSpec = false;

  if (Specs.size() == 1 &&
      isa<PlatformAgnosticVersionConstraintAvailabilitySpec>(Specs[0])) {
    // @available(swift N) and @available(_PackageDescription N) are allowed 
    // only in isolation; they cannot be combined with other availability specs
    // in a single list.
    return;
  }

  SmallVector<AvailabilitySpec *, 5> RecognizedSpecs;
  for (auto *Spec : Specs) {
    RecognizedSpecs.push_back(Spec);
    if (isa<OtherPlatformAvailabilitySpec>(Spec)) {
      HasOtherPlatformSpec = true;
      continue;
    }

    if (auto *PlatformAgnosticSpec =
        dyn_cast<PlatformAgnosticVersionConstraintAvailabilitySpec>(Spec)) {
      P.diagnose(PlatformAgnosticSpec->getPlatformAgnosticNameLoc(),
                 diag::availability_must_occur_alone,
                 PlatformAgnosticSpec->isLanguageVersionSpecific() ? "swift" : "_PackageDescription");
      continue;
    }

    auto *VersionSpec = cast<PlatformVersionConstraintAvailabilitySpec>(Spec);
    // We keep specs for unrecognized platforms around for error recovery
    // during parsing but remove them once parsing is completed.
    if (VersionSpec->isUnrecognizedPlatform()) {
      RecognizedSpecs.pop_back();
      continue;
    }

    bool Inserted = Platforms.insert(VersionSpec->getPlatform()).second;
    if (!Inserted) {
      // Rule out multiple version specs referring to the same platform.
      // For example, we emit an error for
      /// #available(OSX 10.10, OSX 10.11, *)
      PlatformKind Platform = VersionSpec->getPlatform();
      P.diagnose(VersionSpec->getPlatformLoc(),
                 diag::availability_query_repeated_platform,
                 platformString(Platform));
    }
  }

  if (!HasOtherPlatformSpec) {
    SourceLoc InsertWildcardLoc = Specs.back()->getSourceRange().End;
    P.diagnose(InsertWildcardLoc, diag::availability_query_wildcard_required)
        .fixItInsertAfter(InsertWildcardLoc, ", *");
  }

  Specs = RecognizedSpecs;
}

// #available(...)
ParserResult<PoundAvailableInfo> Parser::parseStmtConditionPoundAvailable() {
  SyntaxParsingContext ConditonCtxt(SyntaxContext,
                                    SyntaxKind::AvailabilityCondition);
  SourceLoc PoundLoc = consumeToken(tok::pound_available);

  if (!Tok.isFollowingLParen()) {
    diagnose(Tok, diag::avail_query_expected_condition);
    return makeParserError();
  }

  StructureMarkerRAII ParsingAvailabilitySpecList(*this, Tok);

  if (ParsingAvailabilitySpecList.isFailed()) {
    return makeParserError();
  }
    
  SourceLoc LParenLoc = consumeToken(tok::l_paren);

  SmallVector<AvailabilitySpec *, 5> Specs;
  ParserStatus Status = parseAvailabilitySpecList(Specs);

  for (auto *Spec : Specs) {
    if (auto *PlatformAgnostic =
        dyn_cast<PlatformAgnosticVersionConstraintAvailabilitySpec>(Spec)) {
        diagnose(PlatformAgnostic->getPlatformAgnosticNameLoc(),
                 PlatformAgnostic->isLanguageVersionSpecific() ?
                   diag::pound_available_swift_not_allowed :
                   diag::pound_available_package_description_not_allowed);
      Status.setIsParseError();
    }
  }

  SourceLoc RParenLoc;
  if (parseMatchingToken(tok::r_paren, RParenLoc,
                         diag::avail_query_expected_rparen, LParenLoc))
    Status.setIsParseError();

  auto *result = PoundAvailableInfo::create(Context, PoundLoc, Specs,RParenLoc);
  return makeParserResult(Status, result);
}

ParserStatus
Parser::parseAvailabilitySpecList(SmallVectorImpl<AvailabilitySpec *> &Specs) {
  SyntaxParsingContext AvailabilitySpecContext(
      SyntaxContext, SyntaxKind::AvailabilitySpecList);
  ParserStatus Status = makeParserSuccess();

  // We don't use parseList() because we want to provide more specific
  // diagnostics disallowing operators in version specs.
  while (1) {
    SyntaxParsingContext AvailabilityEntryContext(
        SyntaxContext, SyntaxKind::AvailabilityArgument);
    auto SpecResult = parseAvailabilitySpec();
    if (auto *Spec = SpecResult.getPtrOrNull()) {
      Specs.push_back(Spec);
    } else {
      if (SpecResult.hasCodeCompletion()) {
        return makeParserCodeCompletionStatus();
      }
      Status.setIsParseError();
    }

    // We don't allow binary operators to combine specs.
    if (Tok.isBinaryOperator()) {
      diagnose(Tok, diag::avail_query_disallowed_operator, Tok.getText());
      consumeToken();
      Status.setIsParseError();
    } else if (consumeIf(tok::comma)) {
      // There is more to parse in this list.

      // Before continuing to parse the next specification, we check that it's
      // also in the shorthand syntax and provide a more specific diagnostic if
      // that's not the case.
      if (Tok.isIdentifierOrUnderscore() &&
          !peekToken().isAny(tok::integer_literal, tok::floating_literal) &&
          !Specs.empty()) {
        auto Text = Tok.getText();
        if (Text == "deprecated" || Text == "renamed" || Text == "introduced" ||
            Text == "message" || Text == "obsoleted" || Text == "unavailable") {
          auto *Previous = Specs.back();
          auto &SourceManager = Context.SourceMgr;
          auto PreviousSpecText =
              SourceManager.extractText(L->getCharSourceRangeFromSourceRange(
                  SourceManager, Previous->getSourceRange()));

          diagnose(Tok,
                   diag::avail_query_argument_and_shorthand_mix_not_allowed,
                   Text, PreviousSpecText);

          // If this was preceded by a single platform version constraint, we
          // can guess that the intention was to treat it as 'introduced' and
          // suggest a fix-it to combine them.
          if (Specs.size() == 1 &&
              PlatformVersionConstraintAvailabilitySpec::classof(Previous) &&
              Text != "introduced") {
            auto *PlatformSpec =
                cast<PlatformVersionConstraintAvailabilitySpec>(Previous);

            auto PlatformName = platformString(PlatformSpec->getPlatform());
            auto PlatformNameEndLoc =
                PlatformSpec->getPlatformLoc().getAdvancedLoc(
                    PlatformName.size());

            diagnose(PlatformSpec->getPlatformLoc(),
                     diag::avail_query_meant_introduced)
                .fixItInsert(PlatformNameEndLoc, ", introduced:");
          }

          Status.setIsParseError();
          break;
        }
      }

      // Otherwise, keep going.
    } else {
      break;
    }
  }

  if (Status.isSuccess())
    validateAvailabilitySpecList(*this, Specs);

  return Status;
}

ParserStatus
Parser::parseStmtConditionElement(SmallVectorImpl<StmtConditionElement> &result,
                                  Diag<> DefaultID, StmtKind ParentKind,
                                  StringRef &BindingKindStr) {
  ParserStatus Status;

  // Parse a leading #available condition if present.
  if (Tok.is(tok::pound_available)) {
    auto res = parseStmtConditionPoundAvailable();
    if (res.isNull() || res.hasCodeCompletion()) {
      Status |= res;
      return Status;
    }
    BindingKindStr = StringRef();
    result.push_back({res.get()});
    return Status;
  }

  // Handle code completion after the #.
  if (Tok.is(tok::pound) && peekToken().is(tok::code_complete) &&
      Tok.getLoc().getAdvancedLoc(1) == peekToken().getLoc()) {
    auto Expr = parseExprPoundCodeCompletion(ParentKind);
    Status |= Expr;
    result.push_back(Expr.get());
  }

  // Parse the basic expression case.  If we have a leading let/var/case
  // keyword or an assignment, then we know this is a binding.
  if (Tok.isNot(tok::kw_let, tok::kw_var, tok::kw_case)) {
    // If we lack it, then this is theoretically a boolean condition.
    // However, we also need to handle migrating from Swift 2 syntax, in
    // which a comma followed by an expression could actually be a pattern
    // clause followed by a binding.  Determine what we have by checking for a
    // syntactically valid pattern followed by an '=', which can never be a
    // boolean condition.
    //
    // However, if this is the first clause, and we see "x = y", then this is
    // almost certainly a typo for '==' and definitely not a continuation of
    // another clause, so parse it as an expression.  This also avoids
    // lookahead + backtracking on simple if conditions that are obviously
    // boolean conditions.
    auto isBooleanExpr = [&]() -> bool {
      Parser::BacktrackingScope Backtrack(*this);
      return !canParseTypedPattern() || Tok.isNot(tok::equal);
    };

    if (BindingKindStr.empty() || isBooleanExpr()) {
      auto diagID = result.empty() ? DefaultID :
        diag::expected_expr_conditional;
      auto BoolExpr = parseExprBasic(diagID);
      Status |= BoolExpr;
      if (BoolExpr.isNull())
        return Status;
      result.push_back(BoolExpr.get());
      BindingKindStr = StringRef();
      return Status;
    }
  }

  SyntaxParsingContext ConditionCtxt(SyntaxContext);

  SourceLoc IntroducerLoc;
  if (Tok.isAny(tok::kw_let, tok::kw_var, tok::kw_case)) {
    BindingKindStr = Tok.getText();
    IntroducerLoc = consumeToken();
  } else {
    // If we lack the leading let/var/case keyword, then we're here because
    // the user wrote something like "if let x = foo(), y = bar() {".  Fix
    // this by inserting a new 'let' keyword before y.
    IntroducerLoc = Tok.getLoc();
    assert(!BindingKindStr.empty() &&
            "Shouldn't get here without a leading binding");
    diagnose(Tok.getLoc(), diag::expected_binding_keyword, BindingKindStr)
      .fixItInsert(Tok.getLoc(), BindingKindStr.str()+" ");
  }

  // We're parsing a conditional binding.
  assert(CurDeclContext->isLocalContext() &&
          "conditional binding in non-local context?!");
    
  ParserResult<Pattern> ThePattern;
    
  if (BindingKindStr == "case") {
    ConditionCtxt.setCreateSyntax(SyntaxKind::MatchingPatternCondition);
    // In our recursive parse, remember that we're in a matching pattern.
    llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
      T(InVarOrLetPattern, IVOLP_InMatchingPattern);
    ThePattern = parseMatchingPattern(/*isExprBasic*/ true);
  } else if (Tok.is(tok::kw_case)) {
    ConditionCtxt.setCreateSyntax(SyntaxKind::Unknown);
    // If will probably be a common typo to write "if let case" instead of
    // "if case let" so detect this and produce a nice fixit.
    diagnose(IntroducerLoc, diag::wrong_condition_case_location,
              BindingKindStr)
    .fixItRemove(IntroducerLoc)
    .fixItInsertAfter(Tok.getLoc(), " " + BindingKindStr.str());

    consumeToken(tok::kw_case);
    
    bool wasLet = BindingKindStr == "let";
    BindingKindStr = "case";
    
    // In our recursive parse, remember that we're in a var/let pattern.
    llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
      T(InVarOrLetPattern, wasLet ? IVOLP_InLet : IVOLP_InVar);
    
    ThePattern = parseMatchingPattern(/*isExprBasic*/ true);
    
    if (ThePattern.isNonNull()) {
      auto *P = new (Context) VarPattern(IntroducerLoc, wasLet,
                                          ThePattern.get(), /*impl*/false);
      ThePattern = makeParserResult(Status, P);
    }

  } else {
    ConditionCtxt.setCreateSyntax(SyntaxKind::OptionalBindingCondition);
    // Otherwise, this is an implicit optional binding "if let".
    ThePattern = parseMatchingPatternAsLetOrVar(BindingKindStr == "let",
                                                IntroducerLoc,
                                                /*isExprBasic*/ true);
    // The let/var pattern is part of the statement.
    if (Pattern *P = ThePattern.getPtrOrNull())
      P->setImplicit();
  }

  ThePattern = parseOptionalPatternTypeAnnotation(ThePattern,
                                                  BindingKindStr != "case");
  if (ThePattern.hasCodeCompletion())
    Status.setHasCodeCompletion();
    
  if (ThePattern.isNull()) {
    // Recover by creating AnyPattern.
    ThePattern = makeParserResult(new (Context) AnyPattern(PreviousLoc));
  }

  // Conditional bindings must have an initializer.
  ParserResult<Expr> Init;
  if (Tok.is(tok::equal)) {
    SyntaxParsingContext InitCtxt(SyntaxContext, SyntaxKind::InitializerClause);
    consumeToken();
    Init = parseExprBasic(diag::expected_expr_conditional_var);
  } else {
    diagnose(Tok, diag::conditional_var_initializer_required);
  }

  if (Init.hasCodeCompletion())
    Status.setHasCodeCompletion();

  if (Init.isNull()) {
    // Recover by creating ErrorExpr.
    Init = makeParserResult(new (Context)
                                ErrorExpr(ThePattern.get()->getEndLoc()));
  }

  result.push_back({IntroducerLoc, ThePattern.get(), Init.get()});
  
  // Add variable bindings from the pattern to our current scope and mark
  // them as being having a non-pattern-binding initializer.
  ThePattern.get()->forEachVariable([&](VarDecl *VD) {
    setLocalDiscriminator(VD);
    if (VD->hasName())
      addToScope(VD);
    VD->setHasNonPatternBindingInit();
  });
  return Status;
}

/// Parse the condition of an 'if' or 'while'.
///
///   condition:
///     condition-clause (',' condition-clause)*
///   condition-clause:
///     expr-basic
///     ('var' | 'let' | 'case') pattern '=' expr-basic
///     '#available' '(' availability-spec (',' availability-spec)* ')'
///
/// The use of expr-basic here disallows trailing closures, which are
/// problematic given the curly braces around the if/while body.
///
ParserStatus Parser::parseStmtCondition(StmtCondition &Condition,
                                        Diag<> DefaultID, StmtKind ParentKind) {
  SyntaxParsingContext ConditionListCtxt(SyntaxContext,
                                         SyntaxKind::ConditionElementList);
  ParserStatus Status;
  Condition = StmtCondition();

  SmallVector<StmtConditionElement, 4> result;

  // For error recovery purposes, keep track of the disposition of the last
  // pattern binding we saw ('let', 'var', or 'case').
  StringRef BindingKindStr;
  
  // We have a simple comma separated list of clauses, but also need to handle
  // a variety of common errors situations (including migrating from Swift 2
  // syntax).
  while (true) {
    SyntaxParsingContext ConditionElementCtxt(SyntaxContext,
                                              SyntaxKind::ConditionElement);
    Status |= parseStmtConditionElement(result, DefaultID, ParentKind,
                                        BindingKindStr);
    if (Status.shouldStopParsing())
      break;

    // If a comma exists consume it and succeed.
    if (consumeIf(tok::comma))
      continue;
    
    // If we have an "&&" token followed by a continuation of the statement
    // condition, then fixit the "&&" to "," and keep going.
    if (Tok.isAny(tok::oper_binary_spaced, tok::oper_binary_unspaced) &&
        Tok.getText() == "&&") {
      diagnose(Tok, diag::expected_comma_stmtcondition)
        .fixItReplaceChars(getEndOfPreviousLoc(), Tok.getRange().getEnd(), ",");
      consumeToken();
      continue;
    }

    // Boolean conditions are separated by commas, not the 'where' keyword, as
    // they were in Swift 2 and earlier.
    if (Tok.is(tok::kw_where)) {
      diagnose(Tok, diag::expected_comma_stmtcondition)
        .fixItReplaceChars(getEndOfPreviousLoc(), Tok.getRange().getEnd(), ",");
      consumeToken();
      continue;
    }
    
    break;
  }; 
  
  Condition = Context.AllocateCopy(result);
  return Status;
}

/// 
///   stmt-if:
///     'if' condition stmt-brace stmt-if-else?
///   stmt-if-else:
///    'else' stmt-brace
///    'else' stmt-if
ParserResult<Stmt> Parser::parseStmtIf(LabeledStmtInfo LabelInfo,
                                       bool IfWasImplicitlyInserted) {
  SyntaxContext->setCreateSyntax(SyntaxKind::IfStmt);
  SourceLoc IfLoc;
  if (IfWasImplicitlyInserted) {
    // The code was invalid due to a missing 'if' (e.g. 'else x < y {') and a
    // fixit implicitly inserted it.
    IfLoc = Tok.getLoc();
  } else {
    IfLoc = consumeToken(tok::kw_if);
  }

  ParserStatus Status;
  StmtCondition Condition;
  ParserResult<BraceStmt> NormalBody;
  
  // A scope encloses the condition and true branch for any variables bound
  // by a conditional binding. The else branch does *not* see these variables.
  {
    Scope S(this, ScopeKind::IfVars);

    auto recoverWithCond = [&](ParserStatus Status,
                               StmtCondition Condition) -> ParserResult<Stmt> {
      if (Condition.empty()) {
        SmallVector<StmtConditionElement, 1> ConditionElems;
        ConditionElems.emplace_back(new (Context) ErrorExpr(IfLoc));
        Condition = Context.AllocateCopy(ConditionElems);
      }
      auto EndLoc = Condition.back().getEndLoc();
      return makeParserResult(
          Status,
          new (Context) IfStmt(
              LabelInfo, IfLoc, Condition,
              BraceStmt::create(Context, EndLoc, {}, EndLoc, /*implicit=*/true),
              SourceLoc(), nullptr));
    };

    if (Tok.is(tok::l_brace)) {
      SourceLoc LBraceLoc = Tok.getLoc();
      diagnose(IfLoc, diag::missing_condition_after_if)
        .highlight(SourceRange(IfLoc, LBraceLoc));
      SmallVector<StmtConditionElement, 1> ConditionElems;
      ConditionElems.emplace_back(new (Context) ErrorExpr(LBraceLoc));
      Condition = Context.AllocateCopy(ConditionElems);
    } else {
      Status |= parseStmtCondition(Condition, diag::expected_condition_if,
                                   StmtKind::If);
      if (Status.isError() || Status.hasCodeCompletion())
        return recoverWithCond(Status, Condition);
    }

    if (Tok.is(tok::kw_else)) {
      SourceLoc ElseLoc = Tok.getLoc();
      diagnose(ElseLoc, diag::unexpected_else_after_if);
      diagnose(ElseLoc, diag::suggest_removing_else)
        .fixItRemove(ElseLoc);
      consumeToken(tok::kw_else);
    }

    NormalBody = parseBraceItemList(diag::expected_lbrace_after_if);
    Status |= NormalBody;
    if (NormalBody.isNull())
      return recoverWithCond(Status, Condition);
  }

  // The else branch, if any, is outside of the scope of the condition.
  SourceLoc ElseLoc;
  ParserResult<Stmt> ElseBody;
  if (Tok.is(tok::kw_else)) {
    ElseLoc = consumeToken(tok::kw_else);

    bool implicitlyInsertIf = false;
    if (Tok.isNot(tok::kw_if, tok::l_brace, tok::code_complete)) {
      // The code looks like 'if ... { ... } else not_if_or_lbrace', so we've
      // got a problem. If the last bit is 'else ... {' on one line, let's
      // assume they've forgotten the 'if'.
      BacktrackingScope backtrack(*this);
      implicitlyInsertIf = skipUntilTokenOrEndOfLine(tok::l_brace);
    }

    if (Tok.is(tok::kw_if) || implicitlyInsertIf) {
      if (implicitlyInsertIf) {
        diagnose(ElseLoc, diag::expected_lbrace_or_if_after_else_fixit)
            .fixItInsertAfter(ElseLoc, " if");
      }
      SyntaxParsingContext ElseIfCtxt(SyntaxContext, SyntaxKind::IfStmt);
      ElseBody = parseStmtIf(LabeledStmtInfo(), implicitlyInsertIf);
    } else if (Tok.is(tok::code_complete)) {
      if (CodeCompletion)
        CodeCompletion->completeAfterIfStmt(/*hasElse*/true);
      Status.setHasCodeCompletion();
      consumeToken(tok::code_complete);
    } else {
      ElseBody = parseBraceItemList(diag::expected_lbrace_or_if_after_else);
    }
    Status |= ElseBody;
  } else if (Tok.is(tok::code_complete)) {
    if (CodeCompletion)
      CodeCompletion->completeAfterIfStmt(/*hasElse*/false);
    Status.setHasCodeCompletion();
    consumeToken(tok::code_complete);
  }

  return makeParserResult(
      Status, new (Context) IfStmt(LabelInfo,
                                   IfLoc, Condition, NormalBody.get(),
                                   ElseLoc, ElseBody.getPtrOrNull()));
}

///   stmt-guard:
///     'guard' condition 'else' stmt-brace
///
ParserResult<Stmt> Parser::parseStmtGuard() {
  SyntaxContext->setCreateSyntax(SyntaxKind::GuardStmt);
  SourceLoc GuardLoc = consumeToken(tok::kw_guard);
  
  ParserStatus Status;
  StmtCondition Condition;
  ParserResult<BraceStmt> Body;

  auto recoverWithCond = [&](ParserStatus Status,
                             StmtCondition Condition) -> ParserResult<Stmt> {
    if (Condition.empty()) {
      SmallVector<StmtConditionElement, 1> ConditionElems;
      ConditionElems.emplace_back(new (Context) ErrorExpr(GuardLoc));
      Condition = Context.AllocateCopy(ConditionElems);
    }
    auto EndLoc = Condition.back().getEndLoc();
    return makeParserResult(
        Status,
        new (Context) GuardStmt(
            GuardLoc, Condition,
            BraceStmt::create(Context, EndLoc, {}, EndLoc, /*implicit=*/true)));
  };

  if (Tok.is(tok::l_brace)) {
    SourceLoc LBraceLoc = Tok.getLoc();
    diagnose(GuardLoc, diag::missing_condition_after_guard)
      .highlight(SourceRange(GuardLoc, LBraceLoc));
    SmallVector<StmtConditionElement, 1> ConditionElems;
    ConditionElems.emplace_back(new (Context) ErrorExpr(LBraceLoc));
    Condition = Context.AllocateCopy(ConditionElems);
  } else {
    Status |= parseStmtCondition(Condition, diag::expected_condition_guard,
                                 StmtKind::Guard);
    if (Status.isError() || Status.hasCodeCompletion()) {
      // FIXME: better recovery
      return recoverWithCond(Status, Condition);
    }
  }

  // Parse the 'else'.  If it is missing, and if the following token isn't a {
  // then the parser is hopelessly lost - just give up instead of spewing.
  if (!consumeIf(tok::kw_else)) {
    checkForInputIncomplete();
    auto diag = diagnose(Tok, diag::expected_else_after_guard);
    if (Tok.is(tok::l_brace))
      diag.fixItInsert(Tok.getLoc(), "else ");
    else
      return recoverWithCond(Status, Condition);
  }

  // Before parsing the body, disable all of the bound variables so that they
  // cannot be used unbound.
  SmallVector<VarDecl *, 4> Vars;
  for (auto &elt : Condition)
    if (auto pattern = elt.getPatternOrNull())
      pattern->collectVariables(Vars);
  Vars.append(DisabledVars.begin(), DisabledVars.end());
  llvm::SaveAndRestore<decltype(DisabledVars)>
  RestoreCurVars(DisabledVars, Vars);

  llvm::SaveAndRestore<decltype(DisabledVarReason)>
  RestoreReason(DisabledVarReason, diag::bound_var_guard_body);

  Body = parseBraceItemList(diag::expected_lbrace_after_guard);
  if (Body.isNull())
    return recoverWithCond(Status, Condition);

  Status |= Body;
  
  return makeParserResult(Status,
              new (Context) GuardStmt(GuardLoc, Condition, Body.get()));
}

/// 
///   stmt-while:
///     (identifier ':')? 'while' expr-basic stmt-brace
ParserResult<Stmt> Parser::parseStmtWhile(LabeledStmtInfo LabelInfo) {
  SyntaxContext->setCreateSyntax(SyntaxKind::WhileStmt);
  SourceLoc WhileLoc = consumeToken(tok::kw_while);

  Scope S(this, ScopeKind::WhileVars);
  
  ParserStatus Status;
  StmtCondition Condition;

  auto recoverWithCond = [&](ParserStatus Status,
                             StmtCondition Condition) -> ParserResult<Stmt> {
    if (Condition.empty()) {
      SmallVector<StmtConditionElement, 1> ConditionElems;
      ConditionElems.emplace_back(new (Context) ErrorExpr(WhileLoc));
      Condition = Context.AllocateCopy(ConditionElems);
    }
    auto EndLoc = Condition.back().getEndLoc();
    return makeParserResult(
        Status,
        new (Context) WhileStmt(
            LabelInfo, WhileLoc, Condition,
            BraceStmt::create(Context, EndLoc, {}, EndLoc, /*implicit=*/true)));
  };

  if (Tok.is(tok::l_brace)) {
    SourceLoc LBraceLoc = Tok.getLoc();
    diagnose(WhileLoc, diag::missing_condition_after_while)
      .highlight(SourceRange(WhileLoc, LBraceLoc));
    SmallVector<StmtConditionElement, 1> ConditionElems;
    ConditionElems.emplace_back(new (Context) ErrorExpr(LBraceLoc));
    Condition = Context.AllocateCopy(ConditionElems);
  } else {
    Status |= parseStmtCondition(Condition, diag::expected_condition_while,
                                 StmtKind::While);
    if (Status.isError() || Status.hasCodeCompletion())
      return recoverWithCond(Status, Condition);
  }

  ParserResult<BraceStmt> Body =
      parseBraceItemList(diag::expected_lbrace_after_while);
  Status |= Body;
  if (Body.isNull())
    return recoverWithCond(Status, Condition);

  return makeParserResult(
      Status, new (Context) WhileStmt(LabelInfo, WhileLoc, Condition,
                                      Body.get()));
}

///
///   stmt-repeat:
///     (identifier ':')? 'repeat' stmt-brace 'while' expr
ParserResult<Stmt> Parser::parseStmtRepeat(LabeledStmtInfo labelInfo) {
  SyntaxContext->setCreateSyntax(SyntaxKind::RepeatWhileStmt);
  SourceLoc repeatLoc = consumeToken(tok::kw_repeat);

  ParserStatus status;

  ParserResult<BraceStmt> body =
      parseBraceItemList(diag::expected_lbrace_after_repeat);
  status |= body;
  if (body.isNull())
    body = makeParserResult(
        body, BraceStmt::create(Context, repeatLoc, {}, PreviousLoc, true));

  SourceLoc whileLoc;

  if (!consumeIf(tok::kw_while, whileLoc)) {
    diagnose(body.getPtrOrNull()->getEndLoc(),
             diag::expected_while_after_repeat_body);
    return body;
  }

  ParserResult<Expr> condition;
  if (Tok.is(tok::l_brace)) {
    SourceLoc lbraceLoc = Tok.getLoc();
    diagnose(whileLoc, diag::missing_condition_after_while);
    condition = makeParserErrorResult(new (Context) ErrorExpr(lbraceLoc));
  } else {
    condition = parseExpr(diag::expected_expr_repeat_while);
    status |= condition;
    if (condition.isNull()) {
      condition = makeParserErrorResult(new (Context) ErrorExpr(whileLoc));
    }
  }

  return makeParserResult(
      status,
      new (Context) RepeatWhileStmt(labelInfo, repeatLoc, condition.get(),
                                    whileLoc, body.get()));
}

/// 
///   stmt-do:
///     (identifier ':')? 'do' stmt-brace
///     (identifier ':')? 'do' stmt-brace stmt-catch+
ParserResult<Stmt> Parser::parseStmtDo(LabeledStmtInfo labelInfo) {
  SyntaxContext->setCreateSyntax(SyntaxKind::DoStmt);
  SourceLoc doLoc = consumeToken(tok::kw_do);

  ParserStatus status;

  ParserResult<BraceStmt> body =
      parseBraceItemList(diag::expected_lbrace_after_do);
  status |= body;
  if (body.isNull())
    body = makeParserResult(
        body, BraceStmt::create(Context, doLoc, {}, PreviousLoc, true));

  // If the next token is 'catch', this is a 'do'/'catch' statement.
  if (Tok.is(tok::kw_catch)) {
    SyntaxParsingContext CatchListCtxt(SyntaxContext,
                                       SyntaxKind::CatchClauseList);
    // Parse 'catch' clauses 
    SmallVector<CatchStmt*, 4> allClauses;
    do {
      ParserResult<CatchStmt> clause = parseStmtCatch();
      status |= clause;
      if (status.hasCodeCompletion() && clause.isNull())
        return makeParserResult<Stmt>(status, nullptr);

      // parseStmtCatch promises to return non-null unless we are
      // completing inside the catch's pattern.
      allClauses.push_back(clause.get());
    } while (Tok.is(tok::kw_catch) && !status.hasCodeCompletion());

    // Recover from all of the clauses failing to parse by returning a
    // normal do-statement.
    if (allClauses.empty()) {
      assert(status.isError());
      return makeParserResult(status,
                        new (Context) DoStmt(labelInfo, doLoc, body.get()));
    }

    return makeParserResult(status,
      DoCatchStmt::create(Context, labelInfo, doLoc, body.get(), allClauses));
  }

  SourceLoc whileLoc;

  // If we don't see a 'while', this is just the bare 'do' scoping
  // statement.
  if (!consumeIf(tok::kw_while, whileLoc)) {
    return makeParserResult(status,
                         new (Context) DoStmt(labelInfo, doLoc, body.get()));
  }

  // But if we do, advise the programmer that it's 'repeat' now.
  diagnose(doLoc, diag::do_while_now_repeat_while)
    .fixItReplace(doLoc, "repeat");
  status.setIsParseError();
  ParserResult<Expr> condition;
  if (Tok.is(tok::l_brace)) {
    SourceLoc lbraceLoc = Tok.getLoc();
    diagnose(whileLoc, diag::missing_condition_after_while);
    condition = makeParserErrorResult(new (Context) ErrorExpr(lbraceLoc));
  } else {
    condition = parseExpr(diag::expected_expr_repeat_while);
    status |= condition;
    if (condition.isNull() || condition.hasCodeCompletion())
      return makeParserResult<Stmt>(status, nullptr); // FIXME: better recovery
  }

  return makeParserResult(
      status,
      new (Context) RepeatWhileStmt(labelInfo, doLoc, condition.get(), whileLoc,
                                body.get()));
}

///  stmt-catch:
///    'catch' pattern ('where' expr)? stmt-brace
///
/// Note that this is not a "first class" statement; it can only
/// appear following a 'do' statement.
///
/// This routine promises to return a non-null result unless there was
/// a code-completion token in the pattern.
ParserResult<CatchStmt> Parser::parseStmtCatch() {
  SyntaxParsingContext CatchClauseCtxt(SyntaxContext, SyntaxKind::CatchClause);
  // A catch block has its own scope for variables bound out of the pattern.
  Scope S(this, ScopeKind::CatchVars);

  SourceLoc catchLoc = consumeToken(tok::kw_catch);

  SmallVector<VarDecl*, 4> boundDecls;

  ParserStatus status;
  GuardedPattern pattern;
  parseGuardedPattern(*this, pattern, status, boundDecls,
                      GuardedPatternContext::Catch, /* isFirst */ true);
  if (status.hasCodeCompletion()) {
    return makeParserCodeCompletionResult<CatchStmt>();
  }

  auto bodyResult = parseBraceItemList(diag::expected_lbrace_after_catch);
  status |= bodyResult;
  if (bodyResult.isNull()) {
    bodyResult = makeParserErrorResult(BraceStmt::create(Context, PreviousLoc,
                                                         {}, PreviousLoc,
                                                         /*implicit=*/ true));
  }

  auto result =
    new (Context) CatchStmt(catchLoc, pattern.ThePattern, pattern.WhereLoc,
                            pattern.Guard, bodyResult.get());
  return makeParserResult(status, result);
}

static bool isStmtForCStyle(Parser &P) {
  // If we have a leading identifier followed by a ':' or 'in', or have a
  // 'case', then this is obviously a for-each loop. "for in ..." is malformed
  // but it's obviously not a C-style for.
  if ((P.Tok.isIdentifierOrUnderscore() &&
         P.peekToken().isAny(tok::colon, tok::kw_in)) ||
      P.Tok.isAny(tok::kw_case, tok::kw_in))
    return false;

  // Otherwise, we have to look forward if we see ';' in control part.
  Parser::BacktrackingScope Backtrack(P);

  // The condition of a c-style-for loop can be parenthesized.
  auto HasLParen = P.consumeIf(tok::l_paren);

  // Skip until we see ';', or something that ends control part.
  while (true) {
    if (P.Tok.isAny(tok::eof, tok::kw_in, tok::l_brace, tok::r_brace,
                    tok::r_paren) || P.isStartOfStmt())
      return false;
    // If we saw newline before ';', consider it is a foreach statement.
    if (!HasLParen && P.Tok.isAtStartOfLine())
      return false;
    if (P.Tok.is(tok::semi))
      return true;
    P.skipSingle();
  }
}

/// 
///   stmt-for-each:
///     (identifier ':')? 'for' pattern 'in' expr-basic \
///             ('where' expr-basic)? stmt-brace
ParserResult<Stmt> Parser::parseStmtForEach(LabeledStmtInfo LabelInfo) {
  SyntaxContext->setCreateSyntax(SyntaxKind::ForInStmt);
  SourceLoc ForLoc = consumeToken(tok::kw_for);
  ParserStatus Status;
  ParserResult<Pattern> pattern;
  ParserResult<Expr> Container;

  // The C-style for loop which was supported in Swift2 and foreach-style-for
  // loop are conflated together into a single keyword, so we have to do some
  // lookahead to resolve what is going on.
  bool IsCStyleFor = isStmtForCStyle(*this);
  auto StartOfControl = Tok.getLoc();

  // Parse the pattern.  This is either 'case <refutable pattern>' or just a
  // normal pattern.
  if (consumeIf(tok::kw_case)) {
    llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
      T(InVarOrLetPattern, Parser::IVOLP_InMatchingPattern);
    pattern = parseMatchingPattern(/*isExprBasic*/true);
    pattern = parseOptionalPatternTypeAnnotation(pattern, /*isOptional*/false);
  } else if (!IsCStyleFor || Tok.is(tok::kw_var)) {
    // Change the parser state to know that the pattern we're about to parse is
    // implicitly mutable.  Bound variables can be changed to mutable explicitly
    // if desired by using a 'var' pattern.
    assert(InVarOrLetPattern == IVOLP_NotInVarOrLet &&
           "for-each loops cannot exist inside other patterns");
    InVarOrLetPattern = IVOLP_ImplicitlyImmutable;
    pattern = parseTypedPattern();
    assert(InVarOrLetPattern == IVOLP_ImplicitlyImmutable);
    InVarOrLetPattern = IVOLP_NotInVarOrLet;
  }
  
  SourceLoc InLoc;
  if (pattern.isNull()) {
    // Recover by creating a "_" pattern.
    pattern = makeParserErrorResult(new (Context) AnyPattern(SourceLoc()));
    consumeIf(tok::kw_in, InLoc);
  } else if (!IsCStyleFor) {
    parseToken(tok::kw_in, InLoc, diag::expected_foreach_in);
  }

  // Bound variables all get their initial values from the generator.
  pattern.get()->markHasNonPatternBindingInit();

  if (IsCStyleFor) {
    // Skip until start of body part.
    if (Tok.is(tok::l_paren)) {
      skipSingle();
    } else {
      // If not parenthesized, don't run over the line.
      while (Tok.isNot(tok::eof, tok::r_brace, tok::l_brace, tok::code_complete)
             && !Tok.isAtStartOfLine())
        skipSingle();
    }
    if (Tok.is(tok::code_complete))
      return makeParserCodeCompletionStatus();

    assert(StartOfControl != Tok.getLoc());
    SourceRange ControlRange(StartOfControl, PreviousLoc);
    Container = makeParserErrorResult(new (Context) ErrorExpr(ControlRange));
    diagnose(ForLoc, diag::c_style_for_stmt_removed)
      .highlight(ControlRange);
    Status = makeParserError();
  } else if (Tok.is(tok::l_brace)) {
    SourceLoc LBraceLoc = Tok.getLoc();
    diagnose(LBraceLoc, diag::expected_foreach_container);
    Container = makeParserErrorResult(new (Context) ErrorExpr(LBraceLoc));
  } else if (Tok.is(tok::code_complete)) {
    Container =
        makeParserResult(new (Context) CodeCompletionExpr(Tok.getLoc()));
    Container.setHasCodeCompletion();
    Status |= Container;
    if (CodeCompletion)
      CodeCompletion->completeForEachSequenceBeginning(
          cast<CodeCompletionExpr>(Container.get()));
    consumeToken(tok::code_complete);
  } else {
    Container = parseExprBasic(diag::expected_foreach_container);
    Status |= Container;
    if (Container.isNull())
      Container = makeParserErrorResult(new (Context) ErrorExpr(Tok.getLoc()));
    if (Container.isParseError())
      // Recover.
      skipUntilDeclStmtRBrace(tok::l_brace, tok::kw_where);
  }

  // Introduce a new scope and place the variables in the pattern into that
  // scope.
  // FIXME: We may want to merge this scope with the scope introduced by
  // the stmt-brace, as in C++.
  Scope S(this, ScopeKind::ForeachVars);
  
  // Introduce variables to the current scope.
  addPatternVariablesToScope(pattern.get());

  // Parse the 'where' expression if present.
  ParserResult<Expr> Where;
  if (Tok.is(tok::kw_where)) {
    SyntaxParsingContext WhereClauseCtxt(SyntaxContext,
                                         SyntaxKind::WhereClause);
    consumeToken();
    Where = parseExprBasic(diag::expected_foreach_where_expr);
    if (Where.isNull())
      Where = makeParserErrorResult(new (Context) ErrorExpr(Tok.getLoc()));
    Status |= Where;
  }

  // stmt-brace
  ParserResult<BraceStmt> Body =
      parseBraceItemList(diag::expected_foreach_lbrace);
  Status |= Body;
  if (Body.isNull())
    Body = makeParserResult(
        Body, BraceStmt::create(Context, ForLoc, {}, PreviousLoc, true));

  return makeParserResult(
      Status,
      new (Context) ForEachStmt(LabelInfo, ForLoc, pattern.get(), InLoc,
                                Container.get(), Where.getPtrOrNull(),
                                Body.get()));
}

///
///    stmt-switch:
///      (identifier ':')? 'switch' expr-basic '{' stmt-case+ '}'
ParserResult<Stmt> Parser::parseStmtSwitch(LabeledStmtInfo LabelInfo) {
  SyntaxContext->setCreateSyntax(SyntaxKind::SwitchStmt);
  SourceLoc SwitchLoc = consumeToken(tok::kw_switch);

  ParserStatus Status;
  ParserResult<Expr> SubjectExpr;
  SourceLoc SubjectLoc = Tok.getLoc();
  if (Tok.is(tok::l_brace)) {
    diagnose(SubjectLoc, diag::expected_switch_expr);
    SubjectExpr = makeParserErrorResult(new (Context) ErrorExpr(SubjectLoc));
  } else {
    SubjectExpr = parseExprBasic(diag::expected_switch_expr);
    if (SubjectExpr.hasCodeCompletion()) {
      return makeParserCodeCompletionResult<Stmt>();
    }
    if (SubjectExpr.isNull()) {
      SubjectExpr = makeParserErrorResult(new (Context) ErrorExpr(SubjectLoc));
    }
    Status |= SubjectExpr;
  }

  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok, diag::expected_lbrace_after_switch);
    return nullptr;
  }
  SourceLoc lBraceLoc = consumeToken(tok::l_brace);
  SourceLoc rBraceLoc;
  
  SmallVector<ASTNode, 8> cases;
  Status |= parseStmtCases(cases, /*IsActive=*/true);

  // We cannot have additional cases after a default clause. Complain on
  // the first offender.
  bool hasDefault = false;
  for (auto Element : cases) {
    if (!Element.is<Stmt*>()) continue;
    auto *CS = cast<CaseStmt>(Element.get<Stmt*>());
    if (hasDefault) {
      diagnose(CS->getLoc(), diag::case_after_default);
      break;
    }
    hasDefault |= CS->isDefault();
  }

  if (parseMatchingToken(tok::r_brace, rBraceLoc,
                         diag::expected_rbrace_switch, lBraceLoc)) {
    Status.setIsParseError();
  }

  return makeParserResult(
      Status, SwitchStmt::create(LabelInfo, SwitchLoc, SubjectExpr.get(),
                                 lBraceLoc, cases, rBraceLoc, Context));
}

ParserStatus
Parser::parseStmtCases(SmallVectorImpl<ASTNode> &cases, bool IsActive) {
  SyntaxParsingContext CasesContext(SyntaxContext, SyntaxKind::SwitchCaseList);
  ParserStatus Status;
  while (Tok.isNot(tok::r_brace, tok::eof,
                   tok::pound_endif, tok::pound_elseif, tok::pound_else)) {
    if (isAtStartOfSwitchCase(*this)) {
      ParserResult<CaseStmt> Case = parseStmtCase(IsActive);
      Status |= Case;
      if (Case.isNonNull())
        cases.emplace_back(Case.get());
    } else if (Tok.is(tok::pound_if)) {
      // '#if' in 'case' position can enclose one or more 'case' or 'default'
      // clauses.
      auto IfConfigResult = parseIfConfig(
        [&](SmallVectorImpl<ASTNode> &Elements, bool IsActive) {
          parseStmtCases(Elements, IsActive);
        });
      Status |= IfConfigResult;
      if (auto ICD = IfConfigResult.getPtrOrNull()) {
        cases.emplace_back(ICD);

        for (auto &Entry : ICD->getActiveClauseElements()) {
          if (Entry.is<Decl*>() && 
              (isa<IfConfigDecl>(Entry.get<Decl*>())))
            // Don't hoist nested '#if'.
            continue;

          assert((Entry.is<Stmt*>() && isa<CaseStmt>(Entry.get<Stmt*>())) ||
                 (Entry.is<Decl*>() && 
                   isa<PoundDiagnosticDecl>(Entry.get<Decl*>())));
          cases.push_back(Entry);
        }
      }
    } else if (Tok.is(tok::pound_warning) || Tok.is(tok::pound_error)) {
      auto PoundDiagnosticResult = parseDeclPoundDiagnostic();
      Status |= PoundDiagnosticResult;
      if (auto PDD = PoundDiagnosticResult.getPtrOrNull()) {
        cases.emplace_back(PDD);
      }
    } else if (Tok.is(tok::code_complete)) {
      if (CodeCompletion)
        CodeCompletion->completeCaseStmtKeyword();
      consumeToken(tok::code_complete);
      return makeParserCodeCompletionStatus();
    } else {
      // If there are non-case-label statements at the start of the switch body,
      // raise an error and recover by discarding them.
      diagnose(Tok, diag::stmt_in_switch_not_covered_by_case);

      while (Tok.isNot(tok::r_brace, tok::eof, tok::pound_elseif,
                       tok::pound_else, tok::pound_endif) &&
             !isTerminatorForBraceItemListKind(BraceItemListKind::Case, {})) {
        skipSingle();
      }
    }
  }
  return Status;
}

static ParserStatus
parseStmtCase(Parser &P, SourceLoc &CaseLoc,
              SmallVectorImpl<CaseLabelItem> &LabelItems,
              SmallVectorImpl<VarDecl *> &BoundDecls, SourceLoc &ColonLoc,
              Optional<MutableArrayRef<VarDecl *>> &CaseBodyDecls) {
  SyntaxParsingContext CaseContext(P.SyntaxContext,
                                   SyntaxKind::SwitchCaseLabel);
  ParserStatus Status;
  bool isFirst = true;
  
  CaseLoc = P.consumeToken(tok::kw_case);

  {
    SyntaxParsingContext ListContext(P.SyntaxContext, SyntaxKind::CaseItemList);

    while (true) {
      SyntaxParsingContext ItemContext(P.SyntaxContext, SyntaxKind::CaseItem);
      GuardedPattern PatternResult;
      parseGuardedPattern(P, PatternResult, Status, BoundDecls,
                          GuardedPatternContext::Case, isFirst);
      LabelItems.emplace_back(PatternResult.ThePattern, PatternResult.WhereLoc,
                              PatternResult.Guard);
      isFirst = false;
      if (!P.consumeIf(tok::comma))
        break;
    }

    // Grab the first case label item pattern and use it to initialize the case
    // body var decls.
    SmallVector<VarDecl *, 4> tmp;
    LabelItems.front().getPattern()->collectVariables(tmp);
    auto Result = P.Context.AllocateUninitialized<VarDecl *>(tmp.size());
    for (unsigned i : indices(tmp)) {
      auto *vOld = tmp[i];
      auto *vNew = new (P.Context) VarDecl(
          /*IsStatic*/ false, vOld->getSpecifier(), false /*IsCaptureList*/,
          vOld->getNameLoc(), vOld->getName(), vOld->getDeclContext());
      vNew->setHasNonPatternBindingInit();
      vNew->setImplicit();
      Result[i] = vNew;
    }
    CaseBodyDecls.emplace(Result);
  }

  ColonLoc = P.Tok.getLoc();
  if (!P.Tok.is(tok::colon)) {
    P.diagnose(P.Tok, diag::expected_case_colon, "case");
    Status.setIsParseError();
  } else
    P.consumeToken(tok::colon);

  return Status;
}

static ParserStatus
parseStmtCaseDefault(Parser &P, SourceLoc &CaseLoc,
                     SmallVectorImpl<CaseLabelItem> &LabelItems,
                     SourceLoc &ColonLoc) {
  SyntaxParsingContext CaseContext(P.SyntaxContext,
                                   SyntaxKind::SwitchDefaultLabel);
  ParserStatus Status;

  CaseLoc = P.consumeToken(tok::kw_default);

  // We don't allow 'where' guards on a 'default' block. For recovery
  // parse one if present.
  SourceLoc WhereLoc;
  ParserResult<Expr> Guard;
  if (P.Tok.is(tok::kw_where)) {
    P.diagnose(P.Tok, diag::default_with_where);
    WhereLoc = P.consumeToken(tok::kw_where);
    Guard = P.parseExpr(diag::expected_case_where_expr);
    Status |= Guard;
  }

  ColonLoc = P.Tok.getLoc();
  if (!P.Tok.is(tok::colon)) {
    P.diagnose(P.Tok, diag::expected_case_colon, "default");
    Status.setIsParseError();
  } else
    P.consumeToken(tok::colon);

  // Create an implicit AnyPattern to represent the default match.
  auto Any = new (P.Context) AnyPattern(CaseLoc);
  LabelItems.push_back(
      CaseLabelItem::getDefault(Any, WhereLoc, Guard.getPtrOrNull()));

  return Status;
}

namespace {

struct FallthroughFinder : ASTWalker {
  FallthroughStmt *result;

  FallthroughFinder() : result(nullptr) {}

  // We walk through statements.  If we find a fallthrough, then we got what
  // we came for.
  std::pair<bool, Stmt *> walkToStmtPre(Stmt *s) override {
    if (auto *f = dyn_cast<FallthroughStmt>(s)) {
      result = f;
    }

    return {true, s};
  }

  // Expressions, patterns and decls cannot contain fallthrough statements, so
  // there is no reason to walk into them.
  std::pair<bool, Expr *> walkToExprPre(Expr *e) override { return {false, e}; }
  std::pair<bool, Pattern *> walkToPatternPre(Pattern *p) override {
    return {false, p};
  }

  bool walkToDeclPre(Decl *d) override { return false; }
  bool walkToTypeLocPre(TypeLoc &tl) override { return false; }
  bool walkToTypeReprPre(TypeRepr *t) override { return false; }

  static FallthroughStmt *findFallthrough(Stmt *s) {
    FallthroughFinder finder;
    s->walk(finder);
    return finder.result;
  }
};

} // end anonymous namespace

ParserResult<CaseStmt> Parser::parseStmtCase(bool IsActive) {
  SyntaxParsingContext CaseContext(SyntaxContext, SyntaxKind::SwitchCase);
  // A case block has its own scope for variables bound out of the pattern.
  Scope S(this, ScopeKind::CaseVars, !IsActive);

  ParserStatus Status;

  SmallVector<CaseLabelItem, 2> CaseLabelItems;
  SmallVector<VarDecl *, 4> BoundDecls;

  SourceLoc UnknownAttrLoc;
  while (Tok.is(tok::at_sign)) {
    SyntaxParsingContext AttrCtx(SyntaxContext, SyntaxKind::Attribute);

    if (peekToken().isContextualKeyword("unknown")) {
      if (!UnknownAttrLoc.isValid()) {
        UnknownAttrLoc = consumeToken(tok::at_sign);
      } else {
        diagnose(Tok, diag::duplicate_attribute, false);
        diagnose(UnknownAttrLoc, diag::previous_attribute, false);
        consumeToken(tok::at_sign);
      }
      consumeIdentifier();

      SyntaxParsingContext Args(SyntaxContext, SyntaxKind::TokenList);
      if (Tok.is(tok::l_paren)) {
        diagnose(Tok, diag::unexpected_lparen_in_attribute, "unknown");
        skipSingle();
      }
    } else {
      consumeToken(tok::at_sign);
      diagnose(Tok, diag::unknown_attribute, Tok.getText());
      consumeIdentifier();

      SyntaxParsingContext Args(SyntaxContext, SyntaxKind::TokenList);
      if (Tok.is(tok::l_paren))
        skipSingle();
    }
  }

  SourceLoc CaseLoc;
  SourceLoc ColonLoc;
  Optional<MutableArrayRef<VarDecl *>> CaseBodyDecls;
  if (Tok.is(tok::kw_case)) {
    Status |= ::parseStmtCase(*this, CaseLoc, CaseLabelItems, BoundDecls,
                              ColonLoc, CaseBodyDecls);
  } else if (Tok.is(tok::kw_default)) {
    Status |= parseStmtCaseDefault(*this, CaseLoc, CaseLabelItems, ColonLoc);
  } else {
    llvm_unreachable("isAtStartOfSwitchCase() lied.");
  }

  assert(!CaseLabelItems.empty() && "did not parse any labels?!");

  // Add a scope so that the parser can find our body bound decls if it emits
  // optimized accesses.
  Optional<Scope> BodyScope;
  if (CaseBodyDecls) {
    BodyScope.emplace(this, ScopeKind::CaseVars);
    for (auto *v : *CaseBodyDecls) {
      setLocalDiscriminator(v);
      // If we had any bad redefinitions, we already diagnosed them against the
      // first case label item.
      if (v->hasName())
        addToScope(v, false /*diagnoseRedefinitions*/);
    }
  }

  SmallVector<ASTNode, 8> BodyItems;

  SourceLoc StartOfBody = Tok.getLoc();
  if (Tok.isNot(tok::r_brace) && !isAtStartOfSwitchCase(*this)) {
    Status |= parseBraceItems(BodyItems, BraceItemListKind::Case);
  } else if (Status.isSuccess()) {
    diagnose(CaseLoc, diag::case_stmt_without_body,
             CaseLabelItems.back().isDefault())
        .highlight(SourceRange(CaseLoc, ColonLoc))
        .fixItInsertAfter(ColonLoc, " break");
  }
  BraceStmt *Body;
  if (BodyItems.empty()) {
    Body = BraceStmt::create(Context, PreviousLoc, ArrayRef<ASTNode>(),
                             PreviousLoc, /*implicit=*/true);
  } else {
    Body = BraceStmt::create(Context, StartOfBody, BodyItems,
                             PreviousLoc, /*implicit=*/true);
  }

  return makeParserResult(
      Status, CaseStmt::create(Context, CaseLoc, CaseLabelItems, UnknownAttrLoc,
                               ColonLoc, Body, CaseBodyDecls, None,
                               FallthroughFinder::findFallthrough(Body)));
}

/// stmt-pound-assert:
///   '#assert' '(' expr (',' string_literal)? ')'
ParserResult<Stmt> Parser::parseStmtPoundAssert() {
  SyntaxContext->setCreateSyntax(SyntaxKind::PoundAssertStmt);

  SourceLoc startLoc = consumeToken(tok::pound_assert);
  SourceLoc endLoc;

  if (Tok.isNot(tok::l_paren)) {
    diagnose(Tok, diag::pound_assert_expected_lparen);
    return makeParserError();
  }
  SourceLoc LBLoc = consumeToken(tok::l_paren);

  auto conditionExprResult = parseExpr(diag::pound_assert_expected_expression);
  if (conditionExprResult.isParseError())
    return ParserStatus(conditionExprResult);

  StringRef message;
  if (consumeIf(tok::comma)) {
    if (Tok.isNot(tok::string_literal)) {
      diagnose(Tok.getLoc(), diag::pound_assert_expected_string_literal);
      return makeParserError();
    }

    auto messageOpt = getStringLiteralIfNotInterpolated(Tok.getLoc(),
                                                        "'#assert' message");
    consumeToken();
    if (!messageOpt)
      return makeParserError();

    message = *messageOpt;
  }

  if (parseMatchingToken(tok::r_paren, endLoc,
                         diag::pound_assert_expected_rparen, LBLoc)) {
    return makeParserError();
  }

  // We check this after consuming everything, so that the SyntaxContext
  // understands this statement even when the feature is disabled.
  if (!Context.LangOpts.EnableExperimentalStaticAssert) {
    diagnose(startLoc, diag::pound_assert_disabled);
    return makeParserError();
  }

  return makeParserResult<Stmt>(new (Context) PoundAssertStmt(
      SourceRange(startLoc, endLoc), conditionExprResult.get(), message));
}
