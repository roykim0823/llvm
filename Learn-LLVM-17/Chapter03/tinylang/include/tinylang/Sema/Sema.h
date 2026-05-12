/// \file
/// \brief The semantic analyzer (action object for the parser).
///
/// Every grammar rule in \ref tinylang::Parser "Parser" invokes a matching
/// `actOn…` method here. Sema is responsible for:
///   - building AST nodes (the parser never `new`s an AST node directly),
///   - managing the chain of \ref tinylang::Scope "Scope" objects,
///   - type checking expressions, statements, and parameter passing,
///   - reporting semantic errors via \ref tinylang::DiagnosticsEngine "DiagnosticsEngine".

#ifndef TINYLANG_SEMA_SEMA_H
#define TINYLANG_SEMA_SEMA_H

#include "tinylang/AST/AST.h"
#include "tinylang/Basic/Diagnostic.h"
#include "tinylang/Sema/Scope.h"
#include <memory>

namespace tinylang {

/// Action object driven by the parser; owns the symbol table and builds the AST.
///
/// The `actOn…` naming is borrowed from Clang's Sema and signals
/// "the parser just matched this production". Most methods append a freshly
/// constructed AST node to an out-parameter (`StmtList&`, `DeclList&`,
/// `Expr*&`, …) and may emit diagnostics. The few methods marked `virtual`
/// exist so gmock can intercept them in the parser unit tests.
class Sema {
  friend class EnterDeclScope;

  /// Pushes a fresh child scope (used by \ref EnterDeclScope).
  void enterScope(Decl *);
  /// Pops the current scope (used by \ref EnterDeclScope).
  void leaveScope();

  /// True if \p Op is a valid operator for operands of type \p Ty.
  bool isOperatorForType(tok::TokenKind Op,
                         TypeDeclaration *Ty);

  /// Diagnoses a call whose argument count/types don't match \p Formals.
  void checkFormalAndActualParameters(
      SMLoc Loc, const FormalParamList &Formals,
      const ExprList &Actuals);

  Scope *CurrentScope;       ///< Active lexical scope (top of the chain).
  Decl *CurrentDecl;         ///< Enclosing decl for newly created decls.
  DiagnosticsEngine &Diags;  ///< Where diagnostics go.

  /// Predeclared types and constants (populated in \ref initialize).
  /// @{
  TypeDeclaration *IntegerType;
  TypeDeclaration *BooleanType;
  BooleanLiteral *TrueLiteral;
  BooleanLiteral *FalseLiteral;
  ConstantDeclaration *TrueConst;
  ConstantDeclaration *FalseConst;
  /// @}

public:
  Sema(DiagnosticsEngine &Diags)
      : CurrentScope(nullptr), CurrentDecl(nullptr),
        Diags(Diags) {
    initialize();
  }
  /// Virtual so gmock can subclass \ref Sema in parser tests.
  virtual ~Sema() = default;

  /// Creates the global scope and inserts `INTEGER`, `BOOLEAN`,
  /// `TRUE`, `FALSE`. Called from the constructor.
  void initialize();

  // ---------------------------------------------------------------------
  // Module and import
  // ---------------------------------------------------------------------

  /// Builds an empty \ref ModuleDeclaration for the *opening* `MODULE …;`
  /// line. The trailing declarations and statements are attached later
  /// by the five-arg overload.
  virtual ModuleDeclaration *actOnModuleDeclaration(SMLoc Loc,
                                                    StringRef Name);

  /// Finalises a module: verifies that the trailing identifier matches
  /// the opening one, and stores the parsed decls/statements.
  virtual void actOnModuleDeclaration(ModuleDeclaration *ModDecl,
                                      SMLoc Loc, StringRef Name,
                                      DeclList &Decls,
                                      StmtList &Stmts);

  /// Always emits `err_not_yet_implemented` in Ch03 — imports are
  /// implemented later.
  virtual void actOnImport(StringRef ModuleName, IdentList &Ids);

  // ---------------------------------------------------------------------
  // Declarations
  // ---------------------------------------------------------------------

  /// `CONST Name = E;` — inserts a \ref ConstantDeclaration into \p Decls
  /// and the current scope.
  void actOnConstantDeclaration(DeclList &Decls, SMLoc Loc,
                                StringRef Name, Expr *E);

  /// `VAR a, b, … : T;` — creates one \ref VariableDeclaration per name
  /// in \p Ids using the resolved type \p D.
  void actOnVariableDeclaration(DeclList &Decls,
                                IdentList &Ids, Decl *D);

  /// `(VAR) a, b : T` — creates \ref FormalParameterDeclaration objects.
  /// \param Params Output list; one entry is appended per name in \p Ids.
  /// \param Ids    Parameter names with their source locations.
  /// \param D      Resolved type declaration for the type annotation `T`.
  /// \param IsVar  `true` for VAR (by-reference) parameters.
  void
  actOnFormalParameterDeclaration(FormalParamList &Params,
                                  IdentList &Ids, Decl *D,
                                  bool IsVar);

  /// Creates an empty \ref ProcedureDeclaration for the heading line.
  ProcedureDeclaration *
  actOnProcedureDeclaration(SMLoc Loc, StringRef Name);

  /// Attaches formal parameters and the (optional) return type after the
  /// heading line is parsed but before the body.
  void actOnProcedureHeading(ProcedureDeclaration *ProcDecl,
                             FormalParamList &Params,
                             Decl *RetType);

  /// Finalises a procedure: validates the trailing identifier and stores
  /// the body's decls and statements.
  void actOnProcedureDeclaration(
      ProcedureDeclaration *ProcDecl, SMLoc Loc,
      StringRef Name, DeclList &Decls, StmtList &Stmts);

  // ---------------------------------------------------------------------
  // Statements
  // ---------------------------------------------------------------------

  /// `D := E;` — type-checks compatibility and appends an
  /// \ref AssignmentStatement to \p Stmts.
  void actOnAssignment(StmtList &Stmts, SMLoc Loc, Decl *D,
                       Expr *E);
  /// Procedure call as a statement (no return value used).
  void actOnProcCall(StmtList &Stmts, SMLoc Loc, Decl *D,
                     ExprList &Params);
  /// `IF Cond THEN IfStmts [ELSE ElseStmts] END` — verifies \p Cond is BOOLEAN.
  void actOnIfStatement(StmtList &Stmts, SMLoc Loc,
                        Expr *Cond, StmtList &IfStmts,
                        StmtList &ElseStmts);
  /// `WHILE Cond DO WhileStmts END` — verifies \p Cond is BOOLEAN.
  void actOnWhileStatement(StmtList &Stmts, SMLoc Loc,
                           Expr *Cond,
                           StmtList &WhileStmts);
  /// `RETURN [RetVal];` — verifies the return value matches the enclosing
  /// procedure's return type (or that no value is given for a proper procedure).
  void actOnReturnStatement(StmtList &Stmts, SMLoc Loc,
                            Expr *RetVal);

  // ---------------------------------------------------------------------
  // Expressions
  // ---------------------------------------------------------------------

  /// Relational operator (`=`, `#`, `<`, `<=`, `>`, `>=`); result type is BOOLEAN.
  Expr *actOnExpression(Expr *Left, Expr *Right,
                        const OperatorInfo &Op);
  /// Additive operator (`+`, `-`, `OR`); typed by operand type.
  Expr *actOnSimpleExpression(Expr *Left, Expr *Right,
                              const OperatorInfo &Op);
  /// Multiplicative operator (`*`, `/`, `DIV`, `MOD`, `AND`).
  Expr *actOnTerm(Expr *Left, Expr *Right,
                  const OperatorInfo &Op);
  /// Unary `-` or `NOT`.
  Expr *actOnPrefixExpression(Expr *E,
                              const OperatorInfo &Op);
  /// Builds an \ref IntegerLiteral from a `123` / `0FFH` / `123B` spelling.
  Expr *actOnIntegerLiteral(SMLoc Loc, StringRef Literal);
  /// Builds a \ref VariableAccess (or a constant reference) for a declared name.
  Expr *actOnVariable(Decl *D);
  /// Builds a \ref FunctionCallExpr; rejects calls of proper procedures.
  Expr *actOnFunctionCall(Decl *D, ExprList &Params);

  /// Resolves one segment of a possibly-qualified identifier (`Mod.x`).
  /// \param Prev `nullptr` for the first segment; the previously resolved
  ///             module-like decl otherwise.
  /// \param Loc  Source location of \p Name (used for diagnostics).
  /// \param Name The segment to look up.
  Decl *actOnQualIdentPart(Decl *Prev, SMLoc Loc,
                           StringRef Name);
};

/// RAII helper that pushes a new scope on construction and pops it on
/// destruction. Used by the parser around module and procedure bodies.
class EnterDeclScope {
  Sema &Semantics;

public:
  EnterDeclScope(Sema &Semantics, Decl *D)
      : Semantics(Semantics) {
    Semantics.enterScope(D);
  }
  ~EnterDeclScope() { Semantics.leaveScope(); }
};
} // namespace tinylang
#endif
