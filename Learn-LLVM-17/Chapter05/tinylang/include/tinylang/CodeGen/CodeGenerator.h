/// \file
/// \brief Top-level entry point for tinylang's LLVM IR code generation.
///
/// \note Ch05 threads an \ref tinylang::ASTContext through the factory and
/// the constructor (so debug info and other source-aware features can reach
/// the source manager); the rest of the façade is unchanged from Ch04.

#ifndef TINYLANG_CODEGEN_CODEGENERATOR_H
#define TINYLANG_CODEGEN_CODEGENERATOR_H

#include "tinylang/AST/AST.h"
#include "tinylang/AST/ASTContext.h"
#include "llvm/Target/TargetMachine.h"
#include <string>

namespace tinylang {

class CodeGenerator {
  llvm::LLVMContext &Ctx;
  ASTContext &ASTCtx;          ///< Added in Ch05 — passed on to CGModule.
  llvm::TargetMachine *TM;

protected:
  CodeGenerator(llvm::LLVMContext &Ctx, ASTContext &ASTCtx, llvm::TargetMachine *TM)
      : Ctx(Ctx), ASTCtx(ASTCtx), TM(TM) {}

public:
  static CodeGenerator *create(llvm::LLVMContext &Ctx, ASTContext &ASTCtx, llvm::TargetMachine *TM);

  std::unique_ptr<llvm::Module> run(ModuleDeclaration *CM, std::string FileName);
};
} // namespace tinylang
#endif
