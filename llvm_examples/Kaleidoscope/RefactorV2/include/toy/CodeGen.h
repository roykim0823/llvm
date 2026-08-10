//===- CodeGen.h - LLVM IR generation for Kaleidoscope --------------------===//
//
// Facade for IR generation, in the spirit of the MLIR Toy tutorial's
// MLIRGen.h: consumers see a small session class; the implementation
// (dispatch over the AST, symbol table, pass pipeline, debug info) lives
// entirely in CodeGen.cpp behind a pImpl. This header deliberately includes
// no LLVM IR headers.
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

class RecordAST;

/// Options controlling IR generation.
struct CodeGenOptions {
  bool optimize = false;      ///< run the per-function pass pipeline
  bool emitDebugInfo = false; ///< attach DWARF debug info (single-module mode)
  std::string sourceFile = "<stdin>"; ///< filename for the debug compile unit
};

/// A code generation session owning the LLVMContext, Module, builder, pass
/// managers, and the cross-module function prototype registry.
///
/// Lifetime note: the session stores non-owning pointers to PrototypeAST
/// nodes, so the ModuleAST must outlive the session (and any JIT'd code
/// generated from it).
class CodeGenSession {
public:
  explicit CodeGenSession(CodeGenOptions options);
  ~CodeGenSession();

  /// Generate IR for one module-level record (function definition, anonymous
  /// top-level expression, or extern declaration) into the current module.
  /// Returns the generated llvm::Function, or nullptr after reporting an
  /// error with its source location.
  llvm::Function *emitRecord(RecordAST &record);

  /// Access the module being populated (e.g. to print or emit object code).
  llvm::Module &currentModule();

  /// Finalize the current module: complete debug info (if enabled) and run
  /// the LLVM verifier. Returns false if verification fails.
  bool finalize();

  /// For the JIT: hand off the current module and its context as a
  /// ThreadSafeModule and start a fresh module. Dependent objects (builder,
  /// pass managers, instrumentation) are torn down before the context moves,
  /// so nothing dangles.
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
