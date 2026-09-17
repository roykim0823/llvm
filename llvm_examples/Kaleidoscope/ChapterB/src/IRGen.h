//===- IRGen.h - LLVM IR generation from the Kaleidoscope AST (internal) --===//
//
// AST -> LLVM IR, consuming the name resolution computed by Sema instead of
// re-resolving anything: variable uses index straight into a vector of
// allocas by VarId (no symbol table, no scopes -- shadowing was already
// resolved), and call sites go straight to their bound PrototypeAST.
//
// The contract cuts both ways: IRGen REQUIRES a Resolutions that Sema
// produced with zero errors (missing bindings are programmer error and
// assert), and in exchange emission of an expression cannot fail -- the
// only failure left at all is an LLVM verifier rejection, which is an
// internal bug, not user input.
//
// The pass pipeline lives in Optimizer, DWARF in DebugInfoEmitter (both
// injected); module lifecycle in the CodeGenSession facade composing all
// three. One IRGen serves one module; the facade constructs a fresh one per
// module (JIT mode hands modules off). Internal to the backend (lives in
// src/, not include/toy/).
//
//===----------------------------------------------------------------------===//

#ifndef TOY_SRC_IRGEN_H
#define TOY_SRC_IRGEN_H

#include "toy/AST.h"
#include "toy/Diagnostics.h"
#include "toy/Sema.h"

#include "DebugInfo.h"

#include "llvm/IR/IRBuilder.h"

#include <vector>

namespace llvm {
class AllocaInst;
class Function;
class Module;
class Value;
} // namespace llvm

namespace toy {

class Optimizer;

class IRGen {
public:
  /// `optimizer` may be null (-opt off). `resolutions` must have been
  /// produced by a resolveModule() run that reported no errors, over the
  /// same ModuleAST whose records are emitted here; it is borrowed, as are
  /// the PrototypeASTs it points into (the ModuleAST owns them).
  IRGen(llvm::Module &module, const Resolutions &resolutions,
        DiagnosticEngine &diags, DebugInfoEmitter &debug,
        Optimizer *optimizer);

  /// Generate IR for one module-level record. Returns the llvm::Function,
  /// or nullptr only on an internal failure (verifier rejection), reported
  /// through the DiagnosticEngine.
  llvm::Function *emitRecord(RecordAST &record);

private:
  llvm::Module &module;
  llvm::LLVMContext &context;
  llvm::IRBuilder<> builder;
  const Resolutions &resolutions;
  DiagnosticEngine &diags;
  DebugInfoEmitter &debug;
  Optimizer *optimizer;

  /// Storage for every declared variable, indexed by VarId. A plain vector
  /// replaces ChapterA's ScopedHashTable: resolution already decided which
  /// declaration each use refers to, so there is nothing left to scope.
  std::vector<llvm::AllocaInst *> allocas;

  llvm::Type *doubleTy();
  llvm::Function *getFunction(PrototypeAST &proto);
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Function *fn,
                                           llvm::StringRef varName);
  void discardBrokenFunction(llvm::Function *fn);

  llvm::Value *emitExpr(ExprAST &expr);
  llvm::Value *emit(NumberExprAST &num);
  llvm::Value *emit(VariableExprAST &var);
  llvm::Value *emit(UnaryExprAST &unary);
  llvm::Value *emit(BinaryExprAST &bin);
  llvm::Value *emit(CallExprAST &call);
  llvm::Value *emit(IfExprAST &ifExpr);
  llvm::Value *emit(ForExprAST &forExpr);
  llvm::Value *emit(VarExprAST &varExpr);

  llvm::Function *emitPrototype(PrototypeAST &proto);
  llvm::Function *emitFunction(FunctionAST &funcAST);
};

} // namespace toy

#endif // TOY_SRC_IRGEN_H
