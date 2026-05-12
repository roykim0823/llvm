/// \file
/// \brief Recursive-descent parser for the tinylang grammar.
///
/// The parser pulls tokens from a \ref tinylang::Lexer "Lexer" and calls `Sema::actOn…` for
/// every successfully matched production — it never constructs AST nodes
/// directly. On a parse error a `parseXxx` method reports the offending
/// token via `err_expected` and returns `true`; the caller invokes
/// \ref tinylang::Parser::skipUntil "Parser::skipUntil" to resync.

#ifndef TINYLANG_PARSER_PARSER_H
#define TINYLANG_PARSER_PARSER_H

#include "tinylang/Basic/Diagnostic.h"
#include "tinylang/Lexer/Lexer.h"
#include "tinylang/Sema/Sema.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace tinylang {

/// Recursive-descent parser for the tinylang grammar.
///
/// Holds a single token of lookahead in \ref Tok. The parser is stateless
/// beyond that — all AST construction and symbol-table mutation happens in
/// \ref Sema.
class Parser {

  Lexer &Lex;       ///< Token source.

  Sema &Actions;    ///< Semantic actions (named `Actions` for readability).

  Token Tok;        ///< Single-token lookahead, refreshed by \ref advance.

  /// Convenience accessor for the shared diagnostics engine.
  DiagnosticsEngine &getDiagnostics() const {
    return Lex.getDiagnostics();
  }

  /// Pulls the next token from the lexer into \ref Tok.
  void advance() { Lex.next(Tok); }

  protected:  // protected so gtest can call into the recursive descent.

  /// Returns `false` if `Tok.is(ExpectedTok)`. Otherwise emits
  /// `err_expected` and returns `true` *without advancing*. Used by
  /// `parseXxx` callers to assert preconditions.
  bool expect(tok::TokenKind ExpectedTok) {
    if (Tok.is(ExpectedTok)) {
      return false;
    }
    const char *Expected =
        tok::getPunctuatorSpelling(ExpectedTok);
    if (!Expected)
      Expected = tok::getKeywordSpelling(ExpectedTok);
    llvm::StringRef Actual(Tok.getLocation().getPointer(),
                           Tok.getLength());
    getDiagnostics().report(Tok.getLocation(),
                            diag::err_expected, Expected,
                            Actual);
    return true;
  }

  /// Like \ref expect, but advances past the token on a match.
  /// Returns `true` (an error) when the current token doesn't match.
  bool consume(tok::TokenKind ExpectedTok) {
    if (Tok.is(ExpectedTok)) {
      advance();
      return false;
    }
    return true;
  }

  /// Error-recovery helper: advances until the current token is one of
  /// \p Toks (returning `false`) or EOF (returning `true`).
  template <typename... Tokens>
  bool skipUntil(Tokens &&...Toks) {
    while (true) {
      if ((... || Tok.is(Toks)))
        return false;

      if (Tok.is(tok::eof))
        return true;
      advance();
    }
  }

  /// @name Grammar productions
  ///
  /// Each method returns `false` on success and `true` on parse error.
  /// On success, AST construction has happened in \ref Sema and the
  /// out-parameter (if any) has been populated.
  /// @{
  bool parseCompilationUnit(ModuleDeclaration *&D);
  bool parseImport();
  bool parseBlock(DeclList &Decls, StmtList &Stmts);
  bool parseDeclaration(DeclList &Decls);
  bool parseConstantDeclaration(DeclList &Decls);
  bool parseVariableDeclaration(DeclList &Decls);
  bool parseProcedureDeclaration(DeclList &ParentDecls);
  bool parseFormalParameters(FormalParamList &Params,
                             Decl *&RetType);
  bool parseFormalParameterList(FormalParamList &Params);
  bool parseFormalParameter(FormalParamList &Params);
  bool parseStatementSequence(StmtList &Stmts);
  bool parseStatement(StmtList &Stmts);
  bool parseIfStatement(StmtList &Stmts);
  bool parseWhileStatement(StmtList &Stmts);
  bool parseReturnStatement(StmtList &Stmts);
  bool parseExpList(ExprList &Exprs);
  bool parseExpression(Expr *&E);
  bool parseRelation(OperatorInfo &Op);
  bool parseSimpleExpression(Expr *&E);
  bool parseAddOperator(OperatorInfo &Op);
  bool parseTerm(Expr *&E);
  bool parseMulOperator(OperatorInfo &Op);
  bool parseFactor(Expr *&E);
  bool parseQualident(Decl *&D);
  bool parseIdentList(IdentList &Ids);
  /// @}

public:
  /// Constructs a parser. The lexer and Sema must outlive this object.
  Parser(Lexer &Lex, Sema &Actions);

  /// Parses the input as a compilation unit and returns the resulting
  /// \ref ModuleDeclaration (or `nullptr` if the grammar didn't accept the
  /// input). Even when a tree is returned, callers should check
  /// `Diags.numErrors()` before consuming it.
  ModuleDeclaration *parse();
};
} // namespace tinylang
#endif
