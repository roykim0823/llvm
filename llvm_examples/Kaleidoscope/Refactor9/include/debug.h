#ifndef DEBUG_H
#define DEBUG_H

#include "llvm/IR/DIBuilder.h"
#include <string>

namespace toy {

class PrototypeAST;
class ExprAST;

struct DebugInfo {
  llvm::DICompileUnit *TheCU;
  llvm::DIType *DblTy;
  std::vector<llvm::DIScope *> LexicalBlocks;

  void emitLocation(ExprAST *AST);
  llvm::DIType *getDoubleTy();
} KSDbgInfo;

struct SourceLocation {
  int Line;
  int Col;
};

class DebugInfoManager {
    public:
    int advance();
    
    // Debug location tracking
    //SourceLocation curLoc;
    SourceLocation lexLoc = {1, 0};
};

} // end namespace toy
#endif // DEBUG_H