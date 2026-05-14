/// \file
/// \brief Implementation of \ref tinylang::CGProcedure (Chapter 4).
///
/// The SSA construction routines (\ref CGProcedure::readLocalVariable,
/// \ref CGProcedure::readLocalVariableRecursive,
/// \ref CGProcedure::sealBlock) implement the on-the-fly variant of Braun et
/// al.'s algorithm: definitions are tracked per basic block, phi nodes are
/// inserted lazily on first use, and \ref CGProcedure::optimizePhi removes
/// trivial phis along the way.
///
/// \section newbie Newbie cheat-sheet
/// SSA = Static Single Assignment: every value is "written" exactly once.
/// Source code like `x := x + 1` becomes a *new* SSA value each time. When
/// control flow merges (after an `IF`, at a `WHILE` header), the two
/// candidate values for `x` are merged via a **phi** instruction. The trick
/// used here is that we never first emit allocas + loads/stores and let
/// `mem2reg` clean it up — instead we build the phis *while we are emitting*
/// the IR, using a per-block map of "what value does each tinylang variable
/// currently hold?".
///
/// Vocabulary used below:
///   - **CurrentDef[BB].Defs** — the per-block map: tinylang Decl → its
///     current SSA value in basic block BB.
///   - **sealed block** — a block whose every predecessor edge has already
///     been emitted. Sealing makes it safe to wire up phi operands.
///   - **incomplete phi** — a phi created in a block that is not sealed yet;
///     its operands are filled in later by \ref sealBlock.

#include "tinylang/CodeGen/CGProcedure.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Casting.h"

using namespace tinylang;

/// Records \p Val as the current definition of \p Decl in \p BB.
void CGProcedure::writeLocalVariable(llvm::BasicBlock *BB,
                                     Decl *Decl,
                                     llvm::Value *Val) {
  assert(BB && "Basic block is nullptr");
  assert(
      (llvm::isa<VariableDeclaration>(Decl) ||
       llvm::isa<FormalParameterDeclaration>(Decl)) &&
      "Declaration must be variable or formal parameter");
  assert(Val && "Value is nullptr");
  CurrentDef[BB].Defs[Decl] = Val;
}

llvm::Value *
CGProcedure::readLocalVariable(llvm::BasicBlock *BB,
                               Decl *Decl) {
  assert(BB && "Basic block is nullptr");
  assert(
      (llvm::isa<VariableDeclaration>(Decl) ||
       llvm::isa<FormalParameterDeclaration>(Decl)) &&
      "Declaration must be variable or formal parameter");
  auto Val = CurrentDef[BB].Defs.find(Decl);
  if (Val != CurrentDef[BB].Defs.end())
    return Val->second;
  return readLocalVariableRecursive(BB, Decl);  // search for the value in the predecessors
}

llvm::Value *CGProcedure::readLocalVariableRecursive(
    llvm::BasicBlock *BB, Decl *Decl) {
  llvm::Value *Val = nullptr;
  // Three cases — each maps to one branch below:
  //   (1) BB is unsealed: we do not yet know every predecessor, so we cannot
  //       fill in the phi operands. Drop a placeholder phi and remember it
  //       in IncompletePhis; sealBlock() will fix it up later.
  //   (2) BB has exactly one predecessor: there is nothing to merge — just
  //       fetch the value from that predecessor recursively.
  //   (3) BB has several predecessors and is sealed: create a phi and
  //       immediately fill in its operands from each predecessor.
  if (!CurrentDef[BB].Sealed) {
    // Case (1): incomplete phi — operands will be added in sealBlock().
    llvm::PHINode *Phi = addEmptyPhi(BB, Decl);
    CurrentDef[BB].IncompletePhis[Phi] = Decl;
    Val = Phi;
  } else if (auto *PredBB = BB->getSinglePredecessor()) {
    // Case (2): single predecessor — recurse.
    Val = readLocalVariable(PredBB, Decl);
  } else {
    // Case (3): multiple predecessors. We insert the phi *before* recursing
    // so that a back-edge that comes back to BB (e.g. a WHILE loop) sees
    // the phi as the current definition and terminates the recursion —
    // otherwise the lookup would loop forever.
    llvm::PHINode *Phi = addEmptyPhi(BB, Decl);
    writeLocalVariable(BB, Decl, Phi);
    Val = addPhiOperands(BB, Decl, Phi);
  }
  writeLocalVariable(BB, Decl, Val);
  return Val;
}

llvm::PHINode *
CGProcedure::addEmptyPhi(llvm::BasicBlock *BB, Decl *Decl) {
  return BB->empty()
             ? llvm::PHINode::Create(mapType(Decl), 0, "",
                                     BB)
             : llvm::PHINode::Create(mapType(Decl), 0, "",
#if __clang_major__ <= 17
                                     &BB->front());
#else
                                     BB->begin());
#endif
}

llvm::Value *CGProcedure::addPhiOperands(
    llvm::BasicBlock *BB, Decl *Decl, llvm::PHINode *Phi) {
  for (auto *PredBB : llvm::predecessors(BB))
    Phi->addIncoming(readLocalVariable(PredBB, Decl),
                     PredBB);
  return optimizePhi(Phi);
}

// A phi is "trivial" when all its incoming values are the same (ignoring
// self-references). Trivial phis bring no information but block downstream
// LLVM optimizations, so we remove them as we go.
//
// Why bother? Plenty of passes look at how a value is *defined*. A phi hides
// that definition behind a CFG-dependent merge, so the fewer phis the better.
llvm::Value *CGProcedure::optimizePhi(llvm::PHINode *Phi) {
  llvm::Value *Same = nullptr;
  // Walk all incoming values. Skip self-references (a phi that uses itself
  // is still "trivial" if every other operand agrees). The moment we see
  // two different non-self values, the phi is not trivial and we keep it.
  for (llvm::Value *V : Phi->incoming_values()) {
    if (V == Same || V == Phi)
      continue;
    if (Same && V != Same)  // two distinct incoming values: keep the phi
      return Phi;
    Same = V;
  }
  if (Same == nullptr) // no operands (unreachable block) — fall back to undef
    Same = llvm::UndefValue::get(Phi->getType());

  // Removing this phi might make *other* phis trivial too (e.g. a phi whose
  // only non-self operand was this one). Collect them before we erase so we
  // can revisit them.
  llvm::SmallVector<llvm::PHINode *, 8> CandidatePhis;
  for (llvm::Use &U : Phi->uses()) {
    if (auto *P =
            llvm::dyn_cast<llvm::PHINode>(U.getUser()))
      if (P != Phi)
        CandidatePhis.push_back(P);
  }
  Phi->replaceAllUsesWith(Same);
  Phi->eraseFromParent();
  for (auto *P : CandidatePhis)
    optimizePhi(P);
  return Same;
}

void CGProcedure::sealBlock(llvm::BasicBlock *BB) {
  assert(!CurrentDef[BB].Sealed &&
         "Attempt to seal already sealed block");
  for (auto PhiDecl : CurrentDef[BB].IncompletePhis) {
    addPhiOperands(BB, PhiDecl.second, PhiDecl.first);
  }
  CurrentDef[BB].IncompletePhis.clear();
  CurrentDef[BB].Sealed = true;
}

// Dispatch table for writes to a tinylang variable/parameter.
//   * local variable        -> update CurrentDef map (pure SSA, no memory)
//   * module-level global   -> emit a `store` to the global
//   * VAR formal parameter  -> emit a `store` through the pointer argument
//   * value formal parameter-> update CurrentDef map (treated like a local)
// New in Ch04 — this is what the AST.h widening of `AssignmentStatement::Var`
// from `VariableDeclaration*` to `Decl*` enables.
void CGProcedure::writeVariable(llvm::BasicBlock *BB,
                                Decl *D, llvm::Value *Val) {
  if (auto *V = llvm::dyn_cast<VariableDeclaration>(D)) {
    if (V->getEnclosingDecl() == Proc)
      writeLocalVariable(BB, D, Val);
    else if (V->getEnclosingDecl() ==
             CGM.getModuleDeclaration()) {
      Builder.CreateStore(Val, CGM.getGlobal(D));
    } else
      llvm::report_fatal_error(
          "Nested procedures not yet supported");
  } else if (auto *FP =
                 llvm::dyn_cast<FormalParameterDeclaration>(
                     D)) {
    if (FP->isVar()) {
      // VAR parameter — lowered as a pointer, so writes become stores.
      Builder.CreateStore(Val, FormalParams[FP]);
    } else
      writeLocalVariable(BB, D, Val);
  } else
    llvm::report_fatal_error("Unsupported declaration");
}

llvm::Value *CGProcedure::readVariable(llvm::BasicBlock *BB,
                                       Decl *D) {
  if (auto *V = llvm::dyn_cast<VariableDeclaration>(D)) {  // variales
    if (V->getEnclosingDecl() == Proc) // local variable
      return readLocalVariable(BB, D);
    else if (V->getEnclosingDecl() ==
             CGM.getModuleDeclaration()) { // global variable by loading
      return Builder.CreateLoad(mapType(D),
                                CGM.getGlobal(D));
    } else
      llvm::report_fatal_error(
          "Nested procedures not yet supported");
  } else if (auto *FP = llvm::dyn_cast<FormalParameterDeclaration>(D)) {  // parameters
    if (FP->isVar()) { // passing by a reference
      return Builder.CreateLoad(mapType(FP, false), FormalParams[FP]);
    } else
      return readLocalVariable(BB, D);  // passing by a value
  } else
    llvm::report_fatal_error("Unsupported declaration");
}

llvm::Type *CGProcedure::mapType(Decl *Decl,
                                 bool HonorReference) {
  if (auto *FP = llvm::dyn_cast<FormalParameterDeclaration>(
          Decl)) {
    if (FP->isVar() && HonorReference) // passed by reference -> LLVM's pointer
      return llvm::PointerType::get(CGM.getLLVMCtx(),
                                    /*AddressSpace=*/0);
    return CGM.convertType(FP->getType());
  }
  if (auto *V = llvm::dyn_cast<VariableDeclaration>(Decl))
    return CGM.convertType(V->getType());
  return CGM.convertType(llvm::cast<TypeDeclaration>(Decl));
}

llvm::FunctionType *CGProcedure::createFunctionType(
    ProcedureDeclaration *Proc) {
  llvm::Type *ResultTy = CGM.VoidTy;
  if (Proc->getRetType()) {
    ResultTy = mapType(Proc->getRetType());
  }
  auto FormalParams = Proc->getFormalParams();
  llvm::SmallVector<llvm::Type *, 8> ParamTypes;
  for (auto FP : FormalParams) {
    llvm::Type *Ty = mapType(FP);
    ParamTypes.push_back(Ty);
  }
  return llvm::FunctionType::get(ResultTy, ParamTypes,
                                 /*IsVarArgs=*/false);
}

llvm::Function *
CGProcedure::createFunction(ProcedureDeclaration *Proc,
                            llvm::FunctionType *FTy) {
  llvm::Function *Fn = llvm::Function::Create(
      Fty, llvm::GlobalValue::ExternalLinkage,
      CGM.mangleName(Proc), CGM.getModule());

  // Give parameters a name and add attributes to the function and params
  for (auto Pair : llvm::enumerate(Fn->args())) {
    llvm::Argument &Arg = Pair.value();
    FormalParameterDeclaration *FP =
        Proc->getFormalParams()[Pair.index()];
    if (FP->isVar()) {
      llvm::AttrBuilder Attr(CGM.getLLVMCtx());
      llvm::TypeSize Sz =
          CGM.getModule()->getDataLayout().getTypeStoreSize(
              CGM.convertType(FP->getType()));
      Attr.addDereferenceableAttr(Sz);
#if __clang_major__ <= 17
      // the pointer for the reference parameter cannot be passed around-
      // there are no copies of the pointer that outlive the call to the
      // function. -> not be captured!
      Attr.addAttribute(llvm::Attribute::NoCapture);
#else
      Attr.addAttribute(llvm::Attribute::Captures);  // temp solution
#endif
      Arg.addAttrs(Attr);
    }
    Arg.setName(FP->getName());
  }
  return Fn;
}

llvm::Value *
CGProcedure::emitInfixExpr(InfixExpression *E) {
  llvm::Value *Left = emitExpr(E->getLeft());
  llvm::Value *Right = emitExpr(E->getRight());
  llvm::Value *Result = nullptr;
  switch (E->getOperatorInfo().getKind()) {
  case tok::plus:
    Result = Builder.CreateNSWAdd(Left, Right);
    break;
  case tok::minus:
    Result = Builder.CreateNSWSub(Left, Right);
    break;
  case tok::star:
    Result = Builder.CreateNSWMul(Left, Right);
    break;
  case tok::kw_DIV:
    Result = Builder.CreateSDiv(Left, Right);
    break;
  case tok::kw_MOD:
    Result = Builder.CreateSRem(Left, Right);
    break;
  case tok::equal:
    Result = Builder.CreateICmpEQ(Left, Right);
    break;
  case tok::hash:
    Result = Builder.CreateICmpNE(Left, Right);
    break;
  case tok::less:
    Result = Builder.CreateICmpSLT(Left, Right);
    break;
  case tok::lessequal:
    Result = Builder.CreateICmpSLE(Left, Right);
    break;
  case tok::greater:
    Result = Builder.CreateICmpSGT(Left, Right);
    break;
  case tok::greaterequal:
    Result = Builder.CreateICmpSGE(Left, Right);
    break;
  case tok::kw_AND:
    Result = Builder.CreateAnd(Left, Right);
    break;
  case tok::kw_OR:
    Result = Builder.CreateOr(Left, Right);
    break;
  case tok::slash:
    // Divide by real numbers not supported.
    LLVM_FALLTHROUGH;
  default:
    llvm_unreachable("Wrong operator");
  }
  return Result;
}

llvm::Value *
CGProcedure::emitPrefixExpr(PrefixExpression *E) {
  llvm::Value *Result = emitExpr(E->getExpr());
  switch (E->getOperatorInfo().getKind()) {
  case tok::plus:
    // Identity - nothing to do.
    break;
  case tok::minus:
    Result = Builder.CreateNeg(Result);
    break;
  case tok::kw_NOT:
    Result = Builder.CreateNot(Result);
    break;
  default:
    llvm_unreachable("Wrong operator");
  }
  return Result;
}

llvm::Value *CGProcedure::emitExpr(Expr *E) {
  if (auto *Infix = llvm::dyn_cast<InfixExpression>(E)) {
    return emitInfixExpr(Infix);
  } else if (auto *Prefix =
                 llvm::dyn_cast<PrefixExpression>(E)) {
    return emitPrefixExpr(Prefix);
  } else if (auto *Var =
                 llvm::dyn_cast<VariableAccess>(E)) {
    auto *Decl = Var->getDecl();
    // With more languages features in place, here you need
    // to add array and record support.
    return readVariable(Curr, Decl);
  } else if (auto *Const =
                 llvm::dyn_cast<ConstantAccess>(E)) {
    return emitExpr(Const->getDecl()->getExpr());
  } else if (auto *IntLit =
                 llvm::dyn_cast<IntegerLiteral>(E)) {
    return llvm::ConstantInt::get(CGM.Int64Ty,
                                  IntLit->getValue());
  } else if (auto *BoolLit =
                 llvm::dyn_cast<BooleanLiteral>(E)) {
    return llvm::ConstantInt::get(CGM.Int1Ty,
                                  BoolLit->getValue());
  }
  llvm::report_fatal_error("Unsupported expression");
}

void CGProcedure::emitStmt(AssignmentStatement *Stmt) {
  auto *Val = emitExpr(Stmt->getExpr());
  writeVariable(Curr, Stmt->getVar(), Val);
}

void CGProcedure::emitStmt(ProcedureCallStatement *Stmt) {
  llvm::report_fatal_error("not implemented");
}

void CGProcedure::emitStmt(IfStatement *Stmt) {
  bool HasElse = Stmt->getElseStmts().size() > 0;

  // Create the required basic blocks.
  llvm::BasicBlock *IfBB = llvm::BasicBlock::Create(
      CGM.getLLVMCtx(), "if.body", Fn);
  llvm::BasicBlock *ElseBB =
      HasElse ? llvm::BasicBlock::Create(CGM.getLLVMCtx(),
                                         "else.body", Fn)
              : nullptr;
  llvm::BasicBlock *AfterIfBB = llvm::BasicBlock::Create(
      CGM.getLLVMCtx(), "after.if", Fn);

  llvm::Value *Cond = emitExpr(Stmt->getCond());
  Builder.CreateCondBr(Cond, IfBB,
                       HasElse ? ElseBB : AfterIfBB);
  sealBlock(Curr);

  setCurr(IfBB);
  emit(Stmt->getIfStmts());
  if (!Curr->getTerminator()) {
    Builder.CreateBr(AfterIfBB);
  }
  sealBlock(Curr);

  if (HasElse) {
    setCurr(ElseBB);
    emit(Stmt->getElseStmts());
    if (!Curr->getTerminator()) {
      Builder.CreateBr(AfterIfBB);
    }
    sealBlock(Curr);
  }
  setCurr(AfterIfBB);
}

// `WHILE Cond DO Body END` lowers to the classic three-block diamond:
//
//        Curr                      (block before the loop)
//          |
//          v
//     +--------+        false
//     | Cond   |--------------------+
//     +--------+                    |
//        | true                     |
//        v                          v
//     +--------+              +-----------+
//     | Body   |              | AfterLoop |
//     +--------+              +-----------+
//        |  back edge to Cond
//        +-->-->-->
//
// Note the sealing order: WhileCondBB has *two* predecessors (Curr and the
// back-edge from WhileBodyBB) so we cannot seal it until *after* the body's
// terminator has been emitted. That is why the body block is created before
// we wire up Cond — the back edge is what unlocks Cond's sealing.
void CGProcedure::emitStmt(WhileStatement *Stmt) {
  // The basic block for the condition.
  llvm::BasicBlock *WhileCondBB = llvm::BasicBlock::Create(
      CGM.getLLVMCtx(), "while.cond", Fn);  // CGM.getLLVMCtx() returns the LLVM context.
                                            // Fn is the current function.
  // The basic block for the while body.
  llvm::BasicBlock *WhileBodyBB = llvm::BasicBlock::Create(
      CGM.getLLVMCtx(), "while.body", Fn);
  // The basic block after the while statement.
  llvm::BasicBlock *AfterWhileBB = llvm::BasicBlock::Create(
      CGM.getLLVMCtx(), "after.while", Fn);

  Builder.CreateBr(WhileCondBB);
  sealBlock(Curr);

  setCurr(WhileCondBB);  // Set the current basic block to the while condition block
  llvm::Value *Cond = emitExpr(Stmt->getCond());
  Builder.CreateCondBr(Cond, WhileBodyBB, AfterWhileBB);

  setCurr(WhileBodyBB);  // Generate the loop body
  emit(Stmt->getWhileStmts());
  Builder.CreateBr(WhileCondBB);  // add a branch back to the basic block of the condition
  sealBlock(WhileCondBB); // both predecessors of Cond are now emitted -> safe to seal
  sealBlock(Curr);

  setCurr(AfterWhileBB);  // The empty basic block following WHILE becomes the new current basic block
}

void CGProcedure::emitStmt(ReturnStatement *Stmt) {
  if (Stmt->getRetVal()) {
    llvm::Value *RetVal = emitExpr(Stmt->getRetVal());
    Builder.CreateRet(RetVal);
  } else {
    Builder.CreateRetVoid();
  }
}

void CGProcedure::emit(const StmtList &Stmts) {
  for (auto *S : Stmts) {
    if (auto *Stmt = llvm::dyn_cast<AssignmentStatement>(S))
      emitStmt(Stmt);
    else if (auto *Stmt =
                 llvm::dyn_cast<ProcedureCallStatement>(S))
      emitStmt(Stmt);
    else if (auto *Stmt = llvm::dyn_cast<IfStatement>(S))
      emitStmt(Stmt);
    else if (auto *Stmt = llvm::dyn_cast<WhileStatement>(S))
      emitStmt(Stmt);
    else if (auto *Stmt =
                 llvm::dyn_cast<ReturnStatement>(S))
      emitStmt(Stmt);
    else
      llvm_unreachable("Unknown statement");
  }
}

// Top-level driver for procedure lowering. The high-level recipe:
//   1. Build the llvm::Function and an "entry" basic block.
//   2. Seed CurrentDef[entry] with each parameter's incoming Argument so
//      that subsequent reads of the parameter return the SSA value.
//   3. For aggregate locals (arrays/records in later chapters), reserve
//      stack space via an alloca. Scalar locals live purely in SSA.
//   4. Emit the body statement-by-statement.
//   5. Tie off control flow: add an implicit `ret void` if the user forgot,
//      and seal the final block.
void CGProcedure::run(ProcedureDeclaration *Proc) {
  this->Proc = Proc;
  Fty = createFunctionType(Proc);
  Fn = createFunction(Proc, Fty);

  llvm::BasicBlock *BB = llvm::BasicBlock::Create(
      CGM.getLLVMCtx(), "entry", Fn);
  setCurr(BB);

  for (auto Pair : llvm::enumerate(Fn->args())) {
    llvm::Argument *Arg = &Pair.value();
    FormalParameterDeclaration *FP =
        Proc->getFormalParams()[Pair.index()];
    // Create mapping FormalParameter -> llvm::Argument for
    // VAR parameters (used by writeVariable/readVariable to emit
    // load/store through the pointer argument).
    FormalParams[FP] = Arg;
    writeLocalVariable(Curr, FP, Arg);
  }

  for (auto *D : Proc->getDecls()) {
    if (auto *Var =
            llvm::dyn_cast<VariableDeclaration>(D)) {
      llvm::Type *Ty = mapType(Var);
      if (Ty->isAggregateType()) {
        // Aggregate locals cannot live in SSA registers — they need a stack
        // slot. (Tinylang in Ch04 has no aggregates yet, but this is the
        // hook for arrays/records added in later chapters.)
        llvm::Value *Val = Builder.CreateAlloca(Ty);
        writeLocalVariable(Curr, Var, Val);
      }
    }
  }

  auto Block = Proc->getStmts();
  emit(Proc->getStmts());
  if (!Curr->getTerminator()) {
    Builder.CreateRetVoid();  // implicit return for a proper procedure
  }
  sealBlock(Curr);
}
