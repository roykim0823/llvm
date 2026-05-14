/// \file
/// \brief Implementation of \ref tinylang::CGModule (Chapter 4).

#include "tinylang/CodeGen/CGModule.h"
#include "tinylang/CodeGen/CGProcedure.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"

using namespace tinylang;

void CGModule::initialize() {
  VoidTy = llvm::Type::getVoidTy(getLLVMCtx());
  Int1Ty = llvm::Type::getInt1Ty(getLLVMCtx());
  Int32Ty = llvm::Type::getInt32Ty(getLLVMCtx());
  Int64Ty = llvm::Type::getInt64Ty(getLLVMCtx());
  Int32Zero =
      llvm::ConstantInt::get(Int32Ty, 0, /*isSigned*/ true);
}

llvm::Type *CGModule::convertType(TypeDeclaration *Ty) {
  if (Ty->getName() == "INTEGER")
    return Int64Ty;
  if (Ty->getName() == "BOOLEAN")
    return Int1Ty;
  llvm::report_fatal_error("Unsupported type");
}

// Name mangling: an external linker needs unique, language-neutral symbol
// names. Tinylang uses a very simple scheme — `_t` prefix followed by
// `<len><name>` for each enclosing decl, outer-first. Example:
//   procedure GCD inside module Gcd  ->  _t3Gcd3GCD
// The length prefix makes the encoding parseable without delimiters.
std::string CGModule::mangleName(Decl *D) {
  std::string Mangled("_t");
  llvm::SmallVector<llvm::StringRef, 4> Parts;
  // Walk *inward* (D, then its parent, ...) collecting names.
  for (; D; D = D->getEnclosingDecl())
    Parts.push_back(D->getName());
  // Emit *outward* (module first, leaf last) so the symbol reads top-down.
  while (!Parts.empty()) {
    llvm::StringRef Name = Parts.pop_back_val();
    Mangled.append(
        llvm::Twine(Name.size()).concat(Name).str());
  }
  return Mangled;
}

llvm::GlobalObject *CGModule::getGlobal(Decl *D) {
  return Globals[D];
}

void CGModule::run(ModuleDeclaration *Mod) {
  for (auto *Decl : Mod->getDecls()) {
    if (auto *Var =
            llvm::dyn_cast<VariableDeclaration>(Decl)) {
      // Create global variables
      llvm::GlobalVariable *V = new llvm::GlobalVariable(
          *M, convertType(Var->getType()),
          /*isConstant=*/false,
          llvm::GlobalValue::PrivateLinkage, nullptr,
          mangleName(Var));
      Globals[Var] = V;
    } else if (auto *Proc =
                   llvm::dyn_cast<ProcedureDeclaration>(
                       Decl)) {
      CGProcedure CGP(*this);
      CGP.run(Proc);
    }
  }
}
