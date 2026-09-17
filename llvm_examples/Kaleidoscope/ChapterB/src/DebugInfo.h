//===- DebugInfo.h - DWARF emission for the toy compiler (internal) -------===//
//
// The debug-info state pulled out of ChapterA's CodeGen Impl into a
// null-object: default-constructed it is disabled and every hook is a
// no-op, so IRGen calls the hooks unconditionally and contains no
// debug-info conditionals at all (ChapterA threaded `if (dbuilder)` through
// function emission).
//
// Internal to the backend (lives in src/, not include/toy/).
//
//===----------------------------------------------------------------------===//

#ifndef TOY_SRC_DEBUGINFO_H
#define TOY_SRC_DEBUGINFO_H

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"

#include <memory>

namespace llvm {
class AllocaInst;
class Argument;
class Function;
class Module;
} // namespace llvm

namespace toy {

class ExprAST;
class PrototypeAST;

class DebugInfoEmitter {
public:
  /// Disabled emitter: every hook is a no-op.
  DebugInfoEmitter() = default;

  /// Enabled emitter for one module: creates the DICompileUnit and the
  /// 'double' basic type, and stamps the module flags DWARF needs.
  DebugInfoEmitter(llvm::Module &module, llvm::StringRef sourceFile,
                   bool optimized);

  bool enabled() const { return dbuilder != nullptr; }

  /// Open a DISubprogram scope for `fn` and clear the builder's location so
  /// the argument spills that follow carry no line info.
  void functionBegin(const PrototypeAST &proto, llvm::Function &fn,
                     llvm::IRBuilder<> &builder);

  /// Attach parameter metadata + a dbg.declare to one argument's alloca.
  /// `index` is the 1-based DWARF argument position.
  void declareParameter(const PrototypeAST &proto, llvm::AllocaInst &alloca,
                        unsigned index, llvm::IRBuilder<> &builder);

  /// Point subsequently emitted instructions at `node`'s source location.
  void setLocation(const ExprAST *node, llvm::IRBuilder<> &builder);

  /// Close the current subprogram scope. On failure the subprogram is
  /// detached from `fn` (the broken body is being discarded).
  void functionEnd(llvm::Function &fn, bool succeeded);

  /// DIBuilder::finalize() -- must run before module verification.
  void finalize();

private:
  std::unique_ptr<llvm::DIBuilder> dbuilder;
  llvm::DICompileUnit *cu = nullptr;
  llvm::DIType *doubleTy = nullptr;
  llvm::DISubprogram *currentSubprogram = nullptr;
};

} // namespace toy

#endif // TOY_SRC_DEBUGINFO_H
