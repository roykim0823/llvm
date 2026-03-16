#ifndef DEBUG_H
#define DEBUG_H

#include "llvm/IR/DIBuilder.h"
#include <string>

#include "codegen_ctx.h"

namespace toy {

class PrototypeAST;
class ExprAST;

struct DebugInfo {
  llvm::DICompileUnit *TheCU;
  llvm::DIType *DblTy;
  std::vector<llvm::DIScope *> LexicalBlocks;

  void emitLocation(ExprAST *AST, CodegenContext &ctx);
  llvm::DIType *getDoubleTy();
};// KSDbgInfo;

struct SourceLocation {
  int Line;
  int Col;
};

class DebugInfoManager {
    public:
    DebugInfoManager(CodegenContext &ctx) : ctx(ctx) {}
    int advance();
    
    // Debug location tracking
    //SourceLocation curLoc;
    SourceLocation lexLoc = {1, 0};
    private:
    CodegenContext &ctx;
};

llvm::DISubroutineType *CreateFunctionType(unsigned NumArgs);
} // end namespace toy
#endif // DEBUG_H