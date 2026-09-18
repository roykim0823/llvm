//===----------------------------------------------------------------------===//
// Code generation: CodeGenSession::Impl
//===----------------------------------------------------------------------===//
// The tutorial's codegen state (TheContext, TheModule, Builder, NamedValues)
// and its per-node `codegen()` methods both live here, as members of one
// private struct. The AST does not know about LLVM; this file dispatches on
// each node's kind tag and emits IR for it.

#include "codegen.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/APFloat.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

#include "ast.h"
#include "log.h"

using namespace toy;

struct CodeGenSession::Impl {
  std::unique_ptr<llvm::LLVMContext> theContext;  // An opaque object that owns a lot of core LLVM data structures,
                                                  // such as the type and constant value tables.
  std::unique_ptr<llvm::Module> theModule;        // an LLVM construct that contains functions and global variables.
  std::unique_ptr<llvm::IRBuilder<>> builder;     // A helper object that makes it easy to generate LLVM instructions.
  std::map<std::string, llvm::Value *> namedValues;  // it keeps track of which values are defined in the current scope and what their LLVM representation is.
                                                     // a.k.a. symbol table

  Impl() {
    // Open a new context and module.
    theContext = std::make_unique<llvm::LLVMContext>();
    theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);
    // Create a new builder for the module.
    builder = std::make_unique<llvm::IRBuilder<>>(*theContext);
  }

  //-----------------------------
  // Expression Code Generation
  //-----------------------------

  // Dispatch on the node kind. Each case hands the concrete node to the
  // matching emit() overload below -- the tutorial's virtual codegen() split
  // into a switch plus one function per node type.
  llvm::Value *emitExpr(ExprAST &expr) {
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return emit(llvm::cast<NumberExprAST>(expr));
    case ExprAST::Expr_Var:
      return emit(llvm::cast<VariableExprAST>(expr));
    case ExprAST::Expr_BinOp:
      return emit(llvm::cast<BinaryExprAST>(expr));
    case ExprAST::Expr_Call:
      return emit(llvm::cast<CallExprAST>(expr));
    }
    llvm_unreachable("unknown expression kind");
  }

  llvm::Value *emit(NumberExprAST &num) {
    return llvm::ConstantFP::get(*theContext, llvm::APFloat(num.getVal()));
  }

  llvm::Value *emit(VariableExprAST &var) {
    // Look this variable up in the function.
    llvm::Value *V = namedValues[var.getName()];
    if (!V)
      return logErrorV("Unknown variable name");
    return V;
  }

  llvm::Value *emit(BinaryExprAST &bin) {
    // Recursively emits code for the left-hand side of the expression, then the right-hand side,
    // then, we compute the result of the binary expression.
    llvm::Value *L = emitExpr(*bin.getLHS());
    llvm::Value *R = emitExpr(*bin.getRHS());
    if (!L || !R)
      return nullptr;

    switch (bin.getOp()) {
    case '+':
      return builder->CreateFAdd(L, R, "addtmp");
    case '-':
      return builder->CreateFSub(L, R, "subtmp");
    case '*':
      return builder->CreateFMul(L, R, "multmp");
    case '<':
      L = builder->CreateFCmpULT(L, R, "cmptmp");
      // Convert bool 0/1 to double 0.0 or 1.0
      return builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*theContext), "booltmp");
    default:
      return logErrorV("invalid binary operator");
    }
  }

  llvm::Value *emit(CallExprAST &call) {
    // Look up the name in the global module table.
    llvm::Function *CalleeF = theModule->getFunction(call.getCallee());
    if (!CalleeF)
      return logErrorV("Unknown function referenced");

    // If argument mismatch error.
    const auto &Args = call.getArgs();
    if (CalleeF->arg_size() != Args.size())
      return logErrorV("Incorrect # arguments passed");

    std::vector<llvm::Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      ArgsV.push_back(emitExpr(*Args[i]));
      if (!ArgsV.back())
        return nullptr;
    }

    return builder->CreateCall(CalleeF, ArgsV, "calltmp");
  }

  //-----------------------------
  // Function Code Generation: prototypes and functions
  //-----------------------------

  // Used both for function bodies and extern declarations.
  llvm::Function *emitPrototype(PrototypeAST &proto) {
    const auto &Args = proto.getArgs();

    // Make the function type:  double(double,double) etc.
    std::vector<llvm::Type *> Doubles(Args.size(), llvm::Type::getDoubleTy(*theContext));
    llvm::FunctionType *FT =
        llvm::FunctionType::get(llvm::Type::getDoubleTy(*theContext), Doubles, false);

    // Create the IR Function corresponding to the Prototype
    llvm::Function *F =
        llvm::Function::Create(FT, llvm::Function::ExternalLinkage, proto.getName(), theModule.get());

    // Set names for all arguments.
    unsigned Idx = 0;
    for (auto &Arg : F->args())
      Arg.setName(Args[Idx++]);

    return F;
  }

  llvm::Function *emitFunction(FunctionAST &fn) {
    PrototypeAST &Proto = *fn.getProto();

    // First, check for an existing function from a previous 'extern' declaration.
    llvm::Function *TheFunction = theModule->getFunction(Proto.getName());

    if (!TheFunction)
      TheFunction = emitPrototype(Proto);

    if (!TheFunction)
      return nullptr;

    if (!TheFunction->empty())
      return (llvm::Function *)logErrorV("Function cannot be redefined.");

    // Create a new basic block to start insertion into.
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(*theContext, "entry", TheFunction);
    builder->SetInsertPoint(BB);

    // Record the function arguments in the NamedValues map.
    namedValues.clear();
    for (auto &Arg : TheFunction->args())
      namedValues[std::string(Arg.getName())] = &Arg;

    if (llvm::Value *RetVal = emitExpr(*fn.getBody())) {
      // Finish off the function.
      builder->CreateRet(RetVal);

      // Validate the generated code, checking for consistency.
      llvm::verifyFunction(*TheFunction);

      return TheFunction;
    }

    // Error reading body, remove function.
    TheFunction->eraseFromParent();
    return nullptr;
  }
};

//===----------------------------------------------------------------------===//
// Public facade
//===----------------------------------------------------------------------===//

CodeGenSession::CodeGenSession() : impl(std::make_unique<Impl>()) {}

// Out of line so that Impl is a complete type where unique_ptr<Impl> is destroyed.
CodeGenSession::~CodeGenSession() = default;

llvm::Function *CodeGenSession::emitPrototype(PrototypeAST &proto) {
  return impl->emitPrototype(proto);
}

llvm::Function *CodeGenSession::emitFunction(FunctionAST &fn) {
  return impl->emitFunction(fn);
}

llvm::Module &CodeGenSession::currentModule() { return *impl->theModule; }

void CodeGenSession::eraseFunction(llvm::Function *fn) { fn->eraseFromParent(); }
