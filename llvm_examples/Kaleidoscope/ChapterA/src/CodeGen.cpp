//===- CodeGen.cpp - Backend session composing IRGen/Optimizer/DebugInfo --===//
//
// ChapterA's 570-line Impl carried IR emission, the pass pipeline, debug
// info, and module lifecycle in one struct. Here the Impl is only the
// composer: it owns the module/context pair and the session-lifetime
// prototype registry, wires the three components together, and rebuilds
// them per module. Each concern reads (and rebuilds) independently.
//
//===----------------------------------------------------------------------===//

#include "toy/CodeGen.h"
#include "toy/AST.h"
#include "toy/Diagnostics.h"
#include "toy/Sema.h"

#include "DebugInfo.h"
#include "IRGen.h"
#include "Optimizer.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

using namespace toy;

struct CodeGenSession::Impl {
  CodeGenOptions options;
  /// The name resolution every per-module IRGen consumes. It doubles as the
  /// cross-module function registry in JIT mode: a callee defined in an
  /// earlier (already handed-off) module is re-declared into the current
  /// one from its bound PrototypeAST. Non-owning; the ModuleAST that owns
  /// the prototypes must outlive the session.
  const Resolutions &resolutions;
  DiagnosticEngine &diags;

  /// Data layout applied to every module (set by the JIT or a TargetMachine).
  std::optional<llvm::DataLayout> dataLayout;

  // Per-module state, rebuilt by initializeModule() after every takeModule().
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<Optimizer> optimizer;      // null when !options.optimize
  std::unique_ptr<DebugInfoEmitter> debugInfo;
  std::unique_ptr<IRGen> irgen;

  Impl(CodeGenOptions opts, const Resolutions &resolutions,
       DiagnosticEngine &diags)
      : options(std::move(opts)), resolutions(resolutions), diags(diags) {
    initializeModule();
  }

  void initializeModule() {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("kaleidoscope", *context);
    if (dataLayout)
      module->setDataLayout(*dataLayout);

    optimizer = options.optimize ? std::make_unique<Optimizer>(*context)
                                 : nullptr;
    debugInfo = options.emitDebugInfo
                    ? std::make_unique<DebugInfoEmitter>(
                          *module, options.sourceFile, options.optimize)
                    : std::make_unique<DebugInfoEmitter>();
    irgen = std::make_unique<IRGen>(*module, resolutions, diags, *debugInfo,
                                    optimizer.get());
  }

  /// Tear down everything referencing the current context, then move the
  /// module/context pair out. Order matters: the builder (inside IRGen),
  /// the instrumentation (inside Optimizer), and the DIBuilder all hold
  /// references into the context or module.
  llvm::orc::ThreadSafeModule take() {
    irgen.reset();
    optimizer.reset();
    debugInfo.reset();

    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(context));
    initializeModule();
    return tsm;
  }
};

//===----------------------------------------------------------------------===//
// Public facade
//===----------------------------------------------------------------------===//

CodeGenSession::CodeGenSession(CodeGenOptions options,
                               const Resolutions &resolutions,
                               DiagnosticEngine &diags)
    : impl(std::make_unique<Impl>(std::move(options), resolutions, diags)) {}

CodeGenSession::~CodeGenSession() = default;

llvm::Function *CodeGenSession::emitRecord(RecordAST &record) {
  return impl->irgen->emitRecord(record);
}

llvm::Module &CodeGenSession::currentModule() { return *impl->module; }

bool CodeGenSession::finalize() {
  impl->debugInfo->finalize();
  if (llvm::verifyModule(*impl->module, &llvm::errs())) {
    impl->diags.error({}, "internal error: module verification failed");
    return false;
  }
  return true;
}

llvm::orc::ThreadSafeModule CodeGenSession::takeModule() {
  return impl->take();
}

void CodeGenSession::setDataLayout(const llvm::DataLayout &layout) {
  impl->dataLayout = layout;
  impl->module->setDataLayout(layout);
}
