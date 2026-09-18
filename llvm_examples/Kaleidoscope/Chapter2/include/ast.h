#ifndef AST_H
#define AST_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace toy{

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//
// The parser will uses a combination of recursive descent parsing and operator
// precedence parsing to parse the input text and produce a AST.
// The AST for a program captures its behavior in such a way that it is easy for
// later stages of the compiler (e.g., code generation) to interpret.
//
// The AST is pure data: nodes carry what the parser saw plus a kind tag, and
// expose it through getters. Consumers of the tree -- code generation, from
// Chapter 3 on (CodeGenSession, codegen.h) -- walk it by switching on the
// kind. Nothing here depends on LLVM, so neither do the lexer and parser.

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  /// Kind tag for LLVM-style RTTI: `llvm::isa<>`/`llvm::cast<>` use each
  /// subclass's `classof()`, which compares against this tag.
  enum ExprASTKind {
    Expr_Num,
    Expr_Var,
    Expr_BinOp,
    Expr_Call,
  };

  virtual ~ExprAST() = default;

  ExprASTKind getKind() const { return Kind; }

protected:
  // Only concrete nodes construct the base, each stamping its own tag.
  // (Without a pure virtual, this is what keeps ExprAST abstract.)
  ExprAST(ExprASTKind Kind) : Kind(Kind) {}

private:
  const ExprASTKind Kind;
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : ExprAST(Expr_Num), Val(Val) {}

  double getVal() const { return Val; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Num; }
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : ExprAST(Expr_Var), Name(Name) {}

  const std::string &getName() const { return Name; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Var; }
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Expr_BinOp), Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  char getOp() const { return Op; }
  ExprAST *getLHS() const { return LHS.get(); }
  ExprAST *getRHS() const { return RHS.get(); }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_BinOp; }
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : ExprAST(Expr_Call), Callee(Callee), Args(std::move(Args)) {}

  const std::string &getCallee() const { return Callee; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Call; }
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes).
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}

  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getArgs() const { return Args; }
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  PrototypeAST *getProto() const { return Proto.get(); }
  ExprAST *getBody() const { return Body.get(); }
};
} // end namespace toy

#endif // AST_H
