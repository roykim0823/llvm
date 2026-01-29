#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "log.h"
#include "codegen_ctx.h"


using namespace toy;

// it keeps track of which values are defined in the current scope and what their LLVM representation is.
std::map<std::string, llvm::AllocaInst*> NamedValues;  // a.k.a. symbol table 
                                                    
extern std::map<char, int> BinopPrecedence;  // Defined in parser.cpp
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos; // To Support JIT
//-------------------------------------

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

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                          llvm::StringRef VarName, CodegenContext &ctx) {
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(llvm::Type::getDoubleTy(*ctx.theContext), nullptr, VarName);
}

llvm::Value *NumberExprAST::codegen(CodegenContext &ctx) {
  return llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen(CodegenContext &ctx) {
  // Look this variable up in the function.
  llvm::AllocaInst *A = NamedValues[Name];
  if (!A)
    return logErrorV("Unknown variable name");
  
  // Load the value.
  return ctx.builder->CreateLoad(A->getAllocatedType(), A,  Name.c_str());
}

llvm::Value *UnaryExprAST::codegen(CodegenContext &ctx) {
  llvm::Value *OperandV = Operand->codegen(ctx);
  if (!OperandV)
    return nullptr;

  llvm::Function *F = getFunction(std::string("unary") + Opcode, ctx);
  if (!F)
    return logErrorV("Unknown unary operator");

  return ctx.builder->CreateCall(F, OperandV, "unop");
}


llvm::Value *BinaryExprAST::codegen(CodegenContext &ctx) {
  // Special case '=' because we don't want to emit the LHS as an expression.
  if (Op == '=') {
    // Assignment requires the LHS to be an identifier.
    // This assume we're building without RTTI because LLVM builds that way by
    // default.  If you build LLVM with RTTI this can be changed to a
    // dynamic_cast for automatic error checking.
    VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
    if (!LHSE)
      return logErrorV("destination of '=' must be a variable");
      
    // Codegen the RHS.
    llvm::Value *Val = RHS->codegen(ctx);
    if (!Val)
      return nullptr;

    // Look up the name.
    llvm::Value *Variable = NamedValues[LHSE->getName()];
    if (!Variable)
      return logErrorV("Unknown variable name");

    ctx.builder->CreateStore(Val, Variable);
    return Val;
  }

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
    break;
  }

  // If it wasn't a builtin binary operator, it must be a user defined one. Emit
  // a call to it.
  llvm::Function *F = getFunction(std::string("binary") + Op, ctx);
  assert(F && "binary operator not found!");

  llvm::Value *Ops[] = {L, R};
  return ctx.builder->CreateCall(F, Ops, "binop");
}

llvm::Value *IfExprAST::codegen(CodegenContext &ctx) {
  llvm::Value *CondV = Cond->codegen(ctx);
  if (!CondV)
    return nullptr;

  // Convert condition to a bool by comparing non-equal to 0.0.
  CondV = ctx.builder->CreateFCmpONE(
      CondV, llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(0.0)), "ifcond");

  llvm::Function *TheFunction = ctx.builder->GetInsertBlock()->getParent();

  // Create blocks for the then and else cases.  Insert the 'then' block at the
  // end of the function.
  llvm::BasicBlock *ThenBB = llvm::BasicBlock::Create(*ctx.theContext, "then", TheFunction);
  llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(*ctx.theContext, "else");
  llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(*ctx.theContext, "ifcont");

  ctx.builder->CreateCondBr(CondV, ThenBB, ElseBB);

  // Emit then value.
  ctx.builder->SetInsertPoint(ThenBB);

  llvm::Value *ThenV = Then->codegen(ctx);
  if (!ThenV)
    return nullptr;

  ctx.builder->CreateBr(MergeBB);
  // Codegen of 'Then' can change the current block, update ThenBB for the PHI.
  ThenBB = ctx.builder->GetInsertBlock();

  // Emit else block.
  TheFunction->insert(TheFunction->end(), ElseBB);  // Add the ElseBB to TheFunction
  ctx.builder->SetInsertPoint(ElseBB);

  llvm::Value *ElseV = Else->codegen(ctx);
  if (!ElseV)
    return nullptr;

  ctx.builder->CreateBr(MergeBB);
  // Codegen of 'Else' can change the current block, update ElseBB for the PHI.
  ElseBB = ctx.builder->GetInsertBlock();

  // Emit merge block.
  TheFunction->insert(TheFunction->end(), MergeBB);
  ctx.builder->SetInsertPoint(MergeBB);
  llvm::PHINode *PN = ctx.builder->CreatePHI(llvm::Type::getDoubleTy(*ctx.theContext), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

// Output for-loop as:
//   var = alloca double
//   ...
//   start = startexpr
//   store start -> var
//   goto loop
// loop:
//   ...
//   bodyexpr
//   ...
// loopend:
//   step = stepexpr
//   endcond = endexpr
//
//   curvar = load var
//   nextvar = curvar + step
//   store nextvar -> var
//   br endcond, loop, endloop
// outloop:
llvm::Value *ForExprAST::codegen(CodegenContext &ctx) {
  // Make the new basic block for the loop header, inserting after current
  // block.
  llvm::Function *TheFunction = ctx.builder->GetInsertBlock()->getParent();
  llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, ctx);

  // Emit the start code first, without 'variable' in scope.
  llvm::Value *StartVal = Start->codegen(ctx);
  if (!StartVal)
    return nullptr;

  // Store the value into the alloca.
  ctx.builder->CreateStore(StartVal, Alloca);

  // Make the new basic block for the loop header, inserting after current block.
  llvm::BasicBlock *LoopBB = llvm::BasicBlock::Create(*ctx.theContext, "loop", TheFunction);

  // Insert an explicit fall through from the current block to the LoopBB.
  ctx.builder->CreateBr(LoopBB);

  // Start insertion in LoopBB.
  ctx.builder->SetInsertPoint(LoopBB);

    // Within the loop, the variable is defined equal to the PHI node.  If it
  // shadows an existing variable, we have to restore it, so save it now.
  llvm::AllocaInst *OldVal = NamedValues[VarName];
  NamedValues[VarName] = Alloca;

  // Emit the body of the loop.  This, like any other expr, can change the
  // current BB.  Note that we ignore the value computed by the body, but don't
  // allow an error.
  if (!Body->codegen(ctx))
    return nullptr;

  // Emit the step value.
  llvm::Value *StepVal = nullptr;
  if (Step) {
    StepVal = Step->codegen(ctx);
    if (!StepVal)
      return nullptr;
  } else {
    // If not specified, use 1.0.
    StepVal = llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(1.0));
  }

  // Compute the end condition.
  llvm::Value *EndCond = End->codegen(ctx);
  if (!EndCond)
    return nullptr;

  // Reload, increment, and restore the alloca.  This handles the case where
  // the body of the loop mutates the variable.
  llvm::Value *CurVar = ctx.builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
  llvm::Value *NextVar = ctx.builder->CreateFAdd(CurVar, StepVal, "nextvar");
  ctx.builder->CreateStore(NextVar, Alloca);

  // Convert condition to a bool by comparing non-equal to 0.0.
  EndCond = ctx.builder->CreateFCmpONE(
      EndCond, llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(0.0)), "loopcond");

  // Create the "after loop" block and insert it.
  llvm::BasicBlock *AfterBB =
      llvm::BasicBlock::Create(*ctx.theContext, "afterloop", TheFunction);

  // Insert the conditional branch into the end of LoopEndBB.
  ctx.builder->CreateCondBr(EndCond, LoopBB, AfterBB);

  // Any new code will be inserted in AfterBB.
  ctx.builder->SetInsertPoint(AfterBB);

  // Restore the unshadowed variable.
  if (OldVal)
    NamedValues[VarName] = OldVal;
  else
    NamedValues.erase(VarName);

  // for expr always returns 0.0.
  return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*ctx.theContext));
}
// llvm::Function *getFunction(std::string Name, CodegenContext &ctx) {
//   // First, see if the function has already been added to the current module.
//   if (auto *F = ctx.theModule->getFunction(Name))
//     return F;

//   // If not, check whether we can codegen the declaration from some existing prototype.
//   auto FI = FunctionProtos.find(Name);
//   if (FI != FunctionProtos.end())
//     return FI->second->codegen(ctx);

//   // If no existing prototype exists, return null.
//   return nullptr;
// }

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


llvm::Value *VarExprAST::codegen(CodegenContext &ctx) {
  std::vector<llvm::AllocaInst *> OldBindings;

  llvm::Function *TheFunction = ctx.builder->GetInsertBlock()->getParent();

  // Register all variables and emit their initializer.
  for (unsigned i = 0, e = VarNames.size(); i != e; ++i) {
    const std::string &VarName = VarNames[i].first;
    ExprAST *Init = VarNames[i].second.get();

    // Emit the initializer before adding the variable to scope, this prevents
    // the initializer from referencing the variable itself, and permits stuff
    // like this:
    //  var a = 1 in
    //    var a = a in ...   # refers to outer 'a'.
    llvm::Value *InitVal;
    if (Init) {
      InitVal = Init->codegen(ctx);
      if (!InitVal)
        return nullptr;
    } else { // If not specified, use 0.0.
      InitVal = llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(0.0));
    }

    llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, ctx);
    ctx.builder->CreateStore(InitVal, Alloca);
    // Remember the old variable binding so that we can restore the binding when
    // we unrecurse.
    OldBindings.push_back(NamedValues[VarName]);

    // Remember this binding.
    NamedValues[VarName] = Alloca;
  }

  // Codegen the body, now that all vars are in scope.
  llvm::Value *BodyVal = Body->codegen(ctx);
  if (!BodyVal)
    return nullptr;

  // Pop all our variables from scope.
  for (unsigned i = 0, e = VarNames.size(); i != e; ++i)
    NamedValues[VarNames[i].first] = OldBindings[i];

  // Return the body computation.
  return BodyVal;
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

   // If this is an operator, install it.
  if (P.isBinaryOp())
    BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();

  // Create a new basic block to start insertion into.
  llvm::BasicBlock *BB = llvm::BasicBlock::Create(*ctx.theContext, "entry", TheFunction);
  ctx.builder->SetInsertPoint(BB);

  // Record the function arguments in the NamedValues map.
  NamedValues.clear();
  for (auto &Arg : TheFunction->args()) {
    // Create an alloca for this variable.
    llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName(), ctx);

    // Store the initial value into the alloca.
    ctx.builder->CreateStore(&Arg, Alloca);

    // Add arguments to variable symbol table.
    NamedValues[std::string(Arg.getName())] = Alloca;
  }

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

  if (P.isBinaryOp())
    BinopPrecedence.erase(P.getOperatorName());
  return nullptr;
}
