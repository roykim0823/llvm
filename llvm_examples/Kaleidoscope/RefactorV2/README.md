# Kaleidoscope RefactorV2

A single-codebase reimplementation of the LLVM Kaleidoscope tutorial
(Chapters 2–9), applying the frontend design of the
[MLIR Toy tutorial](https://mlir.llvm.org/docs/Tutorials/Toy/) to a compiler
that targets LLVM IR.

`Refactor2`–`Refactor9` (siblings of this directory) stay faithful to the
upstream `toy.cpp` code, chapter by chapter. This directory is the "what it
looks like when designed deliberately" counterpart. The chapter progression
is replaced by **one binary with staged actions**:

```sh
./build.sh                                   # or: cmake -B build -G Ninja && cmake --build build

./build/toyc file.k -emit=ast               # Ch2:  dump the AST (with source locations)
./build/toyc file.k -emit=ir                # Ch3:  raw LLVM IR on stdout
./build/toyc file.k -emit=ir  -opt          # Ch4:  optimized IR (mem2reg, InstCombine, Reassociate, GVN, SimplifyCFG)
./build/toyc file.k -emit=jit -opt          # Ch4–7: execute top-level expressions
./build/toyc file.k -emit=obj -o out.o      # Ch8:  object file
./build/toyc file.k -emit=ir  -g            # Ch9:  DWARF debug info
cat file.k | ./build/toyc -emit=jit         # input defaults to stdin ("-")
```

Tests: `cd build && ctest` runs the gtest lexer suite plus a lit/FileCheck
suite (`test/filecheck/*.k`).

## Layout

```
include/toy/Lexer.h      header-only: Location, Token, Lexer (abstract), LexerBuffer
include/toy/AST.h        pure-data AST: kind enum + classof + Location per node
include/toy/Parser.h     header-only recursive descent, parseError<T>, precedence table
include/toy/CodeGen.h    36-line facade: CodeGenSession (pImpl), no LLVM IR headers
src/AST.cpp              ASTDumper (anonymous namespace) behind one free function
src/CodeGen.cpp          all IR generation, symbol table, pass pipeline, debug info
src/toyc.cpp             driver: cl::opt, staged pipeline, JIT loop
src/extern_d.cpp         putchard/printd runtime functions for the JIT
test/lexer_test.cpp      gtest: lexer fed from plain strings
test/filecheck/*.k       lit + FileCheck end-to-end tests
```

This mirrors the Toy tutorial's rule: **Lexer and Parser are header-only;
AST and code generation split declaration/implementation, with the
implementation class hidden in an anonymous namespace (or behind a pImpl)
and a minimal public API.**

---

## The applied patterns, in detail

### 1. Buffer-based lexer (`readNextLine()` / `LexerBuffer`)

**Before.** `Lexer::gettok()` called `getchar()` directly, hardwiring the
lexer to process-global `stdin`. This one decision caused the worst test
infrastructure problem in Refactor2–8: every parser/codegen/JIT test had to
write a fixed-name temp file into `$CWD` and `freopen()` it over `stdin` —
racy under `ctest -j`, impossible to undo, and incapable of running two
inputs in one process.

**After.** `Lexer` is an abstract class owning all tokenization logic, with
one pure virtual extension point:

```cpp
virtual llvm::StringRef readNextLine() = 0;   // "" signals EOF
```

`LexerBuffer` is the concrete implementation over a `(begin, end)` memory
range. The driver reads the whole input once via
`llvm::MemoryBuffer::getFileOrSTDIN()` and hands the lexer the range. A test
is now just:

```cpp
LexerBuffer lexer(str.data(), str.data() + str.size(), "test.k");
```

The same extension point is what makes **location tracking** possible:
`getNextChar()` maintains line/column counters, and `getTok()` snapshots
them *before* consuming a token, so every token knows where it starts. The
`Location` struct stores the filename as a `shared_ptr<std::string>` so AST
nodes can copy locations cheaply.

* **Advantages:** testable without any process-global state; parallel-safe
  tests; real source locations (which Chapter 9's debug info and every error
  message need); multiple inputs per process.
* **Disadvantages:** the interactive REPL is gone — v2 parses the whole
  input before doing anything, so you don't get a `ready>` prompt with
  incremental feedback. (A `LexerStdin` subclass implementing
  `readNextLine()` from a prompt would restore it; that is precisely what
  the abstraction is for, but it is not written.) Also, the whole input must
  fit in memory — irrelevant here, but a real difference from a streaming
  lexer.

### 2. AST with kind tags + `classof` (LLVM-style RTTI), locations, pure data

**Before.** AST nodes had a virtual `codegen(IRGenContext&)` method, private
members with *zero accessors*, no locations, and no way to distinguish node
types at runtime (LLVM builds with `-fno-rtti`, so `dynamic_cast` is
unavailable). Three consequences:

- `ast.h` had to include `ir_gen_ctx.h`, which includes `IRBuilder.h`,
  `PassBuilder.h`, and the ORC JIT — so `lexer_test` compiled the JIT
  headers just to test `gettok()`.
- Parser tests could only assert "the pointer is not null", because the tree
  was write-only. Operator precedence — the thing Chapter 2 is *about* — was
  untestable at the AST level.
- The `'='` assignment codegen did
  `static_cast<VariableExprAST*>(LHS.get())` and then checked the result for
  null — a check that can never fire. `1 = 2` was undefined behavior.

**After.** Every node carries a `const ExprASTKind kind` set at
construction, plus a `Location`, and exposes getters. Each class has the
one-line enabler for `llvm::isa/dyn_cast/cast`:

```cpp
static bool classof(const ExprAST *c) { return c->getKind() == Expr_Num; }
```

The nodes have **no virtual methods except the destructor**; codegen and
dumping live outside the tree. Module-level items (`FunctionAST`,
`ExternAST`) form a second small hierarchy under `RecordAST` (the Toy Ch7
pattern), so `ModuleAST` is one ordered, heterogeneous list — order matters
for JIT semantics and for operator definitions.

The payoff shows up in three places:

- `AST.h` includes only `Lexer.h` (for `Location`) and LLVM ADT headers —
  no IR headers anywhere near the frontend.
- `1 = 2` is now a *checked* error:
  `llvm::dyn_cast<VariableExprAST>(bin.getLHS())` returns null and codegen
  reports `destination of '=' must be a variable` with the source location
  (see `test/filecheck/error-assign.k`).
- The dumper makes the parse tree observable, so precedence and
  associativity are directly testable (`test/filecheck/ast.k`).

* **Advantages:** decoupled layers, safe downcasts without C++ RTTI, testable
  parser output, locations available to every later stage.
* **Disadvantages:** more boilerplate per node (kind enum entry, `classof`,
  getters, location plumbing through every constructor), and adding a node
  type now requires touching *three* places (AST.h, the dumper, the codegen
  switch) instead of one class. The virtual-method design has genuinely
  better locality when the tree and its behaviors evolve together; the
  external-traversal design wins when multiple independent consumers walk
  the same tree (dumper, codegen, and whatever comes next). A compiler
  almost always ends up in the second situation.

### 3. External ASTDumper (anonymous namespace, `TypeSwitch`, RAII indent)

The dumper lives entirely in `src/AST.cpp`; the header exposes exactly one
free function, `void dump(ModuleAST&)`. Dispatch uses `llvm::TypeSwitch`
over the kind tags; indentation is an RAII `Indent` struct bumped by an
`INDENT()` macro at the top of each method. Locations print as
`@file:line:col`, which the FileCheck tests match with a wildcarded
directory but **exact** line/col — making the tests a regression suite for
the location plumbing itself.

* **Advantages:** the dump format is a stable, greppable contract; zero cost
  in the AST classes; trivially replaceable (e.g. by a JSON dumper) without
  touching the tree.
* **Disadvantages:** the `.Case<...>` list and the kind enum must be kept in
  sync by hand; a forgotten case falls into the `<unknown Expr>` default
  rather than a compile error. (A `switch` on the enum — as codegen does —
  gets `-Wswitch` coverage instead; the dumper trades that for terser code.)

### 4. Parser: `parseError<T>`, and parser-owned operator precedence

Two things changed beyond cosmetics.

**Error reporting.** Every failure path goes through one template:

```cpp
return parseError<ExprAST>(")", "to close parenthesized expression");
// Parse error (12, 9): expected ')' to close parenthesized expression but has Token ';'
```

The explicit template parameter is the caller's return type, so error
returns are one-liners; location comes from the lexer. Contrast the old
`logError("expected ')'")` with no location and three near-identical helper
functions.

**Precedence ownership — the real design fix.** In Refactor6–9, the
`binopPrecedence` map lived in `IRGenContext` (the codegen context!) and was
*written by* `FunctionAST::codegen` when a user-defined operator was
defined. IR generation retroactively changed how source text parses — a
genuine layering inversion, and the source of a crash chain (when codegen
failed, the precedence entry leaked, and the next use of the operator hit an
`assert`).

In v2 the parser owns the table and registers `def binary| 5 (a b) ...` **at
prototype-parse time**, before the body is parsed:

- subsequent input — including the operator's own body, so it can be
  recursive — parses with the right precedence;
- if the *body fails to parse*, the parser unregisters the operator;
- codegen never mutates grammar state, in either direction.

* **Advantages:** one-directional dependency (parse → codegen), no
  parse-behavior-depends-on-codegen-success edge cases, and whole-module
  parsing becomes possible (which the staged driver needs).
* **Disadvantages:** a subtle semantic difference from upstream: if an
  operator's *codegen* fails (e.g. its body references an unknown variable),
  upstream unregistered it; v2's parser has already parsed later input with
  the operator registered. In practice the driver stops at the first codegen
  error, so the difference is unobservable, but it is a real divergence to
  be aware of. Also, "the parser does a little semantic work" (registering
  operators) slightly blurs the "no semantic checks in the parser" rule the
  Toy tutorial states; it is the price of user-extensible grammar.

### 5. Codegen behind a facade: pImpl session, switch dispatch, `ScopedHashTable`

**Before.** `IRGenContext` was a god object with ~14 *public* members —
`LLVMContext`, `Module`, `IRBuilder`, six pass/analysis managers,
instrumentation, the ORC JIT, an `ExitOnError` that calls `exit()`, the
prototype registry, and the parser's precedence table — with no invariants,
mutated freely by AST `codegen` methods and by tests.

**After.** `include/toy/CodeGen.h` is a small facade with **no LLVM IR
includes**: a `CodeGenSession` with five methods (`emitRecord`,
`currentModule`, `finalize`, `takeModule`, `setDataLayout`) and a pImpl.
Everything else — the emit overloads, the pass pipeline, debug info state —
is private to `src/CodeGen.cpp`. Dispatch is a `switch` on the node kind
with `llvm::cast<>`, exactly like Toy's `MLIRGenImpl::mlirGen(ExprAST&)`.

The symbol table is the headline improvement:

```cpp
llvm::ScopedHashTable<llvm::StringRef, llvm::AllocaInst *> symbolTable;
// per function / var-expr / for-loop:
SymbolTableScopeT varScope(symbolTable);   // RAII: pops everything on scope exit
```

The old design used a plain `std::map` with *manual* save/restore
(`OldBindings` vectors, `OldVal` temporaries) — and got it wrong twice:
`VarExprAST::codegen`'s error path returned before restoring (leaving the
symbol table corrupted), and every failed `operator[]` lookup *inserted* a
null entry that then participated in the shadow/restore logic. With RAII
scopes, both bug classes are structurally impossible: scopes pop on success
*and* on error, and `lookup()` never inserts.

Other deliberate departures from upstream, all in this file:

| Upstream behavior | v2 behavior |
|---|---|
| `assert(F && "binary operator not found!")` → abort in debug, UB in release | located error: `unknown binary operator '?'` |
| unchecked `static_cast` for `'='` → UB on `1 = 2` | `dyn_cast` → located error |
| `verifyFunction`/`verifyModule` results discarded | checked; broken functions erased, driver exits non-zero |
| `if/then/else` codegen leaked unparented `BasicBlock`s on error | blocks deleted on the error path |
| `InitializeModuleAndPassManager()` reassigned the context while the old module/instrumentation still referenced it | `takeModule()` tears down builder/pass managers/instrumentation *before* moving the module+context out as a `ThreadSafeModule` |
| prototype ownership moved out of `FunctionAST` during codegen (second `codegen()` call = null deref) | the AST keeps ownership; the session stores non-owning `PrototypeAST*` |

* **Advantages:** compile-time firewall (touching codegen internals rebuilds
  one .cpp; consumers of the facade never see LLVM IR headers), invariants
  enforced in one place, error paths that cannot corrupt state, and a JIT
  handoff without use-after-free hazards.
* **Disadvantages:** the pImpl adds indirection — tests can no longer poke
  `ctx->namedValues` or `ctx->builder` directly, which is exactly why the
  old gtest codegen suites (building AST nodes by hand and inspecting
  internals) have no v2 equivalent. v2 deliberately trades white-box unit
  tests for end-to-end FileCheck tests; if you want white-box tests back,
  the session needs test-only accessors or a friend declaration.
  One lifetime rule also became implicit: `functionProtos` holds non-owning
  pointers, so **the `ModuleAST` must outlive the `CodeGenSession`** (the
  driver guarantees this; the header documents it).

### 6. Staged driver (`cl::opt`, ordered `Action` enum)

**Before.** Each chapter directory *edited* the shared code — the JIT blocks
in `handleTopLevelExpression` were commented in or out per chapter, and
`IRGenContext` grew/lost members. That is how Refactor8 shipped with test
suites dereferencing a JIT that was commented out (they crashed with
SIGSEGV).

**After.** One driver, one code path, behavior selected by flags:

```cpp
enum Action { None, DumpAST, DumpIR, EmitObj, RunJIT };
```

`-emit=ast` short-circuits before any LLVM target machinery is initialized
(the frontend has zero backend dependency, same as Toy). JIT mode walks the
module's records in source order: definitions each get their own module
(`takeModule()` per record) so they can be freed independently; top-level
expressions are compiled, executed, printed as `Evaluated to ...`, and their
resources released via a `ResourceTracker`.

* **Advantages:** no more per-chapter code surgery, so no more
  "tests written for a feature this directory just removed"; every stage is
  exercised by the same frontend; adding a stage is adding an enum value and
  a case.
* **Disadvantages:** a single binary carries all stages, so even `-emit=ast`
  links against the ORC JIT (larger binary, slower link). The Toy tutorial's
  per-chapter binaries are better as a *teaching* progression — you can diff
  Ch4 against Ch5 to see exactly what a feature costs. v2 gives up that
  pedagogical diffability; the per-chapter story lives in `Refactor2–9`
  instead, which is why both trees exist.

### 7. Layered error handling

Each layer has exactly one failure mechanism, in the Toy style:

| Layer | Mechanism | Signal |
|---|---|---|
| Lexer | never fails (`assert` for API misuse only) | — |
| Parser | `parseError<T>` → `llvm::errs()` with (line, col) | `nullptr` |
| CodeGen | `emitError(Location, msg)` → `llvm::errs()` | `nullptr` |
| Driver | distinct messages | non-zero exit code |

The old tree mixed three incompatible styles (`logError*` returning null,
`assert` that vanished in release builds, and `ExitOnErr` calling `exit()`
from library code — which could take the whole gtest process down). The
non-zero exit code is what lets lit error tests be written as
`RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s`.

* **Advantages:** predictable failure behavior in every build mode; errors
  carry locations; testable as text.
* **Disadvantages:** it is still "print to stderr and return null" — there
  is no diagnostic engine, no error recovery (first parse error aborts the
  whole module, where the old REPL skipped a token and kept going), and no
  way for an embedder to intercept diagnostics. MLIR's `emitError` /
  `DiagnosticEngine` solves that properly; a plain-LLVM equivalent would
  need a callback or `llvm::Expected<T>` plumbing, which was judged not
  worth it here.

### 8. Testing strategy: FileCheck for shape, gtest for the rest

The test pyramid follows what each tool measures well:

- **gtest** (`test/lexer_test.cpp`): token-level facts and *locations*, fed
  from strings. No `freopen`, no temp files, safe under `ctest -j`.
- **lit + FileCheck** (`test/filecheck/*.k`): everything observable from the
  outside. Each `.k` file is its own expectation (`RUN:` line + `CHECK`
  directives): AST structure and precedence (`ast.k`), raw and optimized IR
  shape with def-use captures like `[[MUL:%.*]]` (`ir.k`, `opt.k`,
  `controlflow.k`, `userops.k`, `mutablevars.k`), executed values
  (`jit.k`), object emission (`objfile.k`), debug metadata (`debuginfo.k`),
  and exact error locations (`error-*.k`).

Compared to the Refactor2–9 suites, the coverage that previously required
fragile substring matching (instruction counting loops, float formatting,
block-label greps) is expressed directly; and things that were *untestable*
before — precedence from the dump, error locations, `1 = 2` — have tests.
What was lost: the white-box gtest codegen tests (see §5's disadvantage) and
JIT-execution tests as C++ (`EXPECT_DOUBLE_EQ(fp(), 6.0)`) — v2 checks
executed values through the printed `Evaluated to` text instead, which is
one step less precise (fixed 6-digit formatting) but covers the same
behavior end-to-end.

---

## Known limitations / future work

- **No interactive REPL** (see §1). Add a `LexerStdin` subclass and a
  record-at-a-time driver loop if wanted.
- **No error recovery** in the parser; first error aborts (§7).
- **`-g` is only supported with `-emit=ir`/`-emit=obj`** (single-module
  mode); JIT mode disables it, like upstream Chapter 9.
- Numbers still have no exponent syntax (`1.5e3` lexes as `1.5`, `e3`) —
  kept for language fidelity; the lexer test documents it.
- v2 requires `def`-before-use across records for *calls at JIT time*
  (records are processed in source order), same as the upstream REPL.
