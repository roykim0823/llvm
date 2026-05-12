/// \file
/// \brief Top-level entry point for tinylang's LLVM IR code generation
/// (added in Chapter 4).
///
/// \ref tinylang::CodeGenerator wraps the configured \ref llvm::LLVMContext and
/// \ref llvm::TargetMachine, then delegates the actual lowering of a
/// \ref tinylang::ModuleDeclaration "ModuleDeclaration" to
/// \ref tinylang::CGModule "CGModule". The driver calls \ref run once per
/// successfully parsed translation unit.

#ifndef TINYLANG_CODEGEN_CODEGENERATOR_H
#define TINYLANG_CODEGEN_CODEGENERATOR_H

#include "tinylang/AST/AST.h"
#include "llvm/Target/TargetMachine.h"
#include <string>

namespace tinylang {

/// Façade that turns a tinylang AST into an \ref llvm::Module.
///
/// Owns no state beyond a context/target pair; the heavy lifting happens in
/// \ref tinylang::CGModule "CGModule" and \ref tinylang::CGProcedure
/// "CGProcedure". Use \ref create as the public factory and call \ref run
/// per compilation unit.
class CodeGenerator {
  llvm::LLVMContext &Ctx;     ///< Context shared with the driver.
  llvm::TargetMachine *TM;    ///< Target machine used for data layout + triple.

protected:
  CodeGenerator(llvm::LLVMContext &Ctx,
                llvm::TargetMachine *TM)
      : Ctx(Ctx), TM(TM) {}

public:
  /// Factory. Returns a heap-allocated \ref CodeGenerator the caller owns.
  static CodeGenerator *create(llvm::LLVMContext &Ctx,
                               llvm::TargetMachine *TM);

  /// Lowers \p CM into a fresh \ref llvm::Module named after \p FileName.
  /// The module's target triple and data layout are taken from \ref TM.
  std::unique_ptr<llvm::Module> run(ModuleDeclaration *CM,
                                    std::string FileName);
};
} // namespace tinylang
#endif
