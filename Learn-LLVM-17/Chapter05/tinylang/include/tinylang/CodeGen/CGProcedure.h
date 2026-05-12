/// \file
/// \brief Procedure-level code generation.
///
/// \note Ch05 deltas vs Ch04:
///   - \ref readVariable takes a `LoadVal` flag — pass `false` to obtain
///     the address (used as a GEP base on the LHS of an assignment),
///   - \ref mapType no longer takes a `HonorReference` flag; VAR formal
///     parameters are wrapped to `ptr` via `PointerType::getUnqual`,
///   - new \ref createBasicBlock helper replaces every
///     `BasicBlock::Create(Ctx, Name, Fn)` call,
///   - \ref DIVariables is a side table reserved for debug info,
///   - \ref emitExpr / \ref emitStmt now consume the new
///     \ref tinylang::Designator selector chain (`a[i]`, `r.f`, `p^`) by
///     emitting GEPs and loads/stores.

#ifndef TINYLANG_CODEGEN_CGPROCEDURE_H
#define TINYLANG_CODEGEN_CGPROCEDURE_H

#include "tinylang/AST/AST.h"
#include "tinylang/CodeGen/CGModule.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

namespace llvm {
class Function;
}

namespace tinylang {

class CGProcedure {
  CGModule &CGM;
  llvm::IRBuilder<> Builder;

  llvm::BasicBlock *Curr;

  ProcedureDeclaration *Proc;
  llvm::FunctionType *Fty;
  llvm::Function *Fn;

  struct BasicBlockDef {
    // Maps the variable (or formal parameter) to its
    // definition.
    llvm::DenseMap<Decl *, llvm::TrackingVH<llvm::Value>>
        Defs;
    // Set of incompleted phi instructions.
    llvm::DenseMap<llvm::PHINode *, Decl *> IncompletePhis;
    // Block is sealed, that is, no more predecessors will
    // be added.
    unsigned Sealed : 1;

    BasicBlockDef() : Sealed(0) {}
  };

  llvm::DenseMap<llvm::BasicBlock *, BasicBlockDef>
      CurrentDef;

  void writeLocalVariable(llvm::BasicBlock *BB, Decl *Decl,
                          llvm::Value *Val);
  llvm::Value *readLocalVariable(llvm::BasicBlock *BB,
                                 Decl *Decl);
  llvm::Value *
  readLocalVariableRecursive(llvm::BasicBlock *BB,
                             Decl *Decl);
  llvm::PHINode *addEmptyPhi(llvm::BasicBlock *BB,
                             Decl *Decl);
  llvm::Value *addPhiOperands(llvm::BasicBlock *BB, Decl *Decl,
                      llvm::PHINode *Phi);
  llvm::Value *optimizePhi(llvm::PHINode *Phi);
  void sealBlock(llvm::BasicBlock *BB);

  llvm::DenseMap<FormalParameterDeclaration *,
                 llvm::Argument *>
      FormalParams;
  /// Side table for debug-info local variables (added in Ch05; filled
  /// only when `-g` is active).
  llvm::DenseMap<Decl *, llvm::DILocalVariable *>
      DIVariables;

  void writeVariable(llvm::BasicBlock *BB, Decl *Decl,
                     llvm::Value *Val);
  /// Reads \p Decl in \p BB. With `LoadVal = false` returns the address
  /// (used as a GEP base when generating LHS code for assignments to
  /// compound designators). (Ch05)
  llvm::Value *readVariable(llvm::BasicBlock *BB,
                            Decl *Decl, bool LoadVal = true);

  llvm::Type *mapType(Decl *Decl);
  llvm::FunctionType *
  createFunctionType(ProcedureDeclaration *Proc);
  llvm::Function *createFunction(ProcedureDeclaration *Proc,
                                 llvm::FunctionType *FTy);

protected:
  void setCurr(llvm::BasicBlock *BB) {
    Curr = BB;
    Builder.SetInsertPoint(Curr);
  }

  /// Convenience wrapper around \ref llvm::BasicBlock::Create
  /// pinned to the current function. (Ch05)
  llvm::BasicBlock *createBasicBlock(
      const llvm::Twine &Name,
      llvm::BasicBlock *InsertBefore = nullptr) {
    return llvm::BasicBlock::Create(CGM.getLLVMCtx(), Name,
                                    Fn, InsertBefore);
  }

  llvm::Value *emitInfixExpr(InfixExpression *E);
  llvm::Value *emitPrefixExpr(PrefixExpression *E);
  llvm::Value *emitExpr(Expr *E);

  void emitStmt(AssignmentStatement *Stmt);
  void emitStmt(ProcedureCallStatement *Stmt);
  void emitStmt(IfStatement *Stmt);
  void emitStmt(WhileStatement *Stmt);
  void emitStmt(ReturnStatement *Stmt);
  void emit(const StmtList &Stmts);

public:
  CGProcedure(CGModule &CGM)
      : CGM(CGM), Builder(CGM.getLLVMCtx()),
        Curr(nullptr){};

  void run(ProcedureDeclaration *Proc);
  /// Placeholder no-arg overload added in Ch05 (reserved for the upcoming
  /// debug-info / late-finalisation pass; currently a no-op).
  void run();
};
} // namespace tinylang
#endif