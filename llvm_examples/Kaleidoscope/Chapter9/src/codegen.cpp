#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "log.h"
#include "debug.h"

using namespace toy;

extern std::unique_ptr<llvm::DIBuilder> DBuilder;
extern DebugInfo KSDbgInfo;

llvm::raw_ostream&  toy::indent(llvm::raw_ostream &O, int size) {
  return O << std::string(size, ' ');
}

// Helper function to look up a variable name in the function's symbol table and return its LLVM IR value.
// To allow each function to live in its own module, we'll need a way to re-generate
// previous function declarations into each new module we open
static llvm::Function *getFunction(std::string Name, IRGenContext &ctx) {
  // First, see if the function has already been added to the current module.
  if (auto *F = ctx.theModule->getFunction(Name))
    return F;

  // If not, check whether we can codegen the declaration from some existing prototype.
  auto FI = ctx.functionProtos.find(Name);
  if (FI != ctx.functionProtos.end())
    return FI->second->codegen(ctx);

  // If no existing prototype exists, return null.
  return nullptr;
}

llvm::Value *NumberExprAST::codegen(IRGenContext &ctx) {
  KSDbgInfo.emitLocation(this, ctx);  // Ch9
  return llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen(IRGenContext &ctx) {
  // Look this variable up in the function.
  llvm::AllocaInst *A = ctx.namedValues[Name];  // llvm::Value* -> llvm::AllocaInst*
  if (!A)
    return logErrorV("Unknown variable name");

  KSDbgInfo.emitLocation(this, ctx); // Ch9
  // Load the value instead of simple Value return
  return ctx.builder->CreateLoad(A->getAllocatedType(), A,  Name.c_str());
}

llvm::Value *UnaryExprAST::codegen(IRGenContext &ctx) {
  llvm::Value *OperandV = Operand->codegen(ctx);
  if (!OperandV)
    return nullptr;

  llvm::Function *F = getFunction(std::string("unary") + Opcode, ctx);
  if (!F)
    return logErrorV("Unknown unary operator");

  KSDbgInfo.emitLocation(this, ctx); // Ch9
  return ctx.builder->CreateCall(F, OperandV, "unop");
}

llvm::Value *BinaryExprAST::codegen(IRGenContext &ctx) {
  KSDbgInfo.emitLocation(this, ctx); // Ch9
  // To support the mutable variable
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
    llvm::Value *Variable = ctx.namedValues[LHSE->getName()];
    if (!Variable)
      return logErrorV("Unknown variable name in Binary Expr");

    ctx.builder->CreateStore(Val, Variable);
    return Val;
  }

  // Recursively emits code for the left-hand side of the expression, then the right-hand side,
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

llvm::Value *CallExprAST::codegen(IRGenContext &ctx) {
  KSDbgInfo.emitLocation(this, ctx); // Ch9

  // To support multiple modules, we need to re-generate the function declaration
  // into the new module if it doesn't already exist.
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

llvm::Value *IfExprAST::codegen(IRGenContext &ctx) {
  KSDbgInfo.emitLocation(this, ctx); // Ch9

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
  TheFunction->insert(TheFunction->end(), ElseBB);
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

/// Helper function for VarExprAST::codegen()
/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                          llvm::StringRef VarName, IRGenContext &ctx) {
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(llvm::Type::getDoubleTy(*ctx.theContext), nullptr, VarName);
}

// Output for-loop as:
//   var = alloca double **
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
llvm::Value *ForExprAST::codegen(IRGenContext &ctx) {
  // Make the new basic block for the loop header, inserting after current block.
  llvm::Function *TheFunction = ctx.builder->GetInsertBlock()->getParent();
  // Create an alloca for the variable in the entry block.
  llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, ctx);

  KSDbgInfo.emitLocation(this, ctx); // Ch9

  // Emit the start code first, without 'variable' in scope.
  llvm::Value *StartVal = Start->codegen(ctx);
  if (!StartVal)
    return nullptr;

  // Store the value into the alloca.
  ctx.builder->CreateStore(StartVal, Alloca);

  // Make the new basic block for the loop header, inserting after current
  // block.
  llvm::BasicBlock *LoopBB = llvm::BasicBlock::Create(*ctx.theContext, "loop", TheFunction);

  // Insert an explicit fall through from the current block to the LoopBB.
  ctx.builder->CreateBr(LoopBB);

  // Start insertion in LoopBB.
  ctx.builder->SetInsertPoint(LoopBB);

  // Within the loop, the variable is defined equal to the PHI node.  If it
  // shadows an existing variable, we have to restore it, so save it now.
  llvm::AllocaInst *OldVal = ctx.namedValues[VarName];  // Use AllocaInst instead of Value
  ctx.namedValues[VarName] = Alloca;

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
  llvm::Value *CurVar =
      ctx.builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
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
    ctx.namedValues[VarName] = OldVal;
  else
    ctx.namedValues.erase(VarName);

  // for expr always returns 0.0.
  return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*ctx.theContext));
}

llvm::Value *VarExprAST::codegen(IRGenContext &ctx) {
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
    OldBindings.push_back(ctx.namedValues[VarName]);

    // Remember this binding.
    ctx.namedValues[VarName] = Alloca;
  }

  KSDbgInfo.emitLocation(this, ctx); // Ch9

  // Codegen the body, now that all vars are in scope.
  llvm::Value *BodyVal = Body->codegen(ctx);
  if (!BodyVal)
    return nullptr;

  // Pop all our variables from scope.
  // for (unsigned i = 0, e = VarNames.size(); i != e; ++i)  // original code

  // In the test case InternalShadowing, var a=1.0, a=2.0.
  //   First a: OldBindings saves the Sentinel. NamedValues["a"] becomes Alloca1.
  //   Second a: OldBindings saves Alloca1. NamedValues["a"] becomes Alloca2.
  //   Restoration:
  //     i=0: NamedValues["a"] is set to OldBindings[0] (Sentinel).
  //     i=1: NamedValues["a"] is set to OldBindings[1] (Alloca1).
  // Final State: NamedValues["a"] is Alloca1, NOT the Sentinel!
  for (int i = VarNames.size()-1; i>=0; --i)  // FIX: the InternalShadowing Unit Test Cases
    ctx.namedValues[VarNames[i].first] = OldBindings[i];

  // Return the body computation.
  return BodyVal;
}

//-----------------------------
// Function Code Generation: prototypes and functions
//-----------------------------

// Used both for function bodies and extern declarations.
llvm::Function *PrototypeAST::codegen(IRGenContext &ctx) {
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

llvm::Function *FunctionAST::codegen(IRGenContext &ctx) {
  // Transfer ownership of the prototype to the FunctionProtos map, but keep a
  // reference to it for use below.  To Support JIT ---------------------
  auto &P = *Proto;
  ctx.functionProtos[Proto->getName()] = std::move(Proto);
  llvm::Function *TheFunction = getFunction(P.getName(), ctx);
  // ---------------------------------------------------------------------
  if (!TheFunction)
    return nullptr;

  // If this is an operator, install it.
  if (P.isBinaryOp())
    ctx.binopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();

  // Create a new basic block to start insertion into.
  llvm::BasicBlock *BB = llvm::BasicBlock::Create(*ctx.theContext, "entry", TheFunction);
  ctx.builder->SetInsertPoint(BB);

  // Create a subprogram DIE for this function.
  llvm::DIFile *Unit = DBuilder->createFile(KSDbgInfo.TheCU->getFilename(),
                                      KSDbgInfo.TheCU->getDirectory());
  llvm::DIScope *FContext = Unit;
  unsigned LineNo = P.getLine();
  unsigned ScopeLine = LineNo;
  llvm::DISubprogram *SP = DBuilder->createFunction(
      FContext, P.getName(), llvm::StringRef(), Unit, LineNo,
      CreateFunctionType(TheFunction->arg_size()), ScopeLine,
      llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
  TheFunction->setSubprogram(SP);

  // Push the current scope.
  KSDbgInfo.LexicalBlocks.push_back(SP);

  // Unset the location for the prologue emission (leading instructions with no
  // location in a function are considered part of the prologue and the debugger
  // will run past them when breaking on a function)
  KSDbgInfo.emitLocation(nullptr, ctx);
  //---------------------------------------------

  // Record the function arguments in the namedValues map.
  ctx.namedValues.clear();
  unsigned ArgIdx = 0;  // Ch9
  for (auto &Arg : TheFunction->args()) {
    // Create an alloca for this variable.
    llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName(), ctx);

    // Create a debug descriptor for the variable., Ch9
    llvm::DILocalVariable *D = DBuilder->createParameterVariable(
        SP, Arg.getName(), ++ArgIdx, Unit, LineNo, KSDbgInfo.getDoubleTy(),
        true);

    DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
                            llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
                            ctx.builder->GetInsertBlock());
    // ------------------------------

     // Store the initial value into the alloca.
    ctx.builder->CreateStore(&Arg, Alloca);

    // Add arguments to variable symbol table.
    ctx.namedValues[std::string(Arg.getName())] = Alloca;
  }

  KSDbgInfo.emitLocation(Body.get(), ctx); // Ch9

  if (llvm::Value *RetVal = Body->codegen(ctx)) {
    // Finish off the function.
    ctx.builder->CreateRet(RetVal);

    // Pop off the lexical block for the function.
    KSDbgInfo.LexicalBlocks.pop_back();

    // Validate the generated code, checking for consistency.
    llvm::verifyFunction(*TheFunction);

    // Run the optimizer on the function.
    // ctx.theFPM->run(*TheFunction, *ctx.theFAM);  // Ch9.

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();

  if (P.isBinaryOp())
    ctx.binopPrecedence.erase(P.getOperatorName());

  // Pop off the lexical block for the function since we added it
  // unconditionally.
  KSDbgInfo.LexicalBlocks.pop_back();  // Ch9

  return nullptr;
}
