//===- AST.h - Node definitions for the Kaleidoscope AST ------------------===//
//
// AST in the style of the MLIR Toy tutorial: nodes are pure data with no
// virtual methods other than the destructor. Each expression node carries a
// kind tag (enabling llvm::isa/dyn_cast/cast without C++ RTTI) and a source
// Location. All traversal -- dumping, code generation -- is external.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_AST_H
#define TOY_AST_H

#include "toy/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace toy {

/// Base class for all expression nodes.
class ExprAST {
public:
  enum ExprASTKind {
    Expr_Num,
    Expr_Var,
    Expr_Unary,
    Expr_BinOp,
    Expr_Call,
    Expr_If,
    Expr_For,
    Expr_VarDecl, // 'var ... in' expression
  };

  ExprAST(ExprASTKind kind, Location location)
      : kind(kind), location(std::move(location)) {}
  virtual ~ExprAST() = default;

  ExprASTKind getKind() const { return kind; }
  const Location &loc() const { return location; }

private:
  const ExprASTKind kind;
  Location location;
};

/// A block-list of expressions.
using ExprASTList = std::vector<std::unique_ptr<ExprAST>>;

/// Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double val;

public:
  NumberExprAST(Location loc, double val)
      : ExprAST(Expr_Num, std::move(loc)), val(val) {}

  double getValue() const { return val; }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_Num; }
};

/// Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string name;

public:
  VariableExprAST(Location loc, llvm::StringRef name)
      : ExprAST(Expr_Var, std::move(loc)), name(name) {}

  llvm::StringRef getName() const { return name; }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_Var; }
};

/// Expression class for a user-defined unary operator.
class UnaryExprAST : public ExprAST {
  char opcode;
  std::unique_ptr<ExprAST> operand;

public:
  UnaryExprAST(Location loc, char opcode, std::unique_ptr<ExprAST> operand)
      : ExprAST(Expr_Unary, std::move(loc)), opcode(opcode),
        operand(std::move(operand)) {}

  char getOpcode() const { return opcode; }
  ExprAST *getOperand() const { return operand.get(); }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_Unary; }
};

/// Expression class for a binary operator (builtin or user-defined).
class BinaryExprAST : public ExprAST {
  char op;
  std::unique_ptr<ExprAST> lhs, rhs;

public:
  BinaryExprAST(Location loc, char op, std::unique_ptr<ExprAST> lhs,
                std::unique_ptr<ExprAST> rhs)
      : ExprAST(Expr_BinOp, std::move(loc)), op(op), lhs(std::move(lhs)),
        rhs(std::move(rhs)) {}

  char getOp() const { return op; }
  ExprAST *getLHS() const { return lhs.get(); }
  ExprAST *getRHS() const { return rhs.get(); }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_BinOp; }
};

/// Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string callee;
  ExprASTList args;

public:
  CallExprAST(Location loc, llvm::StringRef callee, ExprASTList args)
      : ExprAST(Expr_Call, std::move(loc)), callee(callee),
        args(std::move(args)) {}

  llvm::StringRef getCallee() const { return callee; }
  llvm::ArrayRef<std::unique_ptr<ExprAST>> getArgs() const { return args; }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_Call; }
};

/// Expression class for if/then/else.
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> cond, thenExpr, elseExpr;

public:
  IfExprAST(Location loc, std::unique_ptr<ExprAST> cond,
            std::unique_ptr<ExprAST> thenExpr,
            std::unique_ptr<ExprAST> elseExpr)
      : ExprAST(Expr_If, std::move(loc)), cond(std::move(cond)),
        thenExpr(std::move(thenExpr)), elseExpr(std::move(elseExpr)) {}

  ExprAST *getCond() const { return cond.get(); }
  ExprAST *getThen() const { return thenExpr.get(); }
  ExprAST *getElse() const { return elseExpr.get(); }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_If; }
};

/// Expression class for for/in.
class ForExprAST : public ExprAST {
  std::string varName;
  std::unique_ptr<ExprAST> start, end, step, body; // step may be null

public:
  ForExprAST(Location loc, llvm::StringRef varName,
             std::unique_ptr<ExprAST> start, std::unique_ptr<ExprAST> end,
             std::unique_ptr<ExprAST> step, std::unique_ptr<ExprAST> body)
      : ExprAST(Expr_For, std::move(loc)), varName(varName),
        start(std::move(start)), end(std::move(end)), step(std::move(step)),
        body(std::move(body)) {}

  llvm::StringRef getVarName() const { return varName; }
  ExprAST *getStart() const { return start.get(); }
  ExprAST *getEnd() const { return end.get(); }
  ExprAST *getStep() const { return step.get(); } // may be null
  ExprAST *getBody() const { return body.get(); }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_For; }
};

/// Expression class for 'var name (= init)?, ... in body'.
class VarExprAST : public ExprAST {
public:
  /// One declared variable: name and optional initializer (null -> 0.0).
  using VarDecl = std::pair<std::string, std::unique_ptr<ExprAST>>;

  VarExprAST(Location loc, std::vector<VarDecl> varNames,
             std::unique_ptr<ExprAST> body)
      : ExprAST(Expr_VarDecl, std::move(loc)), varNames(std::move(varNames)),
        body(std::move(body)) {}

  llvm::ArrayRef<VarDecl> getVarNames() const { return varNames; }
  ExprAST *getBody() const { return body.get(); }

  /// LLVM style RTTI
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_VarDecl; }

private:
  std::vector<VarDecl> varNames;
  std::unique_ptr<ExprAST> body;
};

/// This class represents the "prototype" for a function, which captures its
/// name, its argument names, and for user-defined operators the precedence.
/// It is not part of the ExprAST hierarchy.
class PrototypeAST {
  Location location;
  std::string name;
  std::vector<std::string> args;
  bool isOperator;
  unsigned precedence; // precedence if a binary op

public:
  PrototypeAST(Location location, llvm::StringRef name,
               std::vector<std::string> args, bool isOperator = false,
               unsigned precedence = 0)
      : location(std::move(location)), name(name), args(std::move(args)),
        isOperator(isOperator), precedence(precedence) {}

  const Location &loc() const { return location; }
  llvm::StringRef getName() const { return name; }
  llvm::ArrayRef<std::string> getArgs() const { return args; }

  bool isUnaryOp() const { return isOperator && args.size() == 1; }
  bool isBinaryOp() const { return isOperator && args.size() == 2; }

  char getOperatorName() const {
    assert(isUnaryOp() || isBinaryOp());
    return name[name.size() - 1];
  }

  unsigned getBinaryPrecedence() const { return precedence; }
};

/// Module-level record: either a function definition (which includes
/// anonymous top-level expressions) or an extern declaration. This mirrors
/// the Toy Ch7 RecordAST, giving the module an ordered, heterogeneous list.
class RecordAST {
public:
  enum RecordASTKind {
    Record_Function,
    Record_Extern,
  };

  RecordAST(RecordASTKind kind) : kind(kind) {}
  virtual ~RecordAST() = default;

  RecordASTKind getKind() const { return kind; }

private:
  const RecordASTKind kind;
};

/// A function definition: prototype plus a single-expression body.
/// Anonymous top-level expressions are wrapped in a FunctionAST whose
/// prototype is named "__anon_exprN" and flagged as top-level.
class FunctionAST : public RecordAST {
  std::unique_ptr<PrototypeAST> proto;
  std::unique_ptr<ExprAST> body;
  bool topLevelExpr;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> proto,
              std::unique_ptr<ExprAST> body, bool topLevelExpr = false)
      : RecordAST(Record_Function), proto(std::move(proto)),
        body(std::move(body)), topLevelExpr(topLevelExpr) {}

  PrototypeAST *getProto() const { return proto.get(); }
  ExprAST *getBody() const { return body.get(); }
  bool isTopLevelExpr() const { return topLevelExpr; }

  /// LLVM style RTTI
  static bool classof(const RecordAST *r) {
    return r->getKind() == Record_Function;
  }
};

/// An extern declaration: just a prototype.
class ExternAST : public RecordAST {
  std::unique_ptr<PrototypeAST> proto;

public:
  ExternAST(std::unique_ptr<PrototypeAST> proto)
      : RecordAST(Record_Extern), proto(std::move(proto)) {}

  PrototypeAST *getProto() const { return proto.get(); }

  /// LLVM style RTTI
  static bool classof(const RecordAST *r) {
    return r->getKind() == Record_Extern;
  }
};

/// A "module" of Kaleidoscope source: an ordered list of records.
class ModuleAST {
  std::vector<std::unique_ptr<RecordAST>> records;

public:
  ModuleAST(std::vector<std::unique_ptr<RecordAST>> records)
      : records(std::move(records)) {}

  auto begin() { return records.begin(); }
  auto end() { return records.end(); }
};

void dump(ModuleAST &);

} // namespace toy

#endif // TOY_AST_H
