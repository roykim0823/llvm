#ifndef AST_H
#define AST_H

#include <memory>
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

class AST;
class Expr;
class Factor;
class BinaryOp;
class WithDecl;

class ASTVisitor {
public:
  virtual void visit(AST &){};
  virtual void visit(Expr &){};
  virtual void visit(Factor &) = 0;
  virtual void visit(BinaryOp &) = 0;
  virtual void visit(WithDecl &) = 0;
};

class AST {
public:
  virtual ~AST() = default;
  virtual void accept(ASTVisitor &V) = 0;
};

// Just a common class hierarchy
class Expr : public AST {
public:
  Expr() {}
};

class Factor : public Expr {
public:
  enum ValueKind { Ident, Number };

private:
  ValueKind Kind;
  llvm::StringRef Val;

public:
  Factor(ValueKind Kind, llvm::StringRef Val)
      : Kind(Kind), Val(Val) {}
  ValueKind getKind() { return Kind; }
  llvm::StringRef getVal() { return Val; }
  virtual void accept(ASTVisitor &V) override {
    V.visit(*this);
  }
};

// In contrast to the parser, the BinaryOp class makes no distinction
// between +/- and */ operators.
// The precedence of the operators is handled in the tree structure.
class BinaryOp : public Expr {
public:
  enum Operator { Plus, Minus, Mul, Div };

private:
  std::unique_ptr<Expr> Left;
  std::unique_ptr<Expr> Right;
  Operator Op;

public:
  BinaryOp(Operator Op, std::unique_ptr<Expr> L, std::unique_ptr<Expr> R)
      : Op(Op), Left(std::move(L)), Right(std::move(R)) {}
  Expr *getLeft() { return Left.get(); }
  Expr *getRight() { return Right.get(); }
  Operator getOperator() { return Op; }
  virtual void accept(ASTVisitor &V) override {
    V.visit(*this);
  }
};

class WithDecl : public AST {
  using VarVector = llvm::SmallVector<llvm::StringRef, 8>;
  VarVector Vars;
  std::unique_ptr<Expr> E;

public:
  WithDecl(llvm::SmallVector<llvm::StringRef, 8> Vars,
           std::unique_ptr<Expr> E)
      : Vars(Vars), E(std::move(E)) {}
  VarVector::const_iterator begin() { return Vars.begin(); }
  VarVector::const_iterator end() { return Vars.end(); }
  Expr *getExpr() { return E.get(); }
  virtual void accept(ASTVisitor &V) override {
    V.visit(*this);
  }
};
#endif