//===- AST.cpp - Helper for printing out the Kaleidoscope AST -------------===//
//
// The AST dump implementation. The ASTDumper class is an implementation
// detail (anonymous namespace); the public API is the single free function
// toy::dump(ModuleAST&) declared in AST.h.
//
//===----------------------------------------------------------------------===//

#include "toy/AST.h"

#include "llvm/ADT/Twine.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;

namespace {

// RAII helper to manage increasing/decreasing the indentation as we traverse
// the AST
struct Indent {
  Indent(int &level) : level(level) { ++level; }
  ~Indent() { --level; }
  int &level;
};

/// Helper class that implements the AST tree traversal and print the nodes
/// along the way. The only data member is the current indentation level.
class ASTDumper {
public:
  void dump(ModuleAST *node);

private:
  void dump(ExprAST *expr);
  void dump(NumberExprAST *num);
  void dump(VariableExprAST *node);
  void dump(UnaryExprAST *node);
  void dump(BinaryExprAST *node);
  void dump(CallExprAST *node);
  void dump(IfExprAST *node);
  void dump(ForExprAST *node);
  void dump(VarExprAST *node);
  void dump(PrototypeAST *node);
  void dump(FunctionAST *node);
  void dump(ExternAST *node);

  // Actually print spaces matching the current indentation level
  void indent() {
    for (int i = 0; i < curIndent; i++)
      llvm::errs() << "  ";
  }
  int curIndent = 0;
};

} // namespace

/// Return a formatted string for the location of any node
template <typename T>
static std::string loc(T *node) {
  const auto &loc = node->loc();
  return (llvm::Twine("@") + *loc.file + ":" + llvm::Twine(loc.line) + ":" +
          llvm::Twine(loc.col))
      .str();
}

// Helper Macro to bump the indentation level and print the leading spaces for
// the current indentation
#define INDENT()                                                               \
  Indent level_(curIndent);                                                    \
  indent();

/// Dispatch to a generic expression to the appropriate subclass using RTTI
void ASTDumper::dump(ExprAST *expr) {
  llvm::TypeSwitch<ExprAST *>(expr)
      .Case<NumberExprAST, VariableExprAST, UnaryExprAST, BinaryExprAST,
            CallExprAST, IfExprAST, ForExprAST, VarExprAST>(
          [&](auto *node) { this->dump(node); })
      .Default([&](ExprAST *) {
        // No match, fallback to a generic message
        INDENT();
        llvm::errs() << "<unknown Expr, kind " << expr->getKind() << ">\n";
      });
}

/// A literal number, just print the value.
void ASTDumper::dump(NumberExprAST *num) {
  INDENT();
  llvm::errs() << num->getValue() << " " << loc(num) << "\n";
}

/// A variable reference, print the name.
void ASTDumper::dump(VariableExprAST *node) {
  INDENT();
  llvm::errs() << "var: " << node->getName() << " " << loc(node) << "\n";
}

/// A unary operation, print the operator and recurse into the operand.
void ASTDumper::dump(UnaryExprAST *node) {
  INDENT();
  llvm::errs() << "UnaryOp: " << node->getOpcode() << " " << loc(node) << "\n";
  dump(node->getOperand());
}

/// A binary operation, print the operator and recurse into both sides.
void ASTDumper::dump(BinaryExprAST *node) {
  INDENT();
  llvm::errs() << "BinOp: " << node->getOp() << " " << loc(node) << "\n";
  dump(node->getLHS());
  dump(node->getRHS());
}

/// Print a call expression, first the callee name and then the arguments.
void ASTDumper::dump(CallExprAST *node) {
  INDENT();
  llvm::errs() << "Call '" << node->getCallee() << "' [ " << loc(node) << "\n";
  for (auto &arg : node->getArgs())
    dump(arg.get());
  indent();
  llvm::errs() << "]\n";
}

/// Print an if expression: condition, then branch, else branch.
void ASTDumper::dump(IfExprAST *node) {
  INDENT();
  llvm::errs() << "If " << loc(node) << "\n";
  dump(node->getCond());
  indent();
  llvm::errs() << "Then\n";
  dump(node->getThen());
  indent();
  llvm::errs() << "Else\n";
  dump(node->getElse());
}

/// Print a for expression: induction variable, bounds, and body.
void ASTDumper::dump(ForExprAST *node) {
  INDENT();
  llvm::errs() << "For " << node->getVarName() << " " << loc(node) << "\n";
  dump(node->getStart());
  dump(node->getEnd());
  if (node->getStep())
    dump(node->getStep());
  indent();
  llvm::errs() << "Body\n";
  dump(node->getBody());
}

/// Print a var/in expression: each declared variable with its initializer,
/// then the body.
void ASTDumper::dump(VarExprAST *node) {
  INDENT();
  llvm::errs() << "Var " << loc(node) << "\n";
  for (auto &decl : node->getVarNames()) {
    indent();
    llvm::errs() << "  decl: " << decl.first << "\n";
    if (decl.second)
      dump(decl.second.get());
  }
  indent();
  llvm::errs() << "In\n";
  dump(node->getBody());
}

/// Print a function prototype: name, operator-ness, and argument names.
void ASTDumper::dump(PrototypeAST *node) {
  INDENT();
  llvm::errs() << "Proto '" << node->getName() << "' " << loc(node) << "\n";
  indent();
  llvm::errs() << "Params: [";
  llvm::interleaveComma(node->getArgs(), llvm::errs());
  llvm::errs() << "]";
  if (node->isBinaryOp())
    llvm::errs() << " binary op '" << node->getOperatorName()
                 << "' prec: " << node->getBinaryPrecedence();
  if (node->isUnaryOp())
    llvm::errs() << " unary op '" << node->getOperatorName() << "'";
  llvm::errs() << "\n";
}

/// Print a function: prototype then body.
void ASTDumper::dump(FunctionAST *node) {
  INDENT();
  llvm::errs() << (node->isTopLevelExpr() ? "TopLevelExpr \n" : "Function \n");
  dump(node->getProto());
  indent();
  llvm::errs() << "Body:\n";
  dump(node->getBody());
}

/// Print an extern declaration.
void ASTDumper::dump(ExternAST *node) {
  INDENT();
  llvm::errs() << "Extern \n";
  dump(node->getProto());
}

/// Print a module: recurse into every record in order.
void ASTDumper::dump(ModuleAST *node) {
  INDENT();
  llvm::errs() << "Module:\n";
  for (auto &record : *node) {
    llvm::TypeSwitch<RecordAST *>(record.get())
        .Case<FunctionAST, ExternAST>([&](auto *r) { this->dump(r); })
        .Default([&](RecordAST *) {
          indent();
          llvm::errs() << "<unknown Record, kind " << record->getKind()
                       << ">\n";
        });
  }
}

namespace toy {

// Public API
void dump(ModuleAST &module) { ASTDumper().dump(&module); }

} // namespace toy
