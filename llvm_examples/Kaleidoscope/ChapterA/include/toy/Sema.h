//===- Sema.h - Semantic analysis: name resolution + checks ---------------===//
//
// Semantic analysis between the parser and the backend, the way clang's
// Sema sits between its parser and CodeGen. Crucially, this is a *resolver*,
// not just a checker: besides diagnosing errors (unknown names, call arity,
// redefinitions, the '=' destination rule), it records what it learns --
// every variable use bound to the declaration that introduced it, every
// call site bound to its prototype -- in a Resolutions side table.
//
// IR generation then *consumes* the bindings instead of re-resolving names
// with a second symbol table: scoping is implemented exactly once, and
// after a clean resolve, IR emission cannot fail on user input at all.
//
// The bindings live in a side table keyed by node address rather than in
// fields on the AST nodes; see the README for the annotate-in-place vs
// side-table trade-off (this keeps the AST pure parse output).
//
// Like the ASTDumper, the resolver class is hidden in Sema.cpp; the public
// API is one free function plus the Resolutions result.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_SEMA_H
#define TOY_SEMA_H

#include "llvm/ADT/DenseMap.h"

#include <cassert>
#include <utility>

namespace toy {

class DiagnosticEngine;
class ExprAST;
class ModuleAST;
class PrototypeAST;
class VariableExprAST;

/// The output of name resolution. Every *declared variable* -- a function
/// parameter, one name of a 'var ... in' expression, or a for-loop variable
/// -- gets a dense VarId (shadowing produces distinct ids); every variable
/// use is bound to the VarId it refers to, and every call site (calls,
/// user-defined unary/binary operators) is bound to its PrototypeAST.
///
/// Non-owning: callee bindings point into the ModuleAST, which must outlive
/// this object and any backend consuming it.
class Resolutions {
public:
  using VarId = unsigned;

  //===--------------------------------------------------------------------===//
  // Queries (the backend)
  //===--------------------------------------------------------------------===//

  /// Total number of declared variables in the module; VarIds are dense in
  /// [0, numVariables()), so per-variable storage can be a plain vector.
  unsigned numVariables() const { return numVars; }

  /// The id assigned to declaration #index of `declNode` (a PrototypeAST
  /// parameter, a VarExprAST name, or index 0 of a ForExprAST).
  VarId declaredVariable(const void *declNode, unsigned index) const {
    auto it = decls.find({declNode, index});
    assert(it != decls.end() && "declaration was never resolved");
    return it->second;
  }

  /// The declaration a variable use refers to.
  VarId boundVariable(const VariableExprAST &use) const {
    auto it = uses.find(&use);
    assert(it != uses.end() && "variable use was never resolved");
    return it->second;
  }

  /// The prototype a call site refers to (CallExprAST, or the
  /// UnaryExprAST/BinaryExprAST of a user-defined operator).
  PrototypeAST &callee(const ExprAST &site) const {
    auto it = callees.find(&site);
    assert(it != callees.end() && "call site was never resolved");
    return *it->second;
  }

  //===--------------------------------------------------------------------===//
  // Recording (the resolver)
  //===--------------------------------------------------------------------===//

  VarId createVariable(const void *declNode, unsigned index) {
    VarId id = numVars++;
    decls[{declNode, index}] = id;
    return id;
  }
  void bindUse(const VariableExprAST &use, VarId id) { uses[&use] = id; }
  void bindCallee(const ExprAST &site, PrototypeAST &proto) {
    callees[&site] = &proto;
  }

private:
  unsigned numVars = 0;
  llvm::DenseMap<std::pair<const void *, unsigned>, VarId> decls;
  llvm::DenseMap<const VariableExprAST *, VarId> uses;
  llvm::DenseMap<const ExprAST *, PrototypeAST *> callees;
};

/// Resolve and check the module's records in source order (matching JIT
/// execution semantics: a function must be declared or defined before it is
/// called). Every error found is reported through `diags`; the walk does
/// not stop at the first one. Always returns the (possibly partial)
/// resolutions; callers consult diags.hadError() for success, and must only
/// hand the result to the backend when it reports none.
Resolutions resolveModule(ModuleAST &module, DiagnosticEngine &diags);

} // namespace toy

#endif // TOY_SEMA_H
