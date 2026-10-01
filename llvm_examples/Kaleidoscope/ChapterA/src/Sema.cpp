//===- Sema.cpp - Semantic analysis: name resolution + checks -------------===//
//
// One scoped walk over the module that does two jobs at once:
//
//  CHECK -- report every semantic error, recovering after each one:
//   - every variable reference resolves to a binding in scope;
//   - the destination of '=' is a variable (and is in scope);
//   - every call names a function already declared or defined, with the
//     right number of arguments (records are walked in source order, the
//     same order the JIT executes them);
//   - a function with a body is not redefined, and a declaration never
//     changes an earlier declaration's arity;
//   - a user-defined unary/binary operator is only used if 'unaryX'/
//     'binaryX' exists.
//
//  RESOLVE -- record what the walk learned into the Resolutions table:
//   each declared variable gets a dense VarId, each use is bound to the
//   VarId of the declaration it refers to (shadowing resolved here, once),
//   and each call site is bound to its PrototypeAST.
//
// The scope discipline is the ScopedHashTable + RAII-scope pattern the
// backend used to duplicate; after this pass, the backend never touches a
// symbol table again.
//
//===----------------------------------------------------------------------===//

#include "toy/Sema.h"
#include "toy/AST.h"
#include "toy/Diagnostics.h"

#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Casting.h"

using namespace toy;

namespace {

class Resolver {
public:
  Resolver(Resolutions &res, DiagnosticEngine &diags)
      : res(res), diags(diags) {}

  void run(ModuleAST &module) {
    for (auto &record : module)
      resolveRecord(*record);
  }

private:
  Resolutions &res;
  DiagnosticEngine &diags;

  /// Functions declared or defined so far, in source order. Non-owning,
  /// like the bindings themselves: the ModuleAST owns the prototypes.
  llvm::StringMap<PrototypeAST *> functions;

  /// Names that already have a body (for the redefinition check).
  llvm::StringSet<> defined;

  /// Variables currently in scope, mapped to their VarId. One RAII scope
  /// per function body / for-loop / var-expr.
  llvm::ScopedHashTable<llvm::StringRef, Resolutions::VarId> scope;
  using ScopeT =
      llvm::ScopedHashTableScope<llvm::StringRef, Resolutions::VarId>;

  //===--------------------------------------------------------------------===//
  // Records
  //===--------------------------------------------------------------------===//

  /// Register a prototype, diagnosing an arity conflict with an earlier
  /// declaration of the same name (ChapterA generated IR against the OLD
  /// arity and then failed confusingly inside the body).
  void declareFunction(PrototypeAST &proto) {
    auto it = functions.find(proto.getName());
    if (it != functions.end() &&
        it->second->getArgs().size() != proto.getArgs().size()) {
      diags.error(proto.loc(),
                  "conflicting declaration of '" + proto.getName() + "': " +
                      llvm::Twine(proto.getArgs().size()) +
                      " parameters, previously declared with " +
                      llvm::Twine(it->second->getArgs().size()));
      diags.note(it->second->loc(), "previous declaration is here");
      return; // keep the original: later uses bind to the first arity
    }
    functions[proto.getName()] = &proto;
  }

  void resolveRecord(RecordAST &record) {
    if (auto *func = llvm::dyn_cast<FunctionAST>(&record))
      return resolveFunction(*func);
    declareFunction(*llvm::cast<ExternAST>(&record)->getProto());
  }

  void resolveFunction(FunctionAST &func) {
    PrototypeAST &proto = *func.getProto();

    if (defined.contains(proto.getName())) {
      diags.error(proto.loc(),
                  "function '" + proto.getName() + "' cannot be redefined");
      diags.note(functions[proto.getName()]->loc(),
                 "previously defined here");
      // Fall through: the body is still resolved for further errors.
    }
    // Register before the body so the function can call itself recursively
    // (the same order in which the parser registers operator precedence).
    declareFunction(proto);
    defined.insert(proto.getName());

    ScopeT paramScope(scope);
    unsigned index = 0;
    for (const std::string &arg : proto.getArgs()) {
      // Params land in a fresh scope, so a hit here is a same-scope
      // duplicate. Legal (the second binding wins), but almost certainly a
      // mistake -- warn. (Per-parameter locations would need the AST to
      // store them; the prototype's location is close enough.)
      if (scope.count(arg))
        diags.warning(proto.loc(), "duplicate parameter name '" + arg +
                                       "' in '" + proto.getName() + "'");
      scope.insert(arg, res.createVariable(&proto, index));
      ++index;
    }
    resolveExpr(*func.getBody());
  }

  //===--------------------------------------------------------------------===//
  // Expressions (dispatch on the AST node kind)
  //===--------------------------------------------------------------------===//

  void resolveExpr(ExprAST &expr) {
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return;
    case ExprAST::Expr_Var:
      return resolve(llvm::cast<VariableExprAST>(expr));
    case ExprAST::Expr_Unary:
      return resolve(llvm::cast<UnaryExprAST>(expr));
    case ExprAST::Expr_BinOp:
      return resolve(llvm::cast<BinaryExprAST>(expr));
    case ExprAST::Expr_Call:
      return resolve(llvm::cast<CallExprAST>(expr));
    case ExprAST::Expr_If:
      return resolve(llvm::cast<IfExprAST>(expr));
    case ExprAST::Expr_For:
      return resolve(llvm::cast<ForExprAST>(expr));
    case ExprAST::Expr_VarDecl:
      return resolve(llvm::cast<VarExprAST>(expr));
    }
  }

  /// Bind one variable use to the declaration in scope, or report it.
  void bindVariable(VariableExprAST &use) {
    if (!scope.count(use.getName())) {
      diags.error(use.loc(), "unknown variable '" + use.getName() + "'");
      return;
    }
    res.bindUse(use, scope.lookup(use.getName()));
  }

  void resolve(VariableExprAST &var) { bindVariable(var); }

  void resolve(UnaryExprAST &unary) {
    resolveExpr(*unary.getOperand());
    auto it = functions.find(std::string("unary") + unary.getOpcode());
    if (it == functions.end()) {
      diags.error(unary.loc(), llvm::Twine("unknown unary operator '") +
                                   llvm::Twine(unary.getOpcode()) + "'");
      return;
    }
    res.bindCallee(unary, *it->second);
  }

  void resolve(BinaryExprAST &bin) {
    // Assignment is special: the LHS is a *destination*, not a value.
    if (bin.getOp() == '=') {
      if (auto *lhs = llvm::dyn_cast<VariableExprAST>(bin.getLHS()))
        bindVariable(*lhs);
      else
        diags.error(bin.loc(), "destination of '=' must be a variable");
      resolveExpr(*bin.getRHS());
      return;
    }

    resolveExpr(*bin.getLHS());
    resolveExpr(*bin.getRHS());

    switch (bin.getOp()) {
    case '+':
    case '-':
    case '*':
    case '<':
      return; // builtin: no callee to bind
    default:
      break;
    }
    // Unreachable from source text: an unregistered binary operator token
    // never parses as a binary op (the parser owns the precedence table,
    // and only 'def binary..' registers into it). Kept for hand-built ASTs.
    auto it = functions.find(std::string("binary") + bin.getOp());
    if (it == functions.end()) {
      diags.error(bin.loc(), llvm::Twine("unknown binary operator '") +
                                 llvm::Twine(bin.getOp()) + "'");
      return;
    }
    res.bindCallee(bin, *it->second);
  }

  void resolve(CallExprAST &call) {
    for (auto &arg : call.getArgs())
      resolveExpr(*arg);

    auto it = functions.find(call.getCallee());
    if (it == functions.end()) {
      diags.error(call.loc(),
                  "unknown function '" + call.getCallee() + "'");
      return;
    }
    size_t expected = it->second->getArgs().size();
    if (expected != call.getArgs().size()) {
      diags.error(call.loc(),
                  "incorrect number of arguments to '" + call.getCallee() +
                      "': expected " + llvm::Twine(expected) + ", got " +
                      llvm::Twine(call.getArgs().size()));
      return;
    }
    res.bindCallee(call, *it->second);
  }

  void resolve(IfExprAST &ifExpr) {
    resolveExpr(*ifExpr.getCond());
    resolveExpr(*ifExpr.getThen());
    resolveExpr(*ifExpr.getElse());
  }

  void resolve(ForExprAST &forExpr) {
    // The start expression runs before the loop variable exists.
    resolveExpr(*forExpr.getStart());

    ScopeT loopScope(scope);
    scope.insert(forExpr.getVarName(), res.createVariable(&forExpr, 0));
    resolveExpr(*forExpr.getEnd());
    if (forExpr.getStep())
      resolveExpr(*forExpr.getStep());
    resolveExpr(*forExpr.getBody());
  }

  void resolve(VarExprAST &varExpr) {
    ScopeT varScope(scope);
    unsigned index = 0;
    for (auto &decl : varExpr.getVarNames()) {
      // The initializer is resolved before its own name is inserted, so
      // 'var a = 1 in var a = a in ...' refers to the outer 'a'.
      if (decl.second)
        resolveExpr(*decl.second);
      scope.insert(decl.first, res.createVariable(&varExpr, index));
      ++index;
    }
    resolveExpr(*varExpr.getBody());
  }
};

} // namespace

Resolutions toy::resolveModule(ModuleAST &module, DiagnosticEngine &diags) {
  Resolutions res;
  Resolver(res, diags).run(module);
  return res;
}
