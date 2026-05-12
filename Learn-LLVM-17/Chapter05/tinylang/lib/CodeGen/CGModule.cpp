/// \file
/// \brief Implementation of \ref tinylang::CGModule.
///
/// Ch05 deltas vs Ch04:
///   - constructor now takes an \ref tinylang::ASTContext;
///   - \ref convertType dispatches on the new \ref TypeDeclaration
///     subclasses and memoises results in `TypeCache`;
///   - a global `-g` command-line flag is registered (debug-info hook;
///     consumed by upcoming chapters).

#include "tinylang/CodeGen/CGModule.h"
#include "tinylang/CodeGen/CGProcedure.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"

using namespace tinylang;

/// Ch05: registers the `-g` flag. Currently parsed but otherwise unused —
/// the debug-info pipeline is wired up in a later chapter.
static llvm::cl::opt<bool>
    Debug("g", llvm::cl::desc("Generate debug information"),
          llvm::cl::init(false));

CGModule::CGModule(ASTContext &ASTCtx, llvm::Module *M)
    : ASTCtx(ASTCtx), M(M) {
  initialize();
}

void CGModule::initialize() {
  VoidTy = llvm::Type::getVoidTy(getLLVMCtx());
  Int1Ty = llvm::Type::getInt1Ty(getLLVMCtx());
  Int32Ty = llvm::Type::getInt32Ty(getLLVMCtx());
  Int64Ty = llvm::Type::getInt64Ty(getLLVMCtx());
  Int32Zero =
      llvm::ConstantInt::get(Int32Ty, 0, /*isSigned*/ true);
}

/// Maps a tinylang \ref TypeDeclaration to its LLVM counterpart.
///
/// Ch05 expansion vs Ch04: dispatches on the new
/// `Pervasive/Alias/Array/Pointer/Record` hierarchy and caches results
/// in \ref TypeCache so a record/array shared across decls produces
/// exactly one \ref llvm::Type.
///   - `PervasiveTypeDeclaration` → `i64` / `i1`,
///   - `AliasTypeDeclaration`     → recurse on the aliased type,
///   - `ArrayTypeDeclaration`     → `[N x ElemTy]` (N must be a const int),
///   - `RecordTypeDeclaration`    → named `StructType` of field types.
/// `PointerTypeDeclaration` falls through to the fatal-error tail and is
/// expected to be handled at lowering sites that build opaque pointers.
llvm::Type *CGModule::convertType(TypeDeclaration *Ty) {
  if (llvm::Type *T = TypeCache[Ty])
    return T;

  if (llvm::isa<PervasiveTypeDeclaration>(Ty)) {
    if (Ty->getName() == "INTEGER")
      return Int64Ty;
    if (Ty->getName() == "BOOLEAN")
      return Int1Ty;
  } else if (auto *AliasTy =
                 llvm::dyn_cast<AliasTypeDeclaration>(Ty)) {
    llvm::Type *T = convertType(AliasTy->getType());
    return TypeCache[Ty] = T;
  } else if (auto *ArrayTy =
                 llvm::dyn_cast<ArrayTypeDeclaration>(Ty)) {
    llvm::Type *Component =
        convertType(ArrayTy->getType());
    // The semantic analysis makes sure that the Nums
    // expression is a constant expression of type
    // INTEGER. To simplify the coding, we expect an
    // IntegerLiteral here.

    Expr *Nums = ArrayTy->getNums();
    assert(llvm::cast<IntegerLiteral>(Nums) &&
           "Expected an integer literal");

    uint64_t NumElements =
        llvm::cast<IntegerLiteral>(Nums)
            ->getValue()
            .getZExtValue();
    llvm::Type *T =
        llvm::ArrayType::get(Component, NumElements);
    // TypeCache is a mapping between the original
    // TypeDeclaration (Ty) and the current Type(T)
    return TypeCache[Ty] = T;
  } else if (auto *RecordTy =
                 llvm ::dyn_cast<RecordTypeDeclaration>(
                     Ty)) {
    llvm::SmallVector<llvm::Type *, 4> Elements;
    for (const auto &F : RecordTy->getFields()) {
      Elements.push_back(convertType(F.getType()));
    }
    llvm::Type *T = llvm::StructType::create(
        Elements, RecordTy->getName(), false);
    return TypeCache[Ty] = T;
  }
  llvm::report_fatal_error("Unsupported type");
}

std::string CGModule::mangleName(Decl *D) {
  std::string Mangled;
  llvm::SmallString<16> Tmp;
  while (D) {
    llvm::StringRef Name = D->getName();
    Tmp.clear();
    Tmp.append(llvm::itostr(Name.size()));
    Tmp.append(Name);
    Mangled.insert(0, Tmp.c_str());
    D = D->getEnclosingDecl();
  }
  Mangled.insert(0, "_t");
  return Mangled;
}

llvm::GlobalObject *CGModule::getGlobal(Decl *D) {
  return Globals[D];
}

void CGModule::run(ModuleDeclaration *Mod) {
  this->Mod = Mod;
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
