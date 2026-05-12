/// \file
/// \brief tinylang's typed AST.
///
/// Three parallel hierarchies — \ref tinylang::Decl "Decl",
/// \ref tinylang::Expr "Expr", \ref tinylang::Stmt "Stmt" — share a
/// common pattern:
///   - a private `Kind` enum and `getKind()`, populated by each leaf class,
///   - a public static `classof(const T*)` so LLVM-style RTTI (`llvm::isa`,
///     `dyn_cast`) works,
///   - data members initialised once at construction.
///
/// The parser does not allocate AST nodes directly — \ref tinylang::Sema "Sema" does, in its
/// `actOn…` methods. Memory ownership for AST nodes is intentionally simple
/// in Ch03 (leaks on shutdown; the process exits soon after).

#ifndef TINYLANG_AST_AST_H
#define TINYLANG_AST_AST_H

#include "tinylang/Basic/LLVM.h"
#include "tinylang/Basic/TokenKinds.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <string>
#include <vector>

namespace tinylang {

class Decl;
class FormalParameterDeclaration;
class Expr;
class Stmt;

/// Convenience aliases used throughout the AST and Sema.
/// @{
using DeclList = std::vector<Decl *>;
using FormalParamList =
    std::vector<FormalParameterDeclaration *>;
using ExprList = std::vector<Expr *>;
using StmtList = std::vector<Stmt *>;
/// @}

/// A name together with its source location.
class Ident {
  SMLoc Loc;
  StringRef Name;

public:
  Ident(SMLoc Loc, const StringRef &Name)
      : Loc(Loc), Name(Name) {}
  SMLoc getLocation() { return Loc; }
  const StringRef &getName() { return Name; }
};

/// `(location, name)` pairs gathered during identifier-list parsing
/// (e.g. `VAR a, b, c : INTEGER`).
using IdentList = std::vector<std::pair<SMLoc, StringRef>>;

/// Base class for every declaration kind.
///
/// Carries the discriminator used for LLVM-style RTTI, the enclosing
/// declaration (used in Ch04 for name mangling of nested decls), source
/// location, and name.
class Decl {
public:
  /// Concrete declaration kinds. Order matters for `classof` ranges.
  enum DeclKind {
    DK_Module, ///< \ref ModuleDeclaration
    DK_Const,  ///< \ref ConstantDeclaration
    DK_Type,   ///< \ref TypeDeclaration (only INTEGER/BOOLEAN in Ch3)
    DK_Var,    ///< \ref VariableDeclaration
    DK_Param,  ///< \ref FormalParameterDeclaration
    DK_Proc    ///< \ref ProcedureDeclaration
  };

private:
  const DeclKind Kind;

protected:
  /// Containing module/procedure (or `nullptr` for top-level decls).
  Decl *EnclosingDecL;
  SMLoc Loc;
  StringRef Name;

public:
  /// Default-ish constructor retained so gmock can subclass \ref Decl
  /// without supplying the full set of fields.
  Decl(DeclKind Kind = DK_Module): Kind(Kind) {};
  /// Standard constructor used by Sema.
  Decl(DeclKind Kind, Decl *EnclosingDecL, SMLoc Loc,
       StringRef Name)
      : Kind(Kind), EnclosingDecL(EnclosingDecL), Loc(Loc),
        Name(Name) {}

  /// Discriminator for `llvm::isa / dyn_cast`.
  DeclKind getKind() const { return Kind; }
  /// Source location of the declaring identifier.
  SMLoc getLocation() { return Loc; }
  /// Declared name.
  StringRef getName() { return Name; }
  /// Enclosing decl (e.g. a procedure's parent module). May be `nullptr`.
  Decl *getEnclosingDecl() { return EnclosingDecL; }
};

/// `MODULE Name; … END Name.` — the compilation-unit root.
class ModuleDeclaration : public Decl {
  DeclList Decls;
  StmtList Stmts;

public:
  /// Default-constructed instance, used by gmock subclasses in tests.
  ModuleDeclaration() {};
  /// Constructor for the opening `MODULE Name;` line, before the body is parsed.
  ModuleDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                    StringRef Name)
      : Decl(DK_Module, EnclosingDecL, Loc, Name) {}

  /// Fully-populated constructor (used by tests).
  ModuleDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                    StringRef Name, DeclList &Decls,
                    StmtList &Stmts)
      : Decl(DK_Module, EnclosingDecL, Loc, Name),
        Decls(Decls), Stmts(Stmts) {}

  /// Top-level declarations in this module (constants, variables, …).
  const DeclList &getDecls() { return Decls; }
  void setDecls(DeclList &D) { Decls = D; }
  /// Statements in the module's `BEGIN … END` block (may be empty).
  const StmtList &getStmts() { return Stmts; }
  void setStmts(StmtList &L) { Stmts = L; }

  /// LLVM-style RTTI hook.
  static bool classof(const Decl *D) {
    return D->getKind() == DK_Module;
  }
};

/// `CONST Name = Expr;`
class ConstantDeclaration : public Decl {
  Expr *E;

public:
  ConstantDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                      StringRef Name, Expr *E)
      : Decl(DK_Const, EnclosingDecL, Loc, Name), E(E) {}

  /// The expression bound to this constant (must be `isConst()`).
  Expr *getExpr() { return E; }

  static bool classof(const Decl *D) {
    return D->getKind() == DK_Const;
  }
};

/// A type-name declaration.
///
/// In Ch03 the only instances are the predeclared `INTEGER` and `BOOLEAN`
/// created by \ref Sema::initialize. Ch05 grows this into a hierarchy
/// covering alias/array/pointer/record types.
class TypeDeclaration : public Decl {
public:
  TypeDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                  StringRef Name)
      : Decl(DK_Type, EnclosingDecL, Loc, Name) {}

  static bool classof(const Decl *D) {
    return D->getKind() == DK_Type;
  }
};

/// `VAR Name : Ty;`
class VariableDeclaration : public Decl {
  TypeDeclaration *Ty;

public:
  VariableDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                      StringRef Name, TypeDeclaration *Ty)
      : Decl(DK_Var, EnclosingDecL, Loc, Name), Ty(Ty) {}

  /// Static type of this variable.
  TypeDeclaration *getType() { return Ty; }

  static bool classof(const Decl *D) {
    return D->getKind() == DK_Var;
  }
};

/// One formal parameter of a procedure heading.
///
/// `IsVar` distinguishes pass-by-reference (`VAR` keyword) from
/// pass-by-value parameters.
class FormalParameterDeclaration : public Decl {
  TypeDeclaration *Ty;
  bool IsVar;

public:
  FormalParameterDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                             StringRef Name,
                             TypeDeclaration *Ty,
                             bool IsVar)
      : Decl(DK_Param, EnclosingDecL, Loc, Name), Ty(Ty),
        IsVar(IsVar) {}

  /// Declared parameter type.
  TypeDeclaration *getType() { return Ty; }
  /// `true` if the parameter is a `VAR` parameter (passed by reference).
  bool isVar() { return IsVar; }

  static bool classof(const Decl *D) {
    return D->getKind() == DK_Param;
  }
};

/// `PROCEDURE Name(Params) [: RetType]; … END Name;`
///
/// Built in two passes: the heading (params + return type) is attached
/// before parsing the body so recursive calls resolve correctly; the
/// body's decls/statements are stored once parsing finishes.
class ProcedureDeclaration : public Decl {
  FormalParamList Params;
  TypeDeclaration *RetType;
  DeclList Decls;
  StmtList Stmts;

public:
  /// Heading-only constructor used at the `PROCEDURE Name` token.
  ProcedureDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                       StringRef Name)
      : Decl(DK_Proc, EnclosingDecL, Loc, Name) {}

  /// Fully-populated constructor (used by tests).
  ProcedureDeclaration(Decl *EnclosingDecL, SMLoc Loc,
                       StringRef Name,
                       FormalParamList &Params,
                       TypeDeclaration *RetType,
                       DeclList &Decls, StmtList &Stmts)
      : Decl(DK_Proc, EnclosingDecL, Loc, Name),
        Params(Params), RetType(RetType), Decls(Decls),
        Stmts(Stmts) {}

  /// Formal parameters in declaration order.
  const FormalParamList &getFormalParams() {
    return Params;
  }
  void setFormalParams(FormalParamList &FP) { Params = FP; }
  /// Declared return type, or `nullptr` for a proper procedure.
  TypeDeclaration *getRetType() { return RetType; }
  void setRetType(TypeDeclaration *Ty) { RetType = Ty; }

  /// Local declarations inside the procedure body.
  const DeclList &getDecls() { return Decls; }
  void setDecls(DeclList &D) { Decls = D; }
  /// Statements in the procedure body.
  const StmtList &getStmts() { return Stmts; }
  void setStmts(StmtList &L) { Stmts = L; }

  static bool classof(const Decl *D) {
    return D->getKind() == DK_Proc;
  }
};

/// Bundles a location with an operator token kind.
///
/// `IsUnspecified` lets a default-constructed instance signal "no operator
/// was parsed yet", which the parser uses while ferrying optional operators
/// between productions (e.g. an optional leading sign in `simpleExpression`).
class OperatorInfo {
  SMLoc Loc;
  uint32_t Kind : 16;
  uint32_t IsUnspecified : 1;

public:
  /// A default ("not specified") operator, used as a placeholder.
  OperatorInfo()
      : Loc(), Kind(tok::unknown), IsUnspecified(true) {}
  OperatorInfo(SMLoc Loc, tok::TokenKind Kind,
               bool IsUnspecified = false)
      : Loc(Loc), Kind(Kind), IsUnspecified(IsUnspecified) {
  }

  SMLoc getLocation() const { return Loc; }
  tok::TokenKind getKind() const {
    return static_cast<tok::TokenKind>(Kind);
  }
  /// `true` if this object stands for "no operator parsed".
  bool isUnspecified() const { return IsUnspecified; }
};

/// Base class for every expression node.
///
/// Carries the static type \ref Ty and an `IsConstant` flag (true for
/// literals and references to `CONST` declarations).
class Expr {
public:
  enum ExprKind {
    EK_Infix,  ///< \ref InfixExpression
    EK_Prefix, ///< \ref PrefixExpression
    EK_Int,    ///< \ref IntegerLiteral
    EK_Bool,   ///< \ref BooleanLiteral
    EK_Var,    ///< \ref VariableAccess
    EK_Const,  ///< \ref ConstantAccess
    EK_Func,   ///< \ref FunctionCallExpr
  };

private:
  const ExprKind Kind;
  TypeDeclaration *Ty;
  bool IsConstant;

protected:
  Expr(ExprKind Kind, TypeDeclaration *Ty, bool IsConst)
      : Kind(Kind), Ty(Ty), IsConstant(IsConst) {}

public:
  ExprKind getKind() const { return Kind; }
  /// Static type of this expression. May be `nullptr` for an error recovery node.
  TypeDeclaration *getType() { return Ty; }
  /// Allows Sema to refine the type after construction (Ch05 uses this).
  void setType(TypeDeclaration *T) { Ty = T; }
  /// `true` if this expression has a compile-time constant value.
  bool isConst() { return IsConstant; }
};

/// Binary operator application: `Left Op Right`.
class InfixExpression : public Expr {
  Expr *Left;
  Expr *Right;
  const OperatorInfo Op;

public:
  InfixExpression(Expr *Left, Expr *Right, OperatorInfo Op,
                  TypeDeclaration *Ty, bool IsConst)
      : Expr(EK_Infix, Ty, IsConst), Left(Left),
        Right(Right), Op(Op) {}

  Expr *getLeft() { return Left; }
  Expr *getRight() { return Right; }
  const OperatorInfo &getOperatorInfo() { return Op; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Infix;
  }
};

/// Unary prefix application: `Op E` (e.g. `NOT x`, `-x`).
class PrefixExpression : public Expr {
  Expr *E;
  const OperatorInfo Op;

public:
  PrefixExpression(Expr *E, OperatorInfo Op,
                   TypeDeclaration *Ty, bool IsConst)
      : Expr(EK_Prefix, Ty, IsConst), E(E), Op(Op) {}

  Expr *getExpr() { return E; }
  const OperatorInfo &getOperatorInfo() { return Op; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Prefix;
  }
};

/// An integer literal stored as an `llvm::APSInt` (so any radix and width
/// the lexer accepts can be represented losslessly).
class IntegerLiteral : public Expr {
  SMLoc Loc;
  llvm::APSInt Value;

public:
  IntegerLiteral(SMLoc Loc, const llvm::APSInt &Value,
                 TypeDeclaration *Ty)
      : Expr(EK_Int, Ty, true), Loc(Loc), Value(Value) {}
  llvm::APSInt &getValue() { return Value; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Int;
  }
};

/// `TRUE` / `FALSE`. The two unique instances are owned by \ref Sema.
class BooleanLiteral : public Expr {
  bool Value;

public:
  BooleanLiteral(bool Value, TypeDeclaration *Ty)
      : Expr(EK_Bool, Ty, true), Value(Value) {}
  bool getValue() { return Value; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Bool;
  }
};

/// Use of a variable or formal parameter as an r-value (`x`, `p`).
class VariableAccess : public Expr {
  Decl *Var;

public:
  VariableAccess(VariableDeclaration *Var)
      : Expr(EK_Var, Var->getType(), false), Var(Var) {}
  VariableAccess(FormalParameterDeclaration *Param)
      : Expr(EK_Var, Param->getType(), false), Var(Param) {}

  /// The underlying \ref VariableDeclaration or \ref FormalParameterDeclaration.
  Decl *getDecl() { return Var; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Var;
  }
};

/// Use of a `CONST` declaration as an r-value. Always `isConst()`.
class ConstantAccess : public Expr {
  ConstantDeclaration *Const;

public:
  ConstantAccess(ConstantDeclaration *Const)
      : Expr(EK_Const, Const->getExpr()->getType(), true),
        Const(Const) {}

  ConstantDeclaration *geDecl() { return Const; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Const;
  }
};

/// A function-style procedure call appearing inside an expression.
///
/// The called procedure must have a non-null return type — see
/// \ref Sema::actOnFunctionCall, which rejects calls of proper procedures.
class FunctionCallExpr : public Expr {
  ProcedureDeclaration *Proc;
  ExprList Params;

public:
  FunctionCallExpr(ProcedureDeclaration *Proc,
                   ExprList Params)
      : Expr(EK_Func, Proc->getRetType(), false),
        Proc(Proc), Params(Params) {}

  ProcedureDeclaration *geDecl() { return Proc; }
  const ExprList &getParams() { return Params; }

  static bool classof(const Expr *E) {
    return E->getKind() == EK_Func;
  }
};

/// Base class for every statement node.
class Stmt {
public:
  enum StmtKind {
    SK_Assign,   ///< \ref AssignmentStatement
    SK_ProcCall, ///< \ref ProcedureCallStatement
    SK_If,       ///< \ref IfStatement
    SK_While,    ///< \ref WhileStatement
    SK_Return    ///< \ref ReturnStatement
  };

private:
  const StmtKind Kind;

protected:
  Stmt(StmtKind Kind) : Kind(Kind) {}

public:
  StmtKind getKind() const { return Kind; }
};

/// `Var := E;`
class AssignmentStatement : public Stmt {
  VariableDeclaration *Var;
  Expr *E;

public:
  AssignmentStatement(VariableDeclaration *Var, Expr *E)
      : Stmt(SK_Assign), Var(Var), E(E) {}

  VariableDeclaration *getVar() { return Var; }
  Expr *getExpr() { return E; }

  static bool classof(const Stmt *S) {
    return S->getKind() == SK_Assign;
  }
};

/// `Proc(args);` — procedure call where the return value (if any) is discarded.
class ProcedureCallStatement : public Stmt {
  ProcedureDeclaration *Proc;
  ExprList Params;

public:
  ProcedureCallStatement(ProcedureDeclaration *Proc,
                         ExprList &Params)
      : Stmt(SK_ProcCall), Proc(Proc), Params(Params) {}

  ProcedureDeclaration *getProc() { return Proc; }
  const ExprList &getParams() { return Params; }

  static bool classof(const Stmt *S) {
    return S->getKind() == SK_ProcCall;
  }
};

/// `IF Cond THEN IfStmts [ELSE ElseStmts] END`
///
/// `ElseStmts` is empty when no ELSE branch was parsed.
class IfStatement : public Stmt {
  Expr *Cond;
  StmtList IfStmts;
  StmtList ElseStmts;

public:
  IfStatement(Expr *Cond, StmtList &IfStmts,
              StmtList &ElseStmts)
      : Stmt(SK_If), Cond(Cond), IfStmts(IfStmts),
        ElseStmts(ElseStmts) {}

  Expr *getCond() { return Cond; }
  const StmtList &getIfStmts() { return IfStmts; }
  const StmtList &getElseStmts() { return ElseStmts; }

  static bool classof(const Stmt *S) {
    return S->getKind() == SK_If;
  }
};

/// `WHILE Cond DO Stmts END`
class WhileStatement : public Stmt {
  Expr *Cond;
  StmtList Stmts;

public:
  WhileStatement(Expr *Cond, StmtList &Stmts)
      : Stmt(SK_While), Cond(Cond), Stmts(Stmts) {}

  Expr *getCond() { return Cond; }
  const StmtList &getWhileStmts() { return Stmts; }

  static bool classof(const Stmt *S) {
    return S->getKind() == SK_While;
  }
};

/// `RETURN [E];` — the expression is `nullptr` for a bare RETURN.
class ReturnStatement : public Stmt {
  Expr *RetVal;

public:
  ReturnStatement(Expr *RetVal)
      : Stmt(SK_Return), RetVal(RetVal) {}

  /// Return expression, or `nullptr` for `RETURN;`.
  Expr *getRetVal() { return RetVal; }

  static bool classof(const Stmt *S) {
    return S->getKind() == SK_Return;
  }
};

} // namespace tinylang
#endif
