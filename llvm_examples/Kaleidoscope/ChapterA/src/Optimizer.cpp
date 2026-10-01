//===- Optimizer.cpp - Per-function optimization pipeline -----------------===//

#include "Optimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

using namespace toy;

Optimizer::Optimizer(llvm::LLVMContext &context)
    : si(context, /*DebugLogging=*/false) {
  si.registerCallbacks(pic, &mam);

  // Promote allocas to registers (SSA construction for mutable variables).
  fpm.addPass(llvm::PromotePass());
  // Simple "peephole" optimizations and bit-twiddling.
  fpm.addPass(llvm::InstCombinePass());
  // Reassociate expressions.
  fpm.addPass(llvm::ReassociatePass());
  // Eliminate common subexpressions.
  fpm.addPass(llvm::GVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  fpm.addPass(llvm::SimplifyCFGPass());

  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerFunctionAnalyses(fam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
}

void Optimizer::run(llvm::Function &fn) { fpm.run(fn, fam); }
