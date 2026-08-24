#ifndef DEBUG_H
#define DEBUG_H

#include "llvm/IR/DIBuilder.h"
#include <string>

#include "ir_gen_ctx.h"

namespace toy {

class PrototypeAST;
class ExprAST;

struct DebugInfo {
  llvm::DICompileUnit *TheCU;
  llvm::DIType *DblTy;
  std::vector<llvm::DIScope *> LexicalBlocks;

  void emitLocation(ExprAST *AST, IRGenContext &ctx);
  llvm::DIType *getDoubleTy();
};

struct SourceLocation {
  int Line;
  int Col;
};

// Location of the token the parser is currently looking at; defined in
// lexer.cpp, defaulted into every ExprAST at construction (as upstream does).
extern SourceLocation CurLoc;

class DebugInfoManager {
    public:
    DebugInfoManager(IRGenContext &ctx) : ctx(ctx) {}
    int advance();

    // Debug location tracking
    //SourceLocation curLoc;  // TODO: move the global varble into the this member variable
    SourceLocation lexLoc = {1, 0};
    private:
    IRGenContext &ctx;
};

llvm::DISubroutineType *CreateFunctionType(unsigned NumArgs);
} // end namespace toy
#endif // DEBUG_H