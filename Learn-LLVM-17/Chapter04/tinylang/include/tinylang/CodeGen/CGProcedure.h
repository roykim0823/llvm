/// \file
/// \brief Procedure-level code generation (added in Chapter 4).
///
/// \ref tinylang::CGProcedure lowers a single
/// \ref tinylang::ProcedureDeclaration "ProcedureDeclaration" into an
/// \ref llvm::Function. SSA form is built on the fly using the Braun et al.
/// algorithm — see \ref readLocalVariableRecursive and \ref sealBlock — so
/// no explicit "promote allocas to registers" pass is required.

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

/// Lowers one tinylang procedure to LLVM IR.
///
/// Holds an \ref llvm::IRBuilder positioned at the current insertion point
/// and the per-block variable-definition tables that drive on-the-fly
/// SSA construction.
class CGProcedure {
  CGModule &CGM;                ///< Owning module-level state.
  llvm::IRBuilder<> Builder;    ///< Insertion-point cursor.

  llvm::BasicBlock *Curr;       ///< Current basic block (also Builder's IP).

  ProcedureDeclaration *Proc;   ///< AST procedure being lowered.
  llvm::FunctionType *Fty;      ///< Function type derived from \ref Proc.
  llvm::Function *Fn;           ///< Current LLVM function.

  /// Per-block state used to build SSA without an alloca/mem2reg detour.
  struct BasicBlockDef {
    /// Current definition for each tinylang local variable or value parameter.
    llvm::DenseMap<Decl *, llvm::TrackingVH<llvm::Value>> Defs;
    /// Phi nodes inserted before all predecessors of this block were known;
    /// resolved by \ref sealBlock.
    llvm::DenseMap<llvm::PHINode *, Decl *> IncompletePhis;
    /// True once every predecessor edge of this block has been emitted.
    /// No further \ref IncompletePhis are added after sealing.
    unsigned Sealed : 1;

    BasicBlockDef() : Sealed(0) {}
  };

  /// Definition tables keyed by basic block.
  llvm::DenseMap<llvm::BasicBlock *, BasicBlockDef> CurrentDef;

  /// Records that \p Val is the latest definition of \p Decl in \p BB.
  void writeLocalVariable(llvm::BasicBlock *BB, Decl *Decl,
                          llvm::Value *Val);
  /// Looks up \p Decl's current value in \p BB, recursing into predecessors
  /// (and possibly creating phi nodes) when needed.
  llvm::Value *readLocalVariable(llvm::BasicBlock *BB,
                                 Decl *Decl);
  /// Slow path of \ref readLocalVariable: handles unsealed blocks and
  /// merges definitions across multiple predecessors via phi nodes.
  llvm::Value *
  readLocalVariableRecursive(llvm::BasicBlock *BB,
                             Decl *Decl);
  /// Inserts an empty (no incoming values) phi at the top of \p BB.
  llvm::PHINode *addEmptyPhi(llvm::BasicBlock *BB,
                             Decl *Decl);
  /// Fills \p Phi with the value of \p Decl coming from each predecessor.
  llvm::Value *addPhiOperands(llvm::BasicBlock *BB,
                              Decl *Decl,
                              llvm::PHINode *Phi);
  /// Removes \p Phi if all its incoming values are equal, returning the
  /// replacement value; otherwise returns \p Phi unchanged.
  llvm::Value *optimizePhi(llvm::PHINode *Phi);
  /// Marks \p BB as having all predecessors known and resolves any
  /// outstanding \ref BasicBlockDef::IncompletePhis.
  void sealBlock(llvm::BasicBlock *BB);

  /// VAR formal parameters are addressable; keep a side map to their
  /// underlying \ref llvm::Argument.
  llvm::DenseMap<FormalParameterDeclaration *,
                 llvm::Argument *>
      FormalParams;

  /// Generic write: dispatches by kind (local var, module-level global,
  /// or VAR parameter handled via a store).
  void writeVariable(llvm::BasicBlock *BB, Decl *Decl,
                     llvm::Value *Val);
  /// Generic read counterpart to \ref writeVariable.
  llvm::Value *readVariable(llvm::BasicBlock *BB,
                            Decl *Decl);

  /// Maps a \ref Decl's tinylang type to an LLVM type. \p HonorReference
  /// makes VAR parameters lower to opaque pointers.
  llvm::Type *mapType(Decl *Decl,
                      bool HonorReference = true);
  /// Builds the \ref llvm::FunctionType from a procedure's signature.
  llvm::FunctionType *
  createFunctionType(ProcedureDeclaration *Proc);
  /// Materialises the \ref llvm::Function and applies attributes
  /// (e.g. `dereferenceable` / `nocapture` on VAR parameters).
  llvm::Function *createFunction(ProcedureDeclaration *Proc,
                                 llvm::FunctionType *FTy);

protected:
  /// Switches \ref Curr (and the Builder's insertion point) to \p BB.
  void setCurr(llvm::BasicBlock *BB) {
    Curr = BB;
    Builder.SetInsertPoint(Curr);
  }

  llvm::Value *emitInfixExpr(InfixExpression *E);
  llvm::Value *emitPrefixExpr(PrefixExpression *E);
  /// Dispatches to the right `emit…` helper based on the dynamic type of \p E.
  llvm::Value *emitExpr(Expr *E);

  void emitStmt(AssignmentStatement *Stmt);
  /// Stub — Ch04 cannot lower procedure calls yet; reaches `report_fatal_error`.
  void emitStmt(ProcedureCallStatement *Stmt);
  void emitStmt(IfStatement *Stmt);
  void emitStmt(WhileStatement *Stmt);
  void emitStmt(ReturnStatement *Stmt);
  /// Walks \p Stmts in order, dispatching each one to the right emitter.
  void emit(const StmtList &Stmts);

public:
  CGProcedure(CGModule &CGM)
      : CGM(CGM), Builder(CGM.getLLVMCtx()),
        Curr(nullptr){};

  /// Lowers \p Proc end-to-end: builds the function, materialises the
  /// entry block, emits the body, and seals the final block.
  void run(ProcedureDeclaration *Proc);
};
} // namespace tinylang
#endif