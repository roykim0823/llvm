
#include "debug.h"
#include "ast.h"

using namespace toy;

std::unique_ptr<llvm::DIBuilder> DBuilder;
DebugInfo KSDbgInfo;

int DebugInfoManager::advance() {
  int LastChar = getchar();

  if (LastChar == '\n' || LastChar == '\r') {
    lexLoc.Line++;
    lexLoc.Col = 0;
  } else
    lexLoc.Col++;

  return LastChar;
}

llvm::DIType *DebugInfo::getDoubleTy() {
  if (DblTy)
    return DblTy;

  DblTy = DBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
  return DblTy;
}

void DebugInfo::emitLocation(ExprAST *AST, CodegenContext &ctx) {
  if (!AST)
    return ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
  llvm::DIScope *Scope;
  if (LexicalBlocks.empty())
    Scope = TheCU;
  else
    Scope = LexicalBlocks.back();
  ctx.builder->SetCurrentDebugLocation(llvm::DILocation::get(
      Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}

namespace toy{
llvm::DISubroutineType *CreateFunctionType(unsigned NumArgs) {
  llvm::SmallVector<llvm::Metadata *, 8> EltTys;
  llvm::DIType *DblTy = KSDbgInfo.getDoubleTy();

  // Add the result type.
  EltTys.push_back(DblTy);

  for (unsigned i = 0, e = NumArgs; i != e; ++i)
    EltTys.push_back(DblTy);

  return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}
}
