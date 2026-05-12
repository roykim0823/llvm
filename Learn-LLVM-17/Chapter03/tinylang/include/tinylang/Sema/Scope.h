/// \file
/// \brief Lexical scope: a name → \ref tinylang::Decl "Decl" map with a parent pointer.

#ifndef TINYLANG_SEMA_SCOPE_H
#define TINYLANG_SEMA_SCOPE_H

#include "tinylang/Basic/LLVM.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace tinylang {

class Decl;

/// A single lexical scope.
///
/// Scopes form a chain via \ref Parent. \ref insert refuses to overwrite
/// an existing binding in this scope, \ref lookup walks the chain to find
/// the nearest enclosing binding. The global scope (no parent) is created
/// by \ref Sema::initialize and pre-populated with `INTEGER`, `BOOLEAN`,
/// `TRUE`, `FALSE`. Procedure bodies introduce a child scope via
/// \ref EnterDeclScope.
class Scope {
  Scope *Parent;
  StringMap<Decl *> Symbols;

public:
  /// Creates a scope nested inside \p Parent (or a root scope if `nullptr`).
  Scope(Scope *Parent = nullptr) : Parent(Parent) {}

  /// Binds \p Declaration in this scope under `Declaration->getName()`.
  /// \returns `true` on success; `false` if a binding with the same name
  /// already exists in *this* scope (callers report `err_symbold_declared`).
  bool insert(Decl *Declaration);

  /// Finds \p Name in this scope or any enclosing one. Returns `nullptr`
  /// when undeclared.
  Decl *lookup(StringRef Name);

  /// The enclosing scope, or `nullptr` for the root.
  Scope *getParent() { return Parent; }
};
} // namespace tinylang
#endif
