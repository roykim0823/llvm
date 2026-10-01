#ifndef CODEGEN_H
#define CODEGEN_H

//===----------------------------------------------------------------------===//
// Code generation facade
//===----------------------------------------------------------------------===//
// Consumers (the driver, the tests) see a small session class. Everything
// LLVM-specific -- the LLVMContext, the Module, the IRBuilder, the symbol
// table, the pass pipeline, the prototype registry, and the walk over the
// AST -- lives in codegen.cpp behind a pImpl. This header deliberately
// includes no LLVM IR headers: it only forward declares the LLVM types that
// cross the boundary.
//
// Chapter 4 adds the two methods a JIT-driven REPL needs: takeModule() hands
// the finished module off (the JIT freezes what it is given, so the session
// immediately opens a fresh one), and setDataLayout() stamps the JIT's data
// layout onto every module the session opens.
//
// Chapter 9 adds CodeGenOptions -- whether to run the optimization pipeline
// and whether to attach debug info, and under which source file name -- and
// finalize(), which completes the debug metadata before the module is used.

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

class PrototypeAST;
class FunctionAST;

/// Options controlling IR generation (Chapter 9).
struct CodeGenOptions {
  bool optimize = true;         ///< run the per-function pass pipeline (Chapter 4)
  bool emitDebugInfo = false;   ///< attach DWARF debug info to everything emitted
  std::string sourceFile = "<stdin>";  ///< file name recorded in the debug compile unit
};

/// A code generation session owning the current LLVMContext + Module, the
/// IRBuilder that emits into it, the per-function optimization pipeline, the
/// registry of every function prototype seen so far (so calls can be
/// re-declared into later modules), and -- with emitDebugInfo -- the DIBuilder
/// and debug scopes that mirror the code.
class CodeGenSession {
public:
  explicit CodeGenSession(CodeGenOptions options = {});
  ~CodeGenSession();

  /// Emit a function declaration (`extern`) into the current module and
  /// register the prototype for cross-module calls.
  llvm::Function *emitPrototype(PrototypeAST &proto);

  /// Emit a function definition (`def ...` or an anonymous top-level
  /// expression) into the current module: registers its prototype, builds
  /// and verifies the body, then runs the optimization pipeline on it.
  /// Returns the llvm::Function, or nullptr after reporting an error -- in
  /// which case nothing half-built is left in the module.
  llvm::Function *emitFunction(FunctionAST &fn);

  /// The module being populated (e.g. to print it or look functions up).
  llvm::Module &currentModule();

  /// Remove a function this session emitted -- the driver uses it to discard a
  /// top-level expression once it has been printed. The module is the
  /// session's, so removals go through the session too: it also drops any
  /// state it keeps about the function (from Chapter 4 on, the analyses the
  /// pass managers cached for it, which would otherwise dangle).
  void eraseFunction(llvm::Function *fn);

  /// For the JIT: move the current module and its context out as a
  /// ThreadSafeModule and open a fresh module. Everything that referenced
  /// the old context (builder, pass managers, instrumentation) is torn down
  /// first, so nothing dangles.
  llvm::orc::ThreadSafeModule takeModule();

  /// Set the data layout applied to the current and every future module
  /// (from the JIT's target machine).
  void setDataLayout(const llvm::DataLayout &layout);

  /// Complete the current module's debug metadata (a no-op without
  /// emitDebugInfo). Call before printing or emitting the module.
  void finalize();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // end namespace toy

#endif // CODEGEN_H
