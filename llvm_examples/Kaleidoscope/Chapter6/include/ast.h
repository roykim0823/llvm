#ifndef AST_H
#define AST_H

#include <cassert>
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
    Expr_If,
    Expr_For,
    Expr_Unary,
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

/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
  char Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
      : ExprAST(Expr_Unary), Opcode(Opcode), Operand(std::move(Operand)) {}

  char getOpcode() const { return Opcode; }
  ExprAST *getOperand() const { return Operand.get(); }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Unary; }
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

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : ExprAST(Expr_If), Cond(std::move(Cond)), Then(std::move(Then)),
        Else(std::move(Else)) {}

  ExprAST *getCond() const { return Cond.get(); }
  ExprAST *getThen() const { return Then.get(); }
  ExprAST *getElse() const { return Else.get(); }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_If; }
};

/// ForExprAST - Expression class for for/in.
class ForExprAST : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start,
             std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
             std::unique_ptr<ExprAST> Body)
      : ExprAST(Expr_For), VarName(VarName), Start(std::move(Start)),
        End(std::move(End)), Step(std::move(Step)), Body(std::move(Body)) {}

  const std::string &getVarName() const { return VarName; }
  ExprAST *getStart() const { return Start.get(); }
  ExprAST *getEnd() const { return End.get(); }
  ExprAST *getStep() const { return Step.get(); }   // may be null: the step is optional
  ExprAST *getBody() const { return Body.get(); }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_For; }
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes).
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
  // for user-defined op
  bool IsOperator;
  unsigned Precedence;  // Precedence if a binary op

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args,
               bool IsOperator = false, unsigned Prec = 0)
      : Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
        Precedence(Prec) {}

  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getArgs() const { return Args; }

  // for user-defined op
  bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
  bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

  char getOperatorName() const {
    assert(isUnaryOp() || isBinaryOp());
    return Name[Name.size() - 1];
  }

  unsigned getBinaryPrecedence() const { return Precedence; }
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
