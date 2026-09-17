//===- IRGen.cpp - LLVM IR generation from the Kaleidoscope AST -----------===//
//
// The IR emitted is ChapterA's, instruction for instruction. The *shape* is
// what changed, and it is worth comparing to ChapterA's CodeGen.cpp:
//
//  - no symbol table: variable uses index `allocas` by their resolved
//    VarId; the ScopedHashTable, its RAII scopes, and the initializer-
//    before-name ordering subtleties are all gone (Sema owns them now);
//  - no name lookups for callees: call sites carry their PrototypeAST;
//  - no user-error paths: unknown names, bad arity, and '=' misuse were
//    diagnosed by Sema, so emitExpr cannot fail and the error-propagation
//    null checks (and the unparented-block cleanup they required in the
//    if/then/else emission) have nothing left to propagate;
//  - debug info goes through the injected DebugInfoEmitter's hooks and the
//    optimizer is an injected component, as before.
//
// The only remaining failure is a verifier rejection -- an internal bug.
//
//===----------------------------------------------------------------------===//

#include "IRGen.h"
#include "Optimizer.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

using namespace toy;

IRGen::IRGen(llvm::Module &module, const Resolutions &resolutions,
             DiagnosticEngine &diags, DebugInfoEmitter &debug,
             Optimizer *optimizer)
    : module(module), context(module.getContext()), builder(context),
      resolutions(resolutions), diags(diags), debug(debug),
      optimizer(optimizer), allocas(resolutions.numVariables(), nullptr) {}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

llvm::Type *IRGen::doubleTy() { return llvm::Type::getDoubleTy(context); }

/// Materialize the function for a resolved prototype in the current module:
/// reuse it if it is already here, otherwise emit a declaration. (In JIT
/// mode each record gets a fresh module, so a callee defined in an earlier
/// module reappears here as a declaration -- the Resolutions table is the
/// cross-module registry.)
llvm::Function *IRGen::getFunction(PrototypeAST &proto) {
  if (auto *f = module.getFunction(proto.getName()))
    return f;
  return emitPrototype(proto);
}

/// Create an alloca in the entry block of the function, so mem2reg can
/// promote it.
llvm::AllocaInst *IRGen::createEntryBlockAlloca(llvm::Function *fn,
                                                llvm::StringRef varName) {
  llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  return tmp.CreateAlloca(doubleTy(), nullptr, varName);
}

/// Drop a function whose emission failed verification. If nothing
/// references it, remove it entirely; if earlier code already calls it,
/// revert it to a plain declaration instead -- erasing it would leave those
/// call sites with dangling operands (use-after-free at module teardown).
void IRGen::discardBrokenFunction(llvm::Function *fn) {
  fn->deleteBody();
  debug.functionEnd(*fn, /*succeeded=*/false);
  if (fn->use_empty())
    fn->eraseFromParent();
}

//===----------------------------------------------------------------------===//
// Expression code generation (dispatch on the AST node kind)
//===----------------------------------------------------------------------===//

llvm::Value *IRGen::emitExpr(ExprAST &expr) {
  debug.setLocation(&expr, builder);
  switch (expr.getKind()) {
  case ExprAST::Expr_Num:
    return emit(llvm::cast<NumberExprAST>(expr));
  case ExprAST::Expr_Var:
    return emit(llvm::cast<VariableExprAST>(expr));
  case ExprAST::Expr_Unary:
    return emit(llvm::cast<UnaryExprAST>(expr));
  case ExprAST::Expr_BinOp:
    return emit(llvm::cast<BinaryExprAST>(expr));
  case ExprAST::Expr_Call:
    return emit(llvm::cast<CallExprAST>(expr));
  case ExprAST::Expr_If:
    return emit(llvm::cast<IfExprAST>(expr));
  case ExprAST::Expr_For:
    return emit(llvm::cast<ForExprAST>(expr));
  case ExprAST::Expr_VarDecl:
    return emit(llvm::cast<VarExprAST>(expr));
  }
  llvm_unreachable("unhandled expression kind");
}

llvm::Value *IRGen::emit(NumberExprAST &num) {
  return llvm::ConstantFP::get(context, llvm::APFloat(num.getValue()));
}

llvm::Value *IRGen::emit(VariableExprAST &var) {
  llvm::AllocaInst *alloca = allocas[resolutions.boundVariable(var)];
  assert(alloca && "use emitted before its resolved declaration");
  return builder.CreateLoad(alloca->getAllocatedType(), alloca,
                            var.getName());
}

llvm::Value *IRGen::emit(UnaryExprAST &unary) {
  llvm::Value *operand = emitExpr(*unary.getOperand());
  llvm::Function *fn = getFunction(resolutions.callee(unary));
  return builder.CreateCall(fn, operand, "unop");
}

llvm::Value *IRGen::emit(BinaryExprAST &bin) {
  // Assignment is special: the LHS is not emitted as an expression. The
  // resolver guaranteed it is a variable reference with a binding.
  if (bin.getOp() == '=') {
    auto &lhs = *llvm::cast<VariableExprAST>(bin.getLHS());
    llvm::Value *val = emitExpr(*bin.getRHS());
    llvm::AllocaInst *alloca = allocas[resolutions.boundVariable(lhs)];
    assert(alloca && "assignment emitted before its resolved declaration");
    builder.CreateStore(val, alloca);
    return val;
  }

  llvm::Value *l = emitExpr(*bin.getLHS());
  llvm::Value *r = emitExpr(*bin.getRHS());

  switch (bin.getOp()) {
  case '+':
    return builder.CreateFAdd(l, r, "addtmp");
  case '-':
    return builder.CreateFSub(l, r, "subtmp");
  case '*':
    return builder.CreateFMul(l, r, "multmp");
  case '<':
    l = builder.CreateFCmpULT(l, r, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return builder.CreateUIToFP(l, doubleTy(), "booltmp");
  default:
    break;
  }

  // Not a builtin: a user-defined operator, bound by the resolver.
  llvm::Function *fn = getFunction(resolutions.callee(bin));
  llvm::Value *ops[] = {l, r};
  return builder.CreateCall(fn, ops, "binop");
}

llvm::Value *IRGen::emit(CallExprAST &call) {
  llvm::Function *callee = getFunction(resolutions.callee(call));
  assert(callee->arg_size() == call.getArgs().size() &&
         "resolver checked the arity");

  std::vector<llvm::Value *> args;
  for (auto &arg : call.getArgs())
    args.push_back(emitExpr(*arg));
  return builder.CreateCall(callee, args, "calltmp");
}

llvm::Value *IRGen::emit(IfExprAST &ifExpr) {
  llvm::Value *cond = emitExpr(*ifExpr.getCond());

  // Convert condition to a bool by comparing non-equal to 0.0.
  cond = builder.CreateFCmpONE(
      cond, llvm::ConstantFP::get(context, llvm::APFloat(0.0)), "ifcond");

  llvm::Function *fn = builder.GetInsertBlock()->getParent();

  // Create the three blocks. 'then' is appended now; 'else' and 'ifcont'
  // are appended after the corresponding arm is emitted so the block order
  // matches the source. (ChapterA needed cleanup code for the case where an
  // arm failed to emit while the later blocks were still unparented; with
  // infallible emission the blocks are always adopted.)
  llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(context, "then", fn);
  llvm::BasicBlock *elseBB = llvm::BasicBlock::Create(context, "else");
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(context, "ifcont");

  builder.CreateCondBr(cond, thenBB, elseBB);

  // Emit the 'then' arm.
  builder.SetInsertPoint(thenBB);
  llvm::Value *thenV = emitExpr(*ifExpr.getThen());
  builder.CreateBr(mergeBB);
  // Emission can change the current block; remember it for the PHI.
  thenBB = builder.GetInsertBlock();

  // Emit the 'else' arm.
  fn->insert(fn->end(), elseBB);
  builder.SetInsertPoint(elseBB);
  llvm::Value *elseV = emitExpr(*ifExpr.getElse());
  builder.CreateBr(mergeBB);
  elseBB = builder.GetInsertBlock();

  // Emit the merge block with the PHI.
  fn->insert(fn->end(), mergeBB);
  builder.SetInsertPoint(mergeBB);
  llvm::PHINode *phi = builder.CreatePHI(doubleTy(), 2, "iftmp");
  phi->addIncoming(thenV, thenBB);
  phi->addIncoming(elseV, elseBB);
  return phi;
}

// Output the for-loop as:
//   var = alloca double; store start -> var; br loop
// loop:
//   body; step; endcond (with the pre-increment value);
//   store (load var) + step -> var; br endcond, loop, afterloop
// afterloop:
llvm::Value *IRGen::emit(ForExprAST &forExpr) {
  llvm::Function *fn = builder.GetInsertBlock()->getParent();

  // The loop variable lives in an alloca (mem2reg rebuilds the PHI). Note
  // there is no scoping concern here anymore: the start expression may
  // mention an outer variable of the same name, but its use was bound to
  // the OUTER VarId by the resolver, so publishing this alloca early is
  // harmless.
  llvm::AllocaInst *alloca = createEntryBlockAlloca(fn, forExpr.getVarName());
  allocas[resolutions.declaredVariable(&forExpr, 0)] = alloca;

  llvm::Value *startVal = emitExpr(*forExpr.getStart());
  builder.CreateStore(startVal, alloca);

  llvm::BasicBlock *loopBB = llvm::BasicBlock::Create(context, "loop", fn);
  builder.CreateBr(loopBB);
  builder.SetInsertPoint(loopBB);

  // Emit the body; its value is ignored.
  emitExpr(*forExpr.getBody());

  // Emit the step value (1.0 if unspecified).
  llvm::Value *stepVal =
      forExpr.getStep() ? emitExpr(*forExpr.getStep())
                        : llvm::ConstantFP::get(context, llvm::APFloat(1.0));

  // Compute the end condition with the pre-increment value.
  llvm::Value *endCond = emitExpr(*forExpr.getEnd());

  // Reload, increment, and restore the alloca (the body may mutate it).
  llvm::Value *curVar = builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                           forExpr.getVarName());
  llvm::Value *nextVar = builder.CreateFAdd(curVar, stepVal, "nextvar");
  builder.CreateStore(nextVar, alloca);

  endCond = builder.CreateFCmpONE(
      endCond, llvm::ConstantFP::get(context, llvm::APFloat(0.0)),
      "loopcond");

  llvm::BasicBlock *afterBB =
      llvm::BasicBlock::Create(context, "afterloop", fn);
  builder.CreateCondBr(endCond, loopBB, afterBB);
  builder.SetInsertPoint(afterBB);

  // A for expression always evaluates to 0.0.
  return llvm::Constant::getNullValue(doubleTy());
}

llvm::Value *IRGen::emit(VarExprAST &varExpr) {
  llvm::Function *fn = builder.GetInsertBlock()->getParent();

  unsigned index = 0;
  for (auto &decl : varExpr.getVarNames()) {
    // 'var a = 1 in var a = a in ...': the initializer's 'a' was bound to
    // the OUTER declaration by the resolver, so evaluation order is all
    // that matters here -- initializer first, then this variable's alloca.
    llvm::Value *initVal =
        decl.second ? emitExpr(*decl.second)
                    : llvm::ConstantFP::get(context, llvm::APFloat(0.0));

    llvm::AllocaInst *alloca = createEntryBlockAlloca(fn, decl.first);
    builder.CreateStore(initVal, alloca);
    allocas[resolutions.declaredVariable(&varExpr, index)] = alloca;
    ++index;
  }

  return emitExpr(*varExpr.getBody());
}

//===----------------------------------------------------------------------===//
// Prototypes and functions
//===----------------------------------------------------------------------===//

llvm::Function *IRGen::emitPrototype(PrototypeAST &proto) {
  std::vector<llvm::Type *> doubles(proto.getArgs().size(), doubleTy());
  llvm::FunctionType *ft = llvm::FunctionType::get(doubleTy(), doubles, false);
  llvm::Function *fn = llvm::Function::Create(
      ft, llvm::Function::ExternalLinkage, proto.getName(), &module);

  unsigned idx = 0;
  for (auto &arg : fn->args())
    arg.setName(proto.getArgs()[idx++]);
  return fn;
}

llvm::Function *IRGen::emitFunction(FunctionAST &funcAST) {
  PrototypeAST &proto = *funcAST.getProto();

  llvm::Function *fn = getFunction(proto);
  assert(fn->empty() && "resolver rejected redefinitions");

  llvm::BasicBlock *bb = llvm::BasicBlock::Create(context, "entry", fn);
  builder.SetInsertPoint(bb);
  debug.functionBegin(proto, *fn, builder);

  // Arguments live in entry-block allocas so they are mutable.
  unsigned argIdx = 0;
  for (auto &arg : fn->args()) {
    llvm::AllocaInst *alloca = createEntryBlockAlloca(fn, arg.getName());
    builder.CreateStore(&arg, alloca);
    allocas[resolutions.declaredVariable(&proto, argIdx)] = alloca;
    ++argIdx; // DWARF parameter positions are 1-based
    debug.declareParameter(proto, *alloca, argIdx, builder);
  }

  builder.CreateRet(emitExpr(*funcAST.getBody()));
  debug.functionEnd(*fn, /*succeeded=*/true);

  // Validate the generated code -- the one failure that can still happen,
  // and it means a compiler bug, not bad user input.
  if (llvm::verifyFunction(*fn, &llvm::errs())) {
    diags.error(proto.loc(), "internal error: function '" + proto.getName() +
                                 "' failed verification");
    discardBrokenFunction(fn);
    return nullptr;
  }

  if (optimizer)
    optimizer->run(*fn);

  return fn;
}

llvm::Function *IRGen::emitRecord(RecordAST &record) {
  if (auto *func = llvm::dyn_cast<FunctionAST>(&record))
    return emitFunction(*func);

  // An extern: emit (or reuse) the declaration in the current module.
  return getFunction(*llvm::cast<ExternAST>(&record)->getProto());
}
