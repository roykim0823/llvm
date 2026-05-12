/// \file
/// \brief Compilation-unit-wide AST context (added in Chapter 5).
///
/// \ref tinylang::ASTContext bundles the per-compilation-unit information
/// the AST and code generator need outside of individual decls: the owning
/// \ref llvm::SourceMgr and the original input filename. It is created in
/// the driver, passed to \ref tinylang::CodeGenerator, and stored on
/// \ref tinylang::CGModule "CGModule" so debug-info emission (and future
/// AST-allocation needs) can reach the source manager.

#ifndef TINYLANG_AST_ASTCONTEXT_H
#define TINYLANG_AST_ASTCONTEXT_H

#include "tinylang/Basic/LLVM.h"
#include "llvm/Support/SourceMgr.h"

namespace tinylang {

/// Per-compilation-unit context handed from the driver into code generation.
class ASTContext {
  llvm::SourceMgr &SrcMgr;  ///< Source manager owning the compiled buffer.
  StringRef Filename;       ///< Original filename (used for module name / DI).

public:
  ASTContext(llvm::SourceMgr &SrcMgr, StringRef Filename)
      : SrcMgr(SrcMgr), Filename(Filename) {}

  StringRef getFilename() { return Filename; }

  llvm::SourceMgr &getSourceMgr() { return SrcMgr; }
  const llvm::SourceMgr &getSourceMgr() const {
    return SrcMgr;
  }
};

} // namespace tinylang

#endif