# Chapters A — Kaleidoscope, Redesigned (MLIR Toy style)

A single-codebase reimplementation of the LLVM Kaleidoscope tutorial
(Chapters 2–9), applying the frontend design of the
[MLIR Toy tutorial](https://mlir.llvm.org/docs/Tutorials/Toy/) to a compiler
that targets LLVM IR.

`Chapter2`–`Chapter9` (siblings of this directory) stay faithful to the
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

## What v2 changes, concept by concept

The *language* is Chapter7-era Kaleidoscope with two deliberate lexer-level
fixes (both pinned by lexer tests): identifiers may contain `_`
(`[a-zA-Z][a-zA-Z0-9_]*` — the old trees' `my_var`-lexes-as-three-tokens
gotcha is gone), and a second `.` terminates a number (`3.14.15` lexes as
`3.14`, `.`, `15` instead of silently swallowing the whole string as
`3.14`). Everything else is a compiler-engineering scheme. The pipeline
first — one binary, and each `-emit=` flag is a tap into it at a different
depth:

```
              file.k / stdin  (read once via llvm::MemoryBuffer)
                        │
                ┌───────▼────────┐
                │  LexerBuffer   │  tokens + Location{file, line, col}
                └───────┬────────┘
                ┌───────▼────────┐
                │  Parser        │  owns the operator-precedence table
                └───────┬────────┘
                    ModuleAST        (pure data: kind tags + locations)
                        │
      -emit=ast ◀───────┤  dump(ModuleAST&)     the frontend stops here —
                        │                       no LLVM target machinery touched
                ┌───────▼────────┐
                │ CodeGenSession │  pImpl facade, ScopedHashTable symbols
                └───────┬────────┘
                    LLVM IR ──(-opt: mem2reg, InstCombine, Reassociate,
                        │              GVN, SimplifyCFG)──▶ optimized IR
             ┌──────────┼───────────────┐
        -emit=ir    -emit=obj       -emit=jit
        print IR    output.o        execute records in order,
        (-g adds    (TargetMachine) "Evaluated to ..."
         DWARF)
```

Each scheme below is a general compiler-engineering idea first, then what v2
does with it; the numbered sections that follow give the full
before/after/tradeoffs.

**Input abstraction for the lexer (§1).** A lexer's job is turning
*characters* into tokens; where the characters come from — a file, a string,
a console — is a separate concern. Baking `getchar()` into the tokenizer
welds the compiler to process-global stdin, the classic testability mistake.
The standard fix is an abstract character source: v2's `Lexer` is abstract
over one pure virtual `readNextLine()`, with `LexerBuffer` reading a memory
range. Tests feed strings; the driver feeds a `MemoryBuffer`; a future REPL
would feed a prompt.

**Source locations (§1, §3).** Every serious compiler threads a
`{file, line, col}` through all stages, because both of its user interfaces
— error messages and debug info — are meaningless without one. v2's lexer
counts lines/columns in one place (`getNextChar()`), snapshots the position
at each token start, and every AST node carries the result. Chapter2–8 had
no locations at all; Chapter9 bolted them on with globals.

**Runtime type identification without RTTI (§2).** Consumers of a
heterogeneous tree constantly ask "which node kind is this?". C++'s answer
(`dynamic_cast`) is off the table — LLVM builds `-fno-rtti` — so LLVM's
idiom is a **kind enum set in the constructor plus a one-line `classof`**,
which unlocks the `llvm::isa<> / dyn_cast<> / cast<>` family. v2's AST
adopts it wholesale; it is what turns upstream's undefined-behavior
`static_cast` on `1 = 2` into a checked, located error.

**Separating the tree from its behaviors (§2, §3, §5).** Two ways to attach
operations to an AST: virtual methods on the nodes (upstream's
`codegen()` — convenient while one behavior evolves with the tree) or
**external traversals** over pure-data nodes (a walker per consumer). This
is the classic *expression problem* tradeoff, and compilers almost always
land on the second side, because many independent consumers walk one tree —
v2 already has two (the `ASTDumper` and the codegen `switch`), with zero IR
knowledge inside the nodes:

```
   upstream: behavior inside the tree      v2: pure data, external walkers
   ┌─────────────────────────────┐         ┌──────────────┐
   │ ExprAST                     │         │ ExprAST      │ kind, Location,
   │   virtual codegen(ctx) ─────┼──▶ IR   │ (data only)  │ getters, classof
   └─────────────────────────────┘         └──────┬───────┘
     ast.h must include IRBuilder,          ┌─────┴──────────┐
     PassBuilder, ORC JIT headers...   ASTDumper        CodeGenSession
                                       (AST.cpp)        (CodeGen.cpp) ──▶ IR
```

**One-directional layering (§4).** Phases should depend forward only:
parse → AST → codegen. Chapter6–9 broke this — the operator-precedence
table lived in the *codegen* context and was written by `codegen()`, so IR
generation retroactively changed how later source text parses. v2 moves the
table into the parser and registers user operators at prototype-parse time;
codegen never mutates grammar state:

```
   Chapter6–9                              ChapterA
   parser ──▶ codegen                       parser ──▶ codegen
     ▲            │  writes precedence        (grammar state stays left;
     └────────────┘  table in IRGenContext     IR state stays right)
```

**Encapsulation: facade + pImpl (§5).** A subsystem's header should show
its *contract*, not its machinery. Upstream's `IRGenContext` exposed ~14
public members (builder, pass managers, JIT, ...) that AST code and tests
mutated freely. v2's `CodeGen.h` is a five-method `CodeGenSession` holding a
pImpl — a compile-time firewall: no LLVM IR header leaks to any consumer,
and every invariant lives in one `.cpp`.

**Lexical scoping as a data structure (§5).** Nested scopes want a symbol
table that nests: bind on scope entry, auto-unbind on exit. Upstream
simulated this over a flat `std::map` with hand-rolled save/restore vectors
— and had two real bugs in the restore paths. v2 uses
`llvm::ScopedHashTable` with RAII scope objects, making "forgot to restore
on the error path" structurally impossible.

**A staged driver (§6).** Real compilers are *one* binary whose flags pick
how far the pipeline runs (`clang -E / -S / -c / -emit-llvm`); features
aren't added by forking the codebase. Chapter2–9 is per-chapter copies with
code surgery between them; v2 is `toyc -emit=ast|ir|obj|jit [-opt] [-g]`
over one code path — the chapter progression becomes flag combinations.

**Located, layered diagnostics (§7).** Each layer gets exactly one failure
channel (parser: `parseError<T>`, codegen: `emitError(loc, ...)`, driver:
exit code), every message carries a source location, and failure is
observable to tools — which is what lets error cases become lit tests
(`RUN: not %toy ...`). Upstream mixed null returns, release-mode-vanishing
`assert`s, and `exit()` from library code.

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
infrastructure problem in Chapter2–8: every parser/codegen/JIT test had to
write a fixed-name temp file into `$CWD` and `freopen()` it over `stdin` —
racy under `ctest -j`, impossible to undo, and incapable of running two
inputs in one process.

**After.** `Lexer` is an abstract class owning all tokenization logic, with
one pure virtual extension point, and `LexerBuffer` is the concrete
implementation over a `(begin, end)` memory range:

**`ChapterA/include/toy/Lexer.h`**
```cpp
class Lexer {
  ...
private:
  /// Delegate to a derived class fetching the next line. Returns an empty
  /// string to signal end of file (EOF).
  virtual llvm::StringRef readNextLine() = 0;
  ...
};

/// A lexer implementation operating on a buffer of memory.
class LexerBuffer final : public Lexer {
public:
  LexerBuffer(const char *begin, const char *end, std::string filename)
      : Lexer(std::move(filename)), current(begin), end(end) {}

private:
  llvm::StringRef readNextLine() override { /* slice one line off the range */ }
  const char *current, *end;
};
```

The driver reads the whole input once via
`llvm::MemoryBuffer::getFileOrSTDIN()` and hands the lexer the range. A test
is now just:

**`ChapterA/test/lexer_test.cpp`**
```cpp
LexerBuffer lexer(str.data(), str.data() + str.size(), "test.k");
```

The same extension point is what makes **location tracking** possible:
`getNextChar()` maintains line/column counters, and `getTok()` snapshots
them *before* consuming a token, so every token knows where it starts:

**`ChapterA/include/toy/Lexer.h`**
```cpp
  int getNextChar() {
    if (curLineBuffer.empty())
      return EOF;
    ++curCol;
    auto nextchar = curLineBuffer.front();
    curLineBuffer = curLineBuffer.drop_front();
    if (curLineBuffer.empty())
      curLineBuffer = readNextLine();
    if (nextchar == '\n') {
      ++curLineNum;
      curCol = 0;
    }
    return nextchar;
  }

  Token getTok() {
    while (isspace(lastChar))
      lastChar = Token(getNextChar());

    // Save the current location before reading the token characters.
    lastLocation.line = curLineNum;
    lastLocation.col = curCol;
    ...
```

The `Location` struct stores the filename as a `shared_ptr<std::string>` so
AST nodes can copy locations cheaply.

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
one-line `classof` enabler for `llvm::isa/dyn_cast/cast` — a complete node
now looks like:

**`ChapterA/include/toy/AST.h`**
```cpp
class NumberExprAST : public ExprAST {
  double val;

public:
  NumberExprAST(Location loc, double val)
      : ExprAST(Expr_Num, std::move(loc)), val(val) {}

  double getValue() const { return val; }

  /// LLVM-style RTTI: enables llvm::isa/dyn_cast/cast on ExprAST*.
  static bool classof(const ExprAST *c) { return c->getKind() == Expr_Num; }
};
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
`INDENT()` macro at the top of each method:

**`ChapterA/src/AST.cpp`**
```cpp
// Helper Macro to bump the indentation level and print the leading spaces
#define INDENT()                                                               \
  Indent level_(curIndent);                                                    \
  indent();

/// Dispatch to a generic expression to the appropriate subclass using RTTI
void ASTDumper::dump(ExprAST *expr) {
  llvm::TypeSwitch<ExprAST *>(expr)
      .Case<NumberExprAST, VariableExprAST, UnaryExprAST, BinaryExprAST,
            CallExprAST, IfExprAST, ForExprAST, VarExprAST>(
          [&](auto *node) { this->dump(node); })
      .Default([&](ExprAST *) {
        INDENT();
        llvm::errs() << "<unknown Expr, kind " << expr->getKind() << ">\n";
      });
}

void ASTDumper::dump(NumberExprAST *num) {
  INDENT();
  llvm::errs() << num->getValue() << " " << loc(num) << "\n";
}
```

Locations print as `@file:line:col`, which the FileCheck tests match with a
wildcarded directory but **exact** line/col — making the tests a regression
suite for the location plumbing itself.

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

**`ChapterA/include/toy/Parser.h`**
```cpp
  /// The explicit template parameter is the return type of the caller, so
  /// the call site reads as a drop-in `return parseError<ExprAST>(...)`.
  template <typename R, typename T, typename U = const char *>
  std::unique_ptr<R> parseError(T &&expected, U &&context = "") {
    auto curToken = lexer.getCurToken();
    llvm::errs() << "Parse error (" << lexer.getLastLocation().line << ", "
                 << lexer.getLastLocation().col << "): expected '" << expected
                 << "' " << context << " but has Token " << curToken;
    if (isprint(curToken))
      llvm::errs() << " '" << (char)curToken << "'";
    llvm::errs() << "\n";
    return nullptr;
  }
```

so a call site is a one-liner with location for free:

```cpp
return parseError<ExprAST>(")", "to close parenthesized expression");
// Parse error (12, 9): expected ')' to close parenthesized expression but has Token ';'
```

Contrast the old `logError("expected ')'")` with no location and three
near-identical helper functions.

**Precedence ownership — the real design fix.** In Chapter6–9, the
`binopPrecedence` map lived in `IRGenContext` (the codegen context!) and was
*written by* `FunctionAST::codegen` when a user-defined operator was
defined. IR generation retroactively changed how source text parses — a
genuine layering inversion, and the source of a crash chain (when codegen
failed, the precedence entry leaked, and the next use of the operator hit an
`assert`).

In v2 the parser owns the table and registers `def binary| 5 (a b) ...` **at
prototype-parse time**, before the body is parsed:

**`ChapterA/include/toy/Parser.h`**
```cpp
  /// definition ::= 'def' prototype expression
  std::unique_ptr<RecordAST> parseDefinition() {
    lexer.consume(tok_def);
    auto proto = parsePrototype();
    if (!proto)
      return nullptr;

    // A user-defined binary operator becomes part of the grammar as soon as
    // its prototype is parsed -- before the body, which may use it
    // recursively, and before any subsequent input.
    bool installedOp = false;
    if (proto->isBinaryOp()) {
      binopPrecedence[proto->getOperatorName()] =
          proto->getBinaryPrecedence();
      installedOp = true;
    }

    auto body = parseExpression();
    if (!body) {
      // The operator was never really defined; unregister it so later input
      // does not parse against a function that will not exist.
      if (installedOp)
        binopPrecedence.erase(proto->getOperatorName());
      return nullptr;
    }
    return std::make_unique<FunctionAST>(std::move(proto), std::move(body));
  }
```

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
includes**: a `CodeGenSession` with five methods and a pImpl. Everything
else — the emit overloads, the pass pipeline, debug info state — is private
to `src/CodeGen.cpp`:

**`ChapterA/include/toy/CodeGen.h`**
```cpp
class CodeGenSession {
public:
  explicit CodeGenSession(CodeGenOptions options);
  ~CodeGenSession();

  /// Generate IR for one module-level record. Returns the llvm::Function,
  /// or nullptr after reporting an error with its source location.
  llvm::Function *emitRecord(RecordAST &record);

  /// Access the module being populated (e.g. to print or emit object code).
  llvm::Module &currentModule();

  /// Finalize: complete debug info (if enabled) and run the verifier.
  bool finalize();

  /// For the JIT: hand off module+context as a ThreadSafeModule and start a
  /// fresh module. Dependent objects are torn down before the context moves.
  llvm::orc::ThreadSafeModule takeModule();

  void setDataLayout(const llvm::DataLayout &layout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
```

Inside the implementation, dispatch is a `switch` on the node kind with
`llvm::cast<>`, exactly like Toy's `MLIRGenImpl::mlirGen(ExprAST&)` — and
unlike the dumper's `TypeSwitch`, a `switch` on the enum gets `-Wswitch`
coverage when a new kind is added:

**`ChapterA/src/CodeGen.cpp`**
```cpp
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return emit(llvm::cast<NumberExprAST>(expr));
    case ExprAST::Expr_Var:
      return emit(llvm::cast<VariableExprAST>(expr));
    ...
    case ExprAST::Expr_VarDecl:
      return emit(llvm::cast<VarExprAST>(expr));
    }
    return emitError(expr.loc(), "unhandled expression kind");
```

The kind tags also pay off at the old UB site — assignment's LHS check is
now real:

**`ChapterA/src/CodeGen.cpp`**
```cpp
    // Assignment is special: the LHS is not emitted as an expression.
    if (bin.getOp() == '=') {
      // The destination must be a variable reference. dyn_cast (enabled by
      // the AST kind tags) makes this a checked error instead of UB.
      auto *lhs = llvm::dyn_cast<VariableExprAST>(bin.getLHS());
      if (!lhs)
        return emitError(bin.loc(), "destination of '=' must be a variable");
      ...
```

The symbol table is the headline improvement:

**`ChapterA/src/CodeGen.cpp`**
```cpp
llvm::ScopedHashTable<llvm::StringRef, llvm::AllocaInst *> symbolTable;
using SymbolTableScopeT =
    llvm::ScopedHashTableScope<llvm::StringRef, llvm::AllocaInst *>;

// per function / var-expr / for-loop body:
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
| a failing body `eraseFromParent()`'d the function even if earlier code already called it (dangling uses → use-after-free at teardown) | broken body dropped; the function is erased only when unreferenced, otherwise it reverts to a declaration (`error-broken-body.k`) |
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
`IRGenContext` grew/lost members. That is how Chapter8 shipped with test
suites dereferencing a JIT that was commented out (they crashed with
SIGSEGV).

**After.** One driver, one code path, behavior selected by flags declared
with `llvm::cl::opt` (the same command-line library every LLVM tool uses —
it generates `-help` for free):

**`ChapterA/src/toyc.cpp`**
```cpp
static cl::opt<std::string> inputFilename(cl::Positional,
                                          cl::desc("<input kaleidoscope file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

enum Action { None, DumpAST, DumpIR, EmitObj, RunJIT };

static cl::opt<enum Action> emitAction(
    "emit", cl::desc("Select the kind of output desired"),
    cl::values(clEnumValN(DumpAST, "ast", "output the AST dump"),
               clEnumValN(DumpIR, "ir", "output the LLVM IR"), ...));

static cl::opt<bool> enableOpt("opt", cl::desc("Enable optimizations"));
static cl::opt<bool> emitDebugInfo("g", cl::desc("Emit debug information"));
```

`-emit=ast` short-circuits before any LLVM target machinery is initialized
(the frontend has zero backend dependency, same as Toy). JIT mode walks the
module's records in source order: definitions each get their own module
(`takeModule()` per record) so they can be freed independently; top-level
expressions are compiled, executed, printed as `Evaluated to ...`, and their
resources released via a `ResourceTracker`:

**`ChapterA/src/toyc.cpp`**
```cpp
    if (!func->isTopLevelExpr()) {
      // A definition: move its module into the JIT and start a fresh one.
      if (auto err = jit->addModule(session.takeModule()))
        return reportErr(std::move(err));
      continue;
    }

    // A top-level expression: add, run, print, and free its module.
    auto rt = jit->getMainJITDylib().createResourceTracker();
    if (auto err = jit->addModule(session.takeModule(), rt))
      return reportErr(std::move(err));

    auto symOrErr = jit->lookup(name);
    if (!symOrErr)
      return reportErr(symOrErr.takeError());

    double (*fp)() = symOrErr->getAddress().toPtr<double (*)()>();
    llvm::outs() << "Evaluated to " << llvm::format("%f", fp()) << "\n";

    if (auto err = rt->remove())
      return reportErr(std::move(err));
```

* **Advantages:** no more per-chapter code surgery, so no more
  "tests written for a feature this directory just removed"; every stage is
  exercised by the same frontend; adding a stage is adding an enum value and
  a case.
* **Disadvantages:** a single binary carries all stages, so even `-emit=ast`
  links against the ORC JIT (larger binary, slower link). The Toy tutorial's
  per-chapter binaries are better as a *teaching* progression — you can diff
  Ch4 against Ch5 to see exactly what a feature costs. v2 gives up that
  pedagogical diffability; the per-chapter story lives in `Chapter2–9`
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

Compared to the Chapter2–9 suites, the coverage that previously required
fragile substring matching (instruction counting loops, float formatting,
block-label greps) is expressed directly; and things that were *untestable*
before — precedence from the dump, error locations, `1 = 2` — have tests.
What was lost: the white-box gtest codegen tests (see §5's disadvantage) and
JIT-execution tests as C++ (`EXPECT_DOUBLE_EQ(fp(), 6.0)`) — v2 checks
executed values through the printed `Evaluated to` text instead, which is
one step less precise (fixed 6-digit formatting) but covers the same
behavior end-to-end.

Running the tests (lit mechanics are the same as the Chapter2–9 dirs — see
the [top-level README](../README.md#testing-the-two-schemes); `%toy` resolves
via `TOY_BIN`, defaulting to `./build/toyc`):

```sh
ctest --test-dir build                    # everything: gtest lexer suite + lit
ctest --test-dir build -R filecheck       # just the lit/FileCheck suite
./build/lexer_test                        # the gtest binary directly

lit -v test/filecheck                     # whole lit suite by hand
lit -v test/filecheck/ast.k               # one test (AST shape + locations)
lit -v test/filecheck/error-parse.k       # error tests: RUN lines use `not %toy`
```

And the stages themselves, by hand:

```sh
./build/toyc test/filecheck/ast.k -emit=ast          # dump AST of a test input
echo 'def f(x) x+1; f(41);' | ./build/toyc -emit=jit -opt   # Evaluated to 42.000000
echo '1 = 2;' | ./build/toyc -emit=ir; echo "exit=$?"       # located error, non-zero exit
```

---

## Known limitations / future work

- **No interactive REPL** (see §1). Add a `LexerStdin` subclass and a
  record-at-a-time driver loop if wanted.
- **No error recovery** in the parser; first error aborts (§7).
- **`-g` is only supported with `-emit=ir`/`-emit=obj`** (single-module
  mode); JIT mode ignores it (and says so with a warning), like upstream —
  which never combined the JIT with debug info either.
- Numbers still have no exponent syntax (`1.5e3` lexes as `1.5`, `e3`) —
  kept for language fidelity; the lexer test documents it.
- v2 requires `def`-before-use across records for *calls at JIT time*
  (records are processed in source order), same as the upstream REPL.
