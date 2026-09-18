#ifndef CODEGEN_H
#define CODEGEN_H

//===----------------------------------------------------------------------===//
// Code generation facade
//===----------------------------------------------------------------------===//
// Consumers (the driver, the tests) see a small session class. Everything
// LLVM-specific -- the LLVMContext, the Module, the IRBuilder, the symbol
// table, and the walk over the AST -- lives in codegen.cpp behind a pImpl.
// This header deliberately includes no LLVM IR headers: it only forward
// declares the two LLVM types that cross the boundary.

#include <memory>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace toy {

class PrototypeAST;
class FunctionAST;

/// A code generation session owning one LLVMContext, one Module and the
/// IRBuilder that emits into it.
class CodeGenSession {
public:
  CodeGenSession();
  ~CodeGenSession();

  /// Emit a function declaration (`extern`). Returns the llvm::Function.
  llvm::Function *emitPrototype(PrototypeAST &proto);

  /// Emit a function definition (`def ...` or an anonymous top-level
  /// expression). Returns the llvm::Function, or nullptr after reporting an
  /// error -- in which case nothing half-built is left in the module.
  llvm::Function *emitFunction(FunctionAST &fn);

  /// The module being populated (e.g. to print it or look functions up).
  llvm::Module &currentModule();

  /// Remove a function this session emitted -- the driver uses it to discard a
  /// top-level expression once it has been printed. The module is the
  /// session's, so removals go through the session too: it also drops any
  /// state it keeps about the function (from Chapter 4 on, the analyses the
  /// pass managers cached for it, which would otherwise dangle).
  void eraseFunction(llvm::Function *fn);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // end namespace toy

#endif // CODEGEN_H
