//===- CodeGen.h - LLVM IR generation for Kaleidoscope --------------------===//
//
// Facade for the backend. Same five-method contract as ChapterA, plus a
// DiagnosticEngine: consumers see a small session class; behind the pImpl
// the work is now split across three components (IRGen for AST-to-IR,
// Optimizer for the pass pipeline, DebugInfoEmitter for DWARF) that the
// session composes and whose lifetimes it manages. This header deliberately
// includes no LLVM IR headers.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_CODEGEN_H
#define TOY_CODEGEN_H

#include <memory>
#include <string>

namespace llvm {
class DataLayout;
class Function;
class Module;
namespace orc {
class ThreadSafeModule;
} // namespace orc
} // namespace llvm

namespace toy {

class DiagnosticEngine;
class RecordAST;
class Resolutions;

/// Options controlling IR generation.
struct CodeGenOptions {
  bool optimize = false;      ///< run the per-function pass pipeline
  bool emitDebugInfo = false; ///< attach DWARF debug info (single-module mode)
  std::string sourceFile = "<stdin>"; ///< filename for the debug compile unit
};

/// A code generation session owning the LLVMContext, Module, and the
/// backend components (IRGen, Optimizer, DebugInfoEmitter).
///
/// The session consumes the name resolution computed by Sema: pass the
/// Resolutions from a resolveModule() run that reported no errors. Emission
/// then cannot fail on user input; the only reportable failure left is an
/// internal one (an LLVM verifier rejection).
///
/// Lifetime notes: the Resolutions table (and the ModuleAST it points into)
/// and the DiagnosticEngine must outlive the session -- and the ModuleAST
/// must outlive any JIT'd code generated from it.
class CodeGenSession {
public:
  CodeGenSession(CodeGenOptions options, const Resolutions &resolutions,
                 DiagnosticEngine &diags);
  ~CodeGenSession();

  /// Generate IR for one module-level record (function definition, anonymous
  /// top-level expression, or extern declaration) into the current module.
  /// Returns the generated llvm::Function, or nullptr after reporting an
  /// internal error through the DiagnosticEngine.
  llvm::Function *emitRecord(RecordAST &record);

  /// Access the module being populated (e.g. to print or emit object code).
  llvm::Module &currentModule();

  /// Finalize the current module: complete debug info (if enabled) and run
  /// the LLVM verifier. Returns false if verification fails.
  bool finalize();

  /// For the JIT: hand off the current module and its context as a
  /// ThreadSafeModule and start a fresh module. The components referencing
  /// the context (IRGen's builder, the optimizer's instrumentation, the
  /// DIBuilder) are torn down before the context moves, so nothing dangles.
  llvm::orc::ThreadSafeModule takeModule();

  /// Set the data layout applied to the current and every future module
  /// (from the JIT or a TargetMachine).
  void setDataLayout(const llvm::DataLayout &layout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace toy

#endif // TOY_CODEGEN_H
