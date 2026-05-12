/// \file
/// \brief Module-level code generation (added in Chapter 4).
///
/// \ref tinylang::CGModule holds the per-module state shared by every
/// \ref tinylang::CGProcedure "CGProcedure": the cached LLVM primitive types,
/// the mapping from tinylang \ref tinylang::Decl "Decl"s to their
/// \ref llvm::GlobalObject, and the name-mangling routine.

#ifndef TINYLANG_CODEGEN_CGMODULE_H
#define TINYLANG_CODEGEN_CGMODULE_H

#include "tinylang/AST/AST.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace tinylang {

/// Per-\ref llvm::Module state for tinylang code generation.
///
/// Created once per compilation unit by \ref tinylang::CodeGenerator
/// "CodeGenerator". Walks the top-level decls of a
/// \ref tinylang::ModuleDeclaration "ModuleDeclaration": each
/// \ref tinylang::VariableDeclaration "VariableDeclaration" becomes a
/// `GlobalVariable`, each \ref tinylang::ProcedureDeclaration
/// "ProcedureDeclaration" is delegated to a fresh
/// \ref tinylang::CGProcedure "CGProcedure".
class CGModule {
  llvm::Module *M;          ///< The LLVM module being filled in.

  ModuleDeclaration *Mod;   ///< The tinylang module currently being lowered.

  /// Map from tinylang declarations to their materialised LLVM global object
  /// (a \ref llvm::GlobalVariable or \ref llvm::Function). Populated by
  /// \ref run and consulted by \ref CGProcedure when emitting loads/stores.
  llvm::DenseMap<Decl *, llvm::GlobalObject *> Globals;

public:
  /// @name Cached LLVM primitive types (filled in by \ref initialize).
  /// @{
  llvm::Type *VoidTy;        ///< `void`.
  llvm::Type *Int1Ty;        ///< `i1`, used for `BOOLEAN`.
  llvm::Type *Int32Ty;       ///< `i32`.
  llvm::Type *Int64Ty;       ///< `i64`, used for `INTEGER`.
  llvm::Constant *Int32Zero; ///< Constant `i32 0` (handy for GEP indices).
  /// @}

public:
  CGModule(llvm::Module *M) : M(M) { initialize(); }
  /// Pre-computes the type/constant cache above.
  void initialize();

  llvm::LLVMContext &getLLVMCtx() { return M->getContext(); }
  llvm::Module *getModule() { return M; }
  ModuleDeclaration *getModuleDeclaration() { return Mod; }

  /// Maps a tinylang \ref TypeDeclaration to its LLVM counterpart.
  /// Currently only `INTEGER` (`i64`) and `BOOLEAN` (`i1`) are supported.
  llvm::Type *convertType(TypeDeclaration *Ty);
  /// Builds an unambiguous symbol name from a \ref Decl chain.
  /// Format: `_t` followed by `<len><name>` for each enclosing decl, outer
  /// first (e.g. `Gcd.GCD` → `_t3Gcd3GCD`).
  std::string mangleName(Decl *D);

  /// Returns the LLVM global previously created for \p D, or `nullptr`.
  llvm::GlobalObject *getGlobal(Decl *);

  /// Lowers \p Mod's top-level decls. Variables become globals, procedures
  /// are lowered via \ref CGProcedure.
  void run(ModuleDeclaration *Mod);
};
} // namespace tinylang
#endif