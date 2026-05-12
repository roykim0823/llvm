/// \file
/// \brief Module-level code generation.
///
/// \note Ch05 deltas vs Ch04:
///   - constructor takes an \ref tinylang::ASTContext (Ch04 took only the
///     LLVM \ref llvm::Module),
///   - \ref tinylang::CGModule::convertType "convertType" now dispatches via
///     LLVM-style RTTI on the new \ref tinylang::TypeDeclaration hierarchy
///     (`Pervasive`/`Alias`/`Array`/`Pointer`/`Record`),
///   - a \ref TypeCache memoises the LLVM type produced for each
///     compound \ref tinylang::TypeDeclaration so a record/array shared
///     across decls maps to one \ref llvm::Type.

#ifndef TINYLANG_CODEGEN_CGMODULE_H
#define TINYLANG_CODEGEN_CGMODULE_H

#include "tinylang/AST/AST.h"
#include "tinylang/AST/ASTContext.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace tinylang {

class CGModule {
  ASTContext &ASTCtx;
  llvm::Module *M;

  ModuleDeclaration *Mod;

  /// Memoised mapping from tinylang compound types (array/record/alias) to
  /// their materialised LLVM type. Added in Ch05.
  llvm::DenseMap<TypeDeclaration *, llvm::Type *> TypeCache;

  // Repository of global objects.
  llvm::DenseMap<Decl *, llvm::GlobalObject *> Globals;

public:
  llvm::Type *VoidTy;
  llvm::Type *Int1Ty;
  llvm::Type *Int32Ty;
  llvm::Type *Int64Ty;
  llvm::Constant *Int32Zero;

public:
  CGModule(ASTContext &ASTCtx, llvm::Module *M);
  void initialize();

  ASTContext &getASTCtx() { return ASTCtx; }
  llvm::LLVMContext &getLLVMCtx() {
    return M->getContext();
  }
  llvm::Module *getModule() { return M; }
  ModuleDeclaration *getModuleDeclaration() { return Mod; }

  llvm::Type *convertType(TypeDeclaration *Ty);
  std::string mangleName(Decl *D);

  llvm::GlobalObject *getGlobal(Decl *);

  void run(ModuleDeclaration *Mod);
};
} // namespace tinylang
#endif
