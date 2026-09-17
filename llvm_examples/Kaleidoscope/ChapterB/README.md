# Chapter B — Diagnostics, Name Resolution, and an Infallible Backend

A second design iteration on [ChapterA](../ChapterA/README.md), addressing
the weaknesses ChapterA's own README names for itself (§5 and §7 there):
error handling was "print to stderr and return null" with no diagnostic
engine, no error recovery, and no way for an embedder to intercept it; and
the codegen `Impl` was a ~570-line struct carrying four jobs at once —
including a full name-resolution walk of its own.

The *language* is unchanged from ChapterA (Chapter7-era Kaleidoscope with
the two lexer fixes), and so are the CLI, the `-emit=` stages, and the IR it
produces — `Lexer.h`, `AST.h`, `AST.cpp`, and `extern_d.cpp` are
byte-identical copies. Everything ChapterB changes is compiler-engineering
structure:

```sh
./build.sh                                   # or: cmake -B build -G Ninja && cmake --build build
./build/toyc file.k -emit=ast|ir|obj|jit [-opt] [-g]   # same driver as ChapterA
cd build && ctest                            # gtest lexer + sema suites, lit/FileCheck suite
```

## What ChapterB changes, concept by concept

**Diagnostics as data (§1).** Every real compiler separates *reporting* a
problem from *rendering* it: clang has `DiagnosticsEngine`, MLIR has
`DiagnosticEngine`, and both exist so that errors can carry structure
(severity, location, message), be counted, be intercepted by an embedder,
and be tested without scraping stderr. ChapterB's `DiagnosticEngine` is the
plain-LLVM miniature: every stage reports into one engine, the default
handler prints clang-style `file:line:col: error: message`, and the unit
tests install a silent handler and assert on the collected list.

**Error recovery (§2).** A compiler that stops at the first error makes the
user fix N problems in N runs. The classic fix is *panic-mode recovery*:
after an error, skip to a synchronization token and continue. ChapterB's
parser resynchronizes at record boundaries (`;`, `def`, `extern`), and sema
simply keeps walking after each error — so one run reports every problem,
ending with a clang-style `N errors generated.` summary:

```
$ toyc broken.k -emit=ir
broken.k:2:12: error: incorrect number of arguments to 'f': expected 2, got 1
broken.k:2:19: error: unknown variable 'y'
broken.k:3:3: error: destination of '=' must be a variable
3 errors generated.
```

**Sema is a resolver, not just a checker (§3) — the headline change.**
ChapterA resolved names *while emitting IR* — which forces
stop-at-first-error (you cannot emit IR for a broken expression) and makes
the checks untestable without LLVM. The naive fix is a checking pass in
front of codegen, but that *duplicates* scope resolution: two symbol
tables, kept consistent only by discipline. ChapterB does what clang's Sema
actually does: semantic analysis **records what it learns**. Its output, a
`Resolutions` table, binds every variable use to the declaration that
introduced it (a dense `VarId`; shadowing resolved here, once) and every
call site to its `PrototypeAST`.

**...which makes IR emission infallible (§4).** The backend consumes
bindings instead of re-resolving: variable uses index a plain
`std::vector<AllocaInst*>` by `VarId` — no symbol table, no scopes, no
"unknown variable" paths, and since arity and operators were checked too,
**emitting an expression cannot fail at all**. Error propagation,
its null-checks, and the error-path block-cleanup all disappear from IRGen;
the only failure left is a verifier rejection, which is a compiler bug, not
user input. (clang's CodeGen makes the same assumption about Sema's output.)

**One component, one job (§5).** The rest of ChapterA's `Impl` splits along
its seams: `IRGen` (AST→IR only), `Optimizer` (the pass pipeline), and
`DebugInfoEmitter` (DWARF as a null-object, so IRGen has *zero* debug-info
conditionals), composed by a now-thin facade. These live behind private
headers in `src/` — the public API in `include/toy/` keeps ChapterA's
five-method shape.

The pipeline with the new stage and its data flow:

```
              file.k / stdin  (read once via llvm::MemoryBuffer)
                        │
                ┌───────▼────────┐      ┌──────────────────────────────┐
                │  LexerBuffer   │      │       DiagnosticEngine       │
                └───────┬────────┘      │  every stage reports into it │
                ┌───────▼────────┐      │  file:line:col: error: ...   │
                │  Parser        │─────▶│  hadError() gates each stage │
                └───────┬────────┘      └──────────────────────────────┘
                    ModuleAST                ▲                  ▲
                        │                    │                  │
      -emit=ast ◀───────┤                    │                  │
                ┌───────▼────────┐           │                  │
                │  Sema          │───────────┘                  │
                └───┬────────────┘  every semantic error, one run
                    │
               Resolutions          uses → VarId, call sites → PrototypeAST
                    │
                ┌───▼────────────────────────────────┐         │
                │ CodeGenSession (facade, pImpl)     │─────────┘
                │ ┌───────┐ ┌───────────┐ ┌────────┐ │   internal errors only:
                │ │ IRGen │ │ Optimizer │ │ Debug  │ │   user errors cannot
                │ │AST→IR │ │ -opt: FPM │ │ Info   │ │   reach the backend
                │ └───────┘ └───────────┘ └────────┘ │
                └───────┬────────────────────────────┘
             ┌──────────┼───────────────┐
        -emit=ir    -emit=obj       -emit=jit
```

## Layout

```
include/toy/Lexer.h        byte-identical to ChapterA
include/toy/AST.h          byte-identical to ChapterA
include/toy/Diagnostics.h  NEW header-only: Diagnostic, DiagnosticEngine
include/toy/Parser.h       modified: reports via the engine, record-level recovery
include/toy/Sema.h         NEW: Resolutions + one free function, resolveModule()
include/toy/CodeGen.h      facade: same five methods + Resolutions& + engine
src/AST.cpp                byte-identical to ChapterA (ASTDumper)
src/Sema.cpp               NEW: the Resolver, in an anonymous namespace
src/IRGen.h/.cpp           AST→IR consuming Resolutions; no symbol table
src/Optimizer.h/.cpp       the pass pipeline (mem2reg..SimplifyCFG)
src/DebugInfo.h/.cpp       DWARF as a null-object behind hook methods
src/CodeGen.cpp            the facade Impl, now a thin composer
src/toyc.cpp               driver: sema stage + "N errors generated."
src/extern_d.cpp           byte-identical to ChapterA
test/lexer_test.cpp        byte-identical to ChapterA
test/sema_test.cpp         NEW gtest: diagnostics, recovery, bindings — no IR
test/filecheck/*.k         carried forward + new error/warning suites
```

Note the second rule this adds to ChapterA's public/private split: `IRGen.h`,
`Optimizer.h`, and `DebugInfo.h` live in `src/`, not `include/toy/` — they
are implementation detail of the backend, free to include LLVM IR headers
because nothing outside `src/` can reach them. The compile-time firewall of
the facade is preserved.

---

## The applied patterns, in detail

### 1. DiagnosticEngine: severity + location + message, handler-based

**Before (ChapterA §7).** Each layer printed directly to `llvm::errs()` in
its own format (`Parse error (12, 9): ...`, `Codegen error (4, 1): ...`) and
signaled failure with `nullptr`. Honest and layered — but unstructured:
nothing could count errors, keep going, or intercept them; testing an error
required a subprocess and stderr scraping.

**After.** A diagnostic is a struct; the engine stores every one, counts
errors, and forwards each to a handler:

**`ChapterB/include/toy/Diagnostics.h`**
```cpp
struct Diagnostic {
  enum Severity { Error, Warning, Note };
  Severity severity;
  Location loc;
  std::string message;
  void print(llvm::raw_ostream &os) const;   // file:line:col: error: message
};

class DiagnosticEngine {
public:
  using Handler = std::function<void(const Diagnostic &)>;
  DiagnosticEngine();                        // default: print to llvm::errs()
  explicit DiagnosticEngine(Handler handler);

  void error(Location loc, const llvm::Twine &message);
  void warning(Location loc, const llvm::Twine &message);
  void note(Location loc, const llvm::Twine &message);   // "previously defined here"

  bool hadError() const;
  unsigned errorCount() const;
  llvm::ArrayRef<Diagnostic> diagnostics() const;
};
```

Three severities earn their keep immediately: `note` lets a redefinition
error point back at the first definition, and `warning` lets sema flag a
duplicate parameter name without failing the compile
(`test/filecheck/warn-dup-param.k` pins that the exit code stays 0).

The engine restores something ChapterA's §8 explicitly gave up: **white-box
error testing**. A unit test installs a silent handler and asserts on
severity, message, and *exact* location — no subprocess, no LLVM IR:

**`ChapterB/test/sema_test.cpp`**
```cpp
TEST(SemaTest, UnknownVariable) {
  FrontendRun run("def f(x) y;");
  ASSERT_EQ(run.diags.errorCount(), 1u);
  EXPECT_EQ(run.diag(0).message, "unknown variable 'y'");
  EXPECT_EQ(run.diag(0).loc.line, 1);
  EXPECT_EQ(run.diag(0).loc.col, 10);
}
```

* **Advantages:** errors are countable, interceptable, and testable as
  values; one rendering convention across all stages; warnings and notes
  become possible at all.
* **Disadvantages:** every stage now carries a `DiagnosticEngine&`
  (constructor plumbing ChapterA didn't need), and messages built eagerly
  into `std::string`s cost a little even when a handler would discard them.
  One sharp edge: a `llvm::Twine` must never be stored across statements
  (it references its operands), so `parseError` builds `std::string`
  eagerly — the same rule LLVM's own style guide states.

### 2. Parser error recovery at record boundaries

The parser reports through the engine and then *resynchronizes* instead of
giving up. The synchronization points are the natural record boundaries:

**`ChapterB/include/toy/Parser.h`**
```cpp
  /// Panic-mode error recovery: skip tokens until a plausible start of the
  /// next record. 'def' and 'extern' are safe stopping points without
  /// consuming them (parseDefinition/parseExtern always consume the keyword,
  /// so the loop in parseModule makes progress); a ';' is consumed since it
  /// *ends* the bad record.
  void recoverToNextRecord() {
    while (true) {
      switch (lexer.getCurToken()) {
      case tok_eof:
      case tok_def:
      case tok_extern:
        return;
      case tok_semicolon:
        lexer.consume(tok_semicolon);
        return;
      default:
        lexer.getNextToken();
      }
    }
  }
```

`parseModule()` calls it after every failed record and keeps going, so it
now **always returns a module** — possibly partial — and callers consult the
engine for success (the driver: `if (diags.hadError()) return ...`). The
comment in the snippet is the progress argument: recovery must guarantee the
loop advances, or a bad token at a boundary loops forever.

Error messages also got a spelling pass. ChapterA printed the raw token
number (`... but has Token -2`); ChapterB renders tokens the way the user
wrote them (`describeCurToken()`): keywords as `'def'`, identifiers as
`identifier 'foo'`, numbers as `a number`, punctuation as `';'`.

* **Advantages:** all parse errors in one run; a partial AST survives for
  tooling that wants it; recovery is ~15 lines because record boundaries in
  this grammar are unambiguous.
* **Disadvantages:** panic-mode is crude — everything between the error and
  the boundary is skipped unexamined, so an error *inside* a `def` hides any
  further errors in that same `def`. And one upstream quirk limits where
  errors land: `parseUnary` treats any ASCII token as a candidate unary
  operator, so in `x + ;` the `;` is consumed as an "operator" and the error
  is reported at whatever follows (see the note in
  `test/sema_test.cpp`). Fixing that would mean whitelisting operator
  characters — a language change, deliberately not made.

### 3. Sema resolves; Resolutions is its output

**Before.** ChapterA's IRGen resolved names *while emitting*: a
`ScopedHashTable` of allocas for variables, a name→prototype registry for
calls, checks for unknown names / arity / redefinition / the `'='` rule
inline in the emit methods. Consequences: the first error necessarily
aborted the run, the checks were untestable without constructing LLVM IR,
and one class of bug was diagnosed misleadingly — `extern f(x); def f(a b)`
generated IR against the *old* arity and then failed inside the body with
`unknown variable 'b'`.

**After.** One scoped walk in `Sema.cpp` does two jobs: **check** (reporting
every error, recovering after each) and **resolve** (recording what the
walk learned). The result is a side table:

**`ChapterB/include/toy/Sema.h`**
```cpp
/// The output of name resolution. Every *declared variable* -- a function
/// parameter, one name of a 'var ... in' expression, or a for-loop variable
/// -- gets a dense VarId (shadowing produces distinct ids); every variable
/// use is bound to the VarId it refers to, and every call site (calls,
/// user-defined unary/binary operators) is bound to its PrototypeAST.
class Resolutions {
public:
  using VarId = unsigned;

  unsigned numVariables() const;                // ids are dense: [0, N)
  VarId declaredVariable(const void *declNode, unsigned index) const;
  VarId boundVariable(const VariableExprAST &use) const;
  PrototypeAST &callee(const ExprAST &site) const;
  ...
};

Resolutions resolveModule(ModuleAST &module, DiagnosticEngine &diags);
```

Scoping — the part ChapterA implemented inside codegen — lives here now,
and *only* here: `ScopedHashTable<StringRef, VarId>` with one RAII scope per
function body / for-loop / var-expr, initializers resolved before their own
name is inserted. Shadowing is decided at resolve time by handing each
declaration a fresh `VarId`:

**`ChapterB/src/Sema.cpp`**
```cpp
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
```

**Side table vs annotating the AST.** The other classic home for bindings
is *on the nodes* (clang's AST is built already-annotated by Sema; Rust
lowers to a new, resolved tree). ChapterB deliberately chose the side table:
the AST stays pure parse output (ChapterA's §2 principle survives — nodes
never change after parsing, and `-emit=ast` dumps exactly what the parser
saw), at the price of a `DenseMap` lookup per use and a lifetime rule (the
`Resolutions` must accompany the ModuleAST wherever the backend goes). For
in-place annotation the trade reverses: no side structure to carry, but
nodes gain a mutable "resolved" slot and every consumer must know whether
resolution has run yet. Both are legitimate; what matters is that
resolution happens *once*.

Sema also diagnoses two things ChapterA could not say clearly: the
arity-conflict case above is now
`conflicting declaration of 'f': 2 parameters, previously declared with 1`
plus a `note: previous declaration is here` (`error-redefine.k`), and a
duplicate parameter (`def f(x x)`) gets a *warning*. One check is
unreachable from source text and the code says so: an *unknown binary
operator* cannot survive to sema, because an unregistered operator token
never parses as a binary op in the first place (the parser owns the
precedence table — the ChapterA design decision paying off again).

* **Advantages:** all semantic errors in one run; scoping implemented
  exactly once; testable without LLVM IR (the whole `sema_test.cpp` suite,
  bindings included, runs in milliseconds); better messages for declaration
  conflicts.
* **Disadvantages:** the side table is address-keyed, so it silently pairs
  only with the ModuleAST it was computed from — hand the backend a
  mismatched pair and the asserts fire (debug) or bindings dangle
  (release). A `(ModuleAST, Resolutions)` wrapper type would make the
  pairing unforgeable; at this scale the constructor comment carries it.

### 4. IRGen consumes bindings — and becomes infallible

This is the payoff of §3, visible as *deleted* code. The backend requires a
clean `Resolutions` and trusts it, exactly as clang's CodeGen trusts Sema:

**`ChapterB/src/IRGen.h`**
```cpp
  /// Storage for every declared variable, indexed by VarId. A plain vector
  /// replaces ChapterA's ScopedHashTable: resolution already decided which
  /// declaration each use refers to, so there is nothing left to scope.
  std::vector<llvm::AllocaInst *> allocas;
```

**`ChapterB/src/IRGen.cpp`**
```cpp
llvm::Value *IRGen::emit(VariableExprAST &var) {
  llvm::AllocaInst *alloca = allocas[resolutions.boundVariable(var)];
  assert(alloca && "use emitted before its resolved declaration");
  return builder.CreateLoad(alloca->getAllocatedType(), alloca,
                            var.getName());
}
```

What that deletes, walking ChapterA's list of hard-won subtleties:

| ChapterA's IRGen had | ChapterB's IRGen |
|---|---|
| `ScopedHashTable` + RAII scope per function/loop/var-expr | a vector indexed by `VarId`; shadowing already resolved |
| "insert the loop variable only *after* emitting the start expression" ordering rule | gone — the start's uses bind to the *outer* VarId, so publishing the alloca early is harmless |
| `unknown variable / function / operator`, arity, `'='` dyn_cast checks | gone (sema's job); contract violations are asserts |
| null-propagation on every emit call | gone — `emitExpr` cannot fail |
| `eraseUnparented` cleanup for blocks leaked on error paths in if/then/else | gone — there are no error paths to leak on |
| name→prototype registry (`functionProtos`) kept by the session for JIT-mode redeclaration | gone — each call site carries its `PrototypeAST*`; the Resolutions table *is* the cross-module registry |

The one failure that remains is real: `verifyFunction` after emission, which
now means a compiler bug and is reported as
`internal error: function 'f' failed verification`
(`discardBrokenFunction` still handles it without dangling call sites).

A note on what was *not* done: ChapterA's §7 wishlist mentioned
`llvm::Expected<T>` plumbing as the "proper" error channel for codegen.
ChapterB makes that moot rather than adopting it — once user errors are
resolved away, there is nothing left for `Expected` to carry. Eliminating a
failure mode beats typing it.

* **Advantages:** IRGen shrinks to the actual translation logic; a whole
  class of "checker and emitter disagree about scope" bugs becomes
  impossible; the alloca vector is O(1) per use with no hashing.
* **Disadvantages:** IRGen is unusable without a prior resolve — embedders
  lose the "just emit this AST" shortcut (that is the contract working as
  intended, but it is a real constraint); and `assert`-based contract
  enforcement vanishes in release builds, leaning on the debug-build test
  suite to catch misuse.

### 5. The backend components and the staged driver

The rest of ChapterA's `Impl` — pass pipeline and DWARF state — splits into
injected components. `Optimizer` owns the pass managers (its member
*declaration order* encodes the teardown-order invariant; see the comment
in `Optimizer.h`). `DebugInfoEmitter` is a **null-object**:
default-constructed it is disabled and every hook is a no-op, so IRGen
calls the hooks unconditionally and contains not a single debug-info
conditional:

**`ChapterB/src/IRGen.cpp`**
```cpp
  llvm::BasicBlock *bb = llvm::BasicBlock::Create(context, "entry", fn);
  builder.SetInsertPoint(bb);
  debug.functionBegin(proto, *fn, builder);   // no-op without -g
```

ChapterA's trickiest invariant — tear down everything referencing the
context *before* moving it into a `ThreadSafeModule` — is now one line per
component:

**`ChapterB/src/CodeGen.cpp`**
```cpp
  llvm::orc::ThreadSafeModule take() {
    irgen.reset();       // the IRBuilder
    optimizer.reset();   // instrumentation callbacks
    debugInfo.reset();   // the DIBuilder
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(context));
    initializeModule();
    return tsm;
  }
```

The driver grows one stage and one convention: sema gates every backend
action, and every failing stage ends with the clang-style summary.
`-emit=ast` remains a pure parse-tree dump (undeclared names dump fine, and
the frontend still touches zero LLVM target machinery):

**`ChapterB/src/toyc.cpp`**
```cpp
  // Semantic analysis gates every backend stage (all errors in one run)
  // and produces the name resolution the backend consumes.
  Resolutions resolutions = resolveModule(*moduleAST, diags);
  if (diags.hadError())
    return reportErrors(diags);          // "N errors generated."
```

## Tests

Same two schemes as everywhere in this repo (see the
[top-level README](../README.md#testing-the-two-schemes)); what ChapterB
adds is the middle of the pyramid:

- **gtest** `lexer_test` — byte-identical carry-over from ChapterA.
- **gtest** `sema_test` — NEW: the DiagnosticEngine as a test fixture.
  Clean programs, every sema error with exact locations, warnings vs
  errors, notes, scoping edge cases (`for`-variable leaving scope, `var`
  initializer resolution), parser recovery counts, message spelling — and
  the resolver's output itself (shadowing yields distinct `VarId`s; every
  declaration form gets one).
- **lit/FileCheck** — the success-path suites (`ast.k`, `ir.k`, `opt.k`,
  `controlflow.k`, `userops.k`, `mutablevars.k`, `jit.k`, `objfile.k`,
  `debuginfo.k`) are carried forward unchanged: the IR and executed values
  must not move — which is also the regression proof that consuming
  bindings emits the same code ChapterA's symbol table did. The error
  suites are rewritten for the new format and capabilities:
  `error-parse.k` (two parse errors, one run), `error-multiple.k` (three
  sema errors + summary), `error-redefine.k` (error + note),
  `error-arity.k`, `error-assign.k`, `error-undef-var.k`,
  `error-unknown-op.k`, `warn-dup-param.k` (warning, exit 0).

One hand-off to record: ChapterA's `error-broken-body.k` (a failing body
must not erase a function that earlier code already calls) has no ChapterB
equivalent — sema now rejects such a module before IR generation begins,
and expression emission cannot fail, so the only path into
`discardBrokenFunction` is a verifier rejection, which no source input
triggers. The hardening remains for that internal-error path.

```sh
ctest --test-dir build                    # everything
./build/sema_test                         # the new suite directly
lit -v test/filecheck/error-multiple.k    # the headline demo
```

## Known limitations / future work

- **Recovery granularity is the record.** An error inside a `def` hides
  later errors in the same `def`; statement-level synchronization points
  don't exist in this grammar.
- **The (ModuleAST, Resolutions) pairing is by convention.** The side table
  is address-keyed; nothing but the documented contract stops a caller
  handing the backend a table computed from a different tree. A wrapper
  type would make it unforgeable (§3's disadvantage).
- **Diagnostics still render eagerly** and hold no source *ranges* — a
  clang-style caret line would need the lexer to keep the current line text.
- ChapterA's own remaining limitations (no REPL, `-g` outside JIT mode
  only, no exponent syntax) are inherited unchanged.
