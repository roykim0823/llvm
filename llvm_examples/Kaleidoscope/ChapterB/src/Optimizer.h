//===- Optimizer.h - Per-function optimization pipeline (internal) --------===//
//
// The pass pipeline pulled out of ChapterA's CodeGen Impl into its own
// component. This header is internal to the backend (it lives in src/, not
// include/toy/): only IRGen and the CodeGenSession facade see it, so it may
// include LLVM pass headers freely.
//
// One Optimizer serves one module/context pair. Destruction order relative
// to the LLVMContext matters (the instrumentation callbacks reference it);
// the members are declared so that they destruct in the safe order, and the
// facade destroys the whole Optimizer before moving the context out.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_SRC_OPTIMIZER_H
#define TOY_SRC_OPTIMIZER_H

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/StandardInstrumentations.h"

namespace llvm {
class Function;
class LLVMContext;
} // namespace llvm

namespace toy {

/// The per-function pipeline of the tutorial's Chapter 4 (plus mem2reg from
/// Chapter 7): PromotePass, InstCombine, Reassociate, GVN, SimplifyCFG.
class Optimizer {
public:
  explicit Optimizer(llvm::LLVMContext &context);

  /// Run the pipeline over one just-emitted function.
  void run(llvm::Function &fn);

private:
  // Declaration order is reversed destruction order: the analysis managers
  // must outlive the pass manager and the instrumentation that registered
  // into them.
  llvm::ModuleAnalysisManager mam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::FunctionAnalysisManager fam;
  llvm::LoopAnalysisManager lam;
  llvm::FunctionPassManager fpm;
  llvm::PassInstrumentationCallbacks pic;
  llvm::StandardInstrumentations si;
};

} // namespace toy

#endif // TOY_SRC_OPTIMIZER_H
