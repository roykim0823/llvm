#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "log.h"
#include "codegen_ctx.h"

using namespace toy;

llvm::Value *NumberExprAST::codegen(CodegenContext &ctx) {
  return llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen(CodegenContext &ctx) {
  // Look this variable up in the function.
  llvm::Value *V = ctx.namedValues[Name];
  if (!V)
    return logErrorV("Unknown variable name");
  return V;
}

llvm::Value *BinaryExprAST::codegen(CodegenContext &ctx) {
  // Recursively emits code for the lef-hand side of the expression, then the righ-hand side,
  // then, we compute the result of the binary expression.
  llvm::Value *L = LHS->codegen(ctx);
  llvm::Value *R = RHS->codegen(ctx);
  if (!L || !R)
    return nullptr;

  switch (Op) {
  case '+':
    return ctx.builder->CreateFAdd(L, R, "addtmp");
  case '-':
    return ctx.builder->CreateFSub(L, R, "subtmp");
  case '*':
    return ctx.builder->CreateFMul(L, R, "multmp");
  case '<':
    L = ctx.builder->CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return ctx.builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*ctx.theContext), "booltmp");
  default:
    return logErrorV("invalid binary operator");
  }
}

static llvm::Function *getFunction(std::string Name, CodegenContext &ctx) {
  // First, see if the function has already been added to the current module.
  if (auto *F = ctx.theModule->getFunction(Name))
    return F;

  // If not, check whether we can codegen the declaration from some existing prototype.
  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end())
    return FI->second->codegen(ctx);

  // If no existing prototype exists, return null.
  return nullptr;
}

llvm::Value *CallExprAST::codegen(CodegenContext &ctx) {
  // Look up the name in the global module table.
  llvm::Function *CalleeF = getFunction(Callee, ctx);
 
  if (!CalleeF)
    return logErrorV("Unknown function referenced");

  // If argument mismatch error.
  if (CalleeF->arg_size() != Args.size())
    return logErrorV("Incorrect # arguments passed");

  std::vector<llvm::Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen(ctx));
    if (!ArgsV.back())
      return nullptr;
  }

  return ctx.builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

//-----------------------------
// Function Code Generation: prototypes and functions
//-----------------------------

// Used both for function bodies and extern declarations.
llvm::Function *PrototypeAST::codegen(CodegenContext &ctx) {
  // Make the function type:  double(double,double) etc.
  std::vector<llvm::Type *> Doubles(Args.size(), llvm::Type::getDoubleTy(*ctx.theContext));
  llvm::FunctionType *FT =
      llvm::FunctionType::get(llvm::Type::getDoubleTy(*ctx.theContext), Doubles, false);

  // Create the IR Function corresponding to the Prototype
  llvm::Function *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, ctx.theModule.get());

  // Set names for all arguments.
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++]);

  return F;
}

llvm::Function *FunctionAST::codegen(CodegenContext &ctx) {
  // Transfer ownership of the prototype to the FunctionProtos map, but keep a
  // reference to it for use below.  To Support JIT ---------------------
  auto &P = *Proto;
  FunctionProtos[Proto->getName()] = std::move(Proto);
  llvm::Function *TheFunction = getFunction(P.getName(), ctx);
  // ---------------------------------------------------------------------
  if (!TheFunction)
    return nullptr;

  // Create a new basic block to start insertion into.
  llvm::BasicBlock *BB = llvm::BasicBlock::Create(*ctx.theContext, "entry", TheFunction);
  ctx.builder->SetInsertPoint(BB);

  // Record the function arguments in the NamedValues map.
  ctx.namedValues.clear();
  for (auto &Arg : TheFunction->args())
    ctx.namedValues[std::string(Arg.getName())] = &Arg;

  if (llvm::Value *RetVal = Body->codegen(ctx)) {
    // Finish off the function.
    ctx.builder->CreateRet(RetVal);

    // Validate the generated code, checking for consistency.
    llvm::verifyFunction(*TheFunction);

    // Run the optimizer on the function.
    ctx.TheFPM->run(*TheFunction);

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();
  return nullptr;
}
