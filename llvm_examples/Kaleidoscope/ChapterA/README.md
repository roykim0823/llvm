# Chapter A — Kaleidoscope, Redesigned (MLIR Toy style)

A single-codebase reimplementation of the LLVM Kaleidoscope tutorial
(Chapters 2–9), applying the frontend design of the
[MLIR Toy tutorial](https://mlir.llvm.org/docs/Tutorials/Toy/) to a compiler
that targets LLVM IR — and then taking it two steps further than the Toy
tutorial does, with a diagnostic engine and a semantic-analysis pass.

`Chapter2`–`Chapter9` (siblings of this directory) follow the upstream
tutorial chapter by chapter, keeping its behavior while giving it a real
compiler's structure. This directory is the "what it looks like when
designed from scratch" counterpart. The chapter progression is replaced by
**one binary with staged actions**:

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

Tests: `cd build && ctest` runs two gtest suites (lexer, sema) plus a
lit/FileCheck suite (`test/filecheck/*.k`).

The *language* is Chapter7-era Kaleidoscope with two deliberate lexer-level
fixes (both pinned by lexer tests): identifiers may contain `_`
(`[a-zA-Z][a-zA-Z0-9_]*` — the `my_var`-lexes-as-three-tokens gotcha of the
chapter track is gone), and a second `.` terminates a number (`3.14.15`
lexes as `3.14`, `.`, `15` instead of silently swallowing the whole string
as `3.14`). Everything else is a compiler-engineering scheme.

## The design, concept by concept

The pipeline first — one binary, and each `-emit=` flag is a tap into it at
a different depth. Two things distinguish it from the chapter track at a
glance: a **semantic-analysis stage** between parsing and IR generation, and
a **diagnostic engine** every stage reports into:

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
        print IR    output.o        execute records in order,
        (-g adds    (TargetMachine) "Evaluated to ..."
         DWARF)
```

Each scheme below is a general compiler-engineering idea first, then what
this design does with it; the numbered sections that follow give the full
detail and tradeoffs.

**Input abstraction for the lexer (§1).** A lexer's job is turning
*characters* into tokens; where the characters come from — a file, a string,
a console — is a separate concern. Baking `getchar()` into the tokenizer, as
the tutorial does, welds the compiler to process-global stdin: the chapter
track's tests have to write temp files and `freopen()` them over stdin. The
standard fix is an abstract character source: the `Lexer` here is abstract
over one pure virtual `readNextLine()`, with `LexerBuffer` reading a memory
range. Tests feed strings; the driver feeds a `MemoryBuffer`; a future REPL
would feed a prompt.

**Source locations (§1, §3).** Every serious compiler threads a
`{file, line, col}` through all stages, because both of its user interfaces
— error messages and debug info — are meaningless without one. The lexer
counts lines and columns in one place (`getNextChar()`), snapshots the
position at each token start, and every AST node carries the result,
filename included. The chapter track gets locations only in Chapter9, line
and column but no file, and uses them for debug info alone.

**Runtime type identification without RTTI (§2).** Consumers of a
heterogeneous tree constantly ask "which node kind is this?". C++'s answer
(`dynamic_cast`) is off the table — LLVM builds `-fno-rtti` — so LLVM's
idiom is a **kind enum set in the constructor plus a one-line `classof`**,
which unlocks the `llvm::isa<> / dyn_cast<> / cast<>` family. The AST
adopts it wholesale (the chapter track adopted the same idiom from Chapter2
on).

**Separating the tree from its behaviors (§2, §3).** Two ways to attach
operations to an AST: virtual methods on the nodes (the tutorial's
`codegen()`) or **external traversals** over pure-data nodes. This is the
classic *expression problem* tradeoff, and compilers almost always land on
the second side, because many independent consumers walk one tree. Here
there are three — the `ASTDumper`, the semantic resolver, and IR
generation — with zero IR knowledge inside the nodes.

**One-directional layering (§4).** Phases should depend forward only:
parse → AST → sema → codegen. The operator-precedence table lives in the
parser and user operators are registered at prototype-parse time, so IR
generation never mutates grammar state (upstream writes the table from
`codegen()`).

**Diagnostics as data (§5).** Every real compiler separates *reporting* a
problem from *rendering* it: clang has `DiagnosticsEngine`, MLIR has
`DiagnosticEngine`, and both exist so that errors can carry structure
(severity, location, message), be counted, be intercepted by an embedder,
and be tested without scraping stderr. The `DiagnosticEngine` here is the
plain-LLVM miniature: every stage reports into one engine, the default
handler prints clang-style `file:line:col: error: message`, and the unit
tests install a silent handler and assert on the collected list.

**Error recovery (§4).** A compiler that stops at the first error makes the
user fix N problems in N runs. The classic fix is *panic-mode recovery*:
after an error, skip to a synchronization token and continue. The parser
resynchronizes at record boundaries (`;`, `def`, `extern`), and sema simply
keeps walking after each error — so one run reports every problem, ending
with a clang-style `N errors generated.` summary:

```
$ toyc broken.k -emit=ir
broken.k:2:12: error: incorrect number of arguments to 'f': expected 2, got 1
broken.k:2:19: error: unknown variable 'y'
broken.k:3:3: error: destination of '=' must be a variable
3 errors generated.
```

**Sema is a resolver, not just a checker (§6) — the headline.** Resolving
names *while emitting IR* (the tutorial's way, and the chapter track's)
forces stop-at-first-error — you cannot emit IR for a broken expression —
and makes the checks untestable without LLVM. The naive fix is a checking
pass in front of codegen, but that *duplicates* scope resolution: two
symbol tables, kept consistent only by discipline. This design does what
clang's Sema actually does: semantic analysis **records what it learns**.
Its output, a `Resolutions` table, binds every variable use to the
declaration that introduced it (a dense `VarId`; shadowing resolved here,
once) and every call site to its `PrototypeAST`.

**...which makes IR emission infallible (§7).** The backend consumes
bindings instead of re-resolving: variable uses index a plain
`std::vector<AllocaInst*>` by `VarId` — no symbol table, no scopes, no
"unknown variable" paths, and since arity and operators were checked too,
**emitting an expression cannot fail at all**. Error propagation, its
null-checks, and error-path cleanup all disappear from IRGen; the only
failure left is a verifier rejection, which is a compiler bug, not user
input. (clang's CodeGen makes the same assumption about Sema's output.)

**Encapsulation: facade + pImpl, one component per job (§8).** A
subsystem's header should show its *contract*, not its machinery.
`CodeGen.h` is a five-method `CodeGenSession` holding a pImpl — a
compile-time firewall: no LLVM IR header leaks to any consumer. Behind it
the work is split along its seams: `IRGen` (AST→IR only), `Optimizer` (the
pass pipeline), and `DebugInfoEmitter` (DWARF as a null-object, so IRGen
has *zero* debug-info conditionals), composed by a thin facade.

**A staged driver (§9).** Real compilers are *one* binary whose flags pick
how far the pipeline runs (`clang -E / -S / -c / -emit-llvm`); features
aren't added by forking the codebase. The chapter track is per-chapter
copies; this is `toyc -emit=ast|ir|obj|jit [-opt] [-g]` over one code path
— the chapter progression becomes flag combinations.

## Layout

```
include/toy/Lexer.h        header-only: Location, Token, Lexer (abstract), LexerBuffer
include/toy/AST.h          pure-data AST: kind enum + classof + Location per node
include/toy/Diagnostics.h  header-only: Diagnostic, DiagnosticEngine
include/toy/Parser.h       header-only recursive descent; reports via the engine, record-level recovery
include/toy/Sema.h         Resolutions + one free function, resolveModule()
include/toy/CodeGen.h      facade: CodeGenSession (pImpl), no LLVM IR headers
src/AST.cpp                ASTDumper (anonymous namespace) behind one free function
src/Sema.cpp               the Resolver, in an anonymous namespace
src/IRGen.h/.cpp           AST→IR consuming Resolutions; no symbol table
src/Optimizer.h/.cpp       the pass pipeline (mem2reg..SimplifyCFG)
src/DebugInfo.h/.cpp       DWARF as a null-object behind hook methods
src/CodeGen.cpp            the facade Impl: a thin composer of the three above
src/toyc.cpp               driver: cl::opt, staged pipeline, sema gate, JIT loop
src/extern_d.cpp           putchard/printd runtime functions for the JIT
test/lexer_test.cpp        gtest: lexer fed from plain strings
test/sema_test.cpp         gtest: diagnostics, recovery, bindings — no IR
test/filecheck/*.k         lit + FileCheck end-to-end tests
```

This mirrors the Toy tutorial's rule — **Lexer and Parser are header-only;
AST and code generation split declaration/implementation, with the
implementation hidden in an anonymous namespace (or behind a pImpl) and a
minimal public API** — plus one rule of its own: `IRGen.h`, `Optimizer.h`
and `DebugInfo.h` live in `src/`, not `include/toy/`. They are
implementation detail of the backend, free to include LLVM IR headers
because nothing outside `src/` can reach them. The compile-time firewall of
the facade is preserved.

---

## The applied patterns, in detail

### 1. Buffer-based lexer (`readNextLine()` / `LexerBuffer`)

**The problem.** The tutorial's `gettok()` calls `getchar()` directly,
hardwiring the lexer to process-global `stdin`. In the chapter track this
is kept deliberately (it is upstream's lexer), and it is the worst test
infrastructure problem there: every parser/codegen/JIT test writes a
pid-suffixed temp file into `$CWD` and `freopen()`s it over `stdin` — a
one-way operation, and incapable of running two inputs in one process.

**The design.** `Lexer` is an abstract class owning all tokenization logic,
with one pure virtual extension point, and `LexerBuffer` is the concrete
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
  tests; real source locations with a file name (which debug info, every
  diagnostic, and the `-emit=ast` dump need); multiple inputs per process.
* **Disadvantages:** the interactive REPL is gone — the whole input is
  parsed before anything happens, so you don't get a `ready>` prompt with
  incremental feedback. (A `LexerStdin` subclass implementing
  `readNextLine()` from a prompt would restore it; that is precisely what
  the abstraction is for, but it is not written.) Also, the whole input must
  fit in memory — irrelevant here, but a real difference from a streaming
  lexer.

### 2. AST with kind tags + `classof` (LLVM-style RTTI), locations, pure data

Every node carries a `const ExprASTKind kind` set at construction, plus a
`Location`, and exposes getters. Each class has the one-line `classof`
enabler for `llvm::isa/dyn_cast/cast` — a complete node looks like:

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

The nodes have **no virtual methods except the destructor**; dumping,
resolving and IR generation live outside the tree. Module-level items
(`FunctionAST`, `ExternAST`) form a second small hierarchy under
`RecordAST` (the Toy Ch7 pattern), so `ModuleAST` is one ordered,
heterogeneous list — order matters for JIT semantics and for operator
definitions.

Contrast with upstream, whose nodes have a virtual `codegen()` and private
members with *zero accessors*: `ast.h` has to include the IR builder, the
parse tree is write-only (parser tests can only assert "not null"), and
runtime type questions are impossible (`'='`'s destination check is an
unchecked `static_cast`, so `1 = 2` is undefined behavior). The payoff of
the pure-data tree shows up in three places:

- `AST.h` includes only `Lexer.h` (for `Location`) and LLVM ADT headers —
  no IR headers anywhere near the frontend.
- The kind tags make `1 = 2` a *checked* error — here in sema, with a
  location (`test/filecheck/error-assign.k`).
- The dumper makes the parse tree observable, so precedence and
  associativity are directly testable (`test/filecheck/ast.k`).

* **Advantages:** decoupled layers, safe downcasts without C++ RTTI, testable
  parser output, locations available to every later stage.
* **Disadvantages:** more boilerplate per node (kind enum entry, `classof`,
  getters, location plumbing through every constructor), and adding a node
  type now requires touching *four* places (AST.h, the dumper, the resolver,
  the IRGen switch) instead of one class. The virtual-method design has
  genuinely better locality when the tree and its behaviors evolve together;
  the external-traversal design wins when multiple independent consumers
  walk the same tree. A compiler almost always ends up in the second
  situation.

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
suite for the location plumbing itself. `-emit=ast` runs the dumper on the
parser's output directly, before sema: undeclared names dump fine, and the
frontend touches zero LLVM target machinery.

* **Advantages:** the dump format is a stable, greppable contract; zero cost
  in the AST classes; trivially replaceable (e.g. by a JSON dumper) without
  touching the tree.
* **Disadvantages:** the `.Case<...>` list and the kind enum must be kept in
  sync by hand; a forgotten case falls into the `<unknown Expr>` default
  rather than a compile error. (A `switch` on the enum — as IRGen does —
  gets `-Wswitch` coverage instead; the dumper trades that for terser code.)

### 4. Parser: recovery at record boundaries, and parser-owned precedence

**Reporting and recovery.** Every failure path reports through the
`DiagnosticEngine` (§5) and then *resynchronizes* instead of giving up. The
synchronization points are the natural record boundaries:

**`ChapterA/include/toy/Parser.h`**
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
**always returns a module** — possibly partial — and callers consult the
engine for success (the driver: `if (diags.hadError()) return ...`). The
comment in the snippet is the progress argument: recovery must guarantee the
loop advances, or a bad token at a boundary loops forever. Messages render
tokens the way the user wrote them (`describeCurToken()`): keywords as
`'def'`, identifiers as `identifier 'foo'`, numbers as `a number`,
punctuation as `';'`.

**Precedence ownership.** Upstream keeps the `binopPrecedence` map in the
codegen globals and writes it from `FunctionAST::codegen()` when a
user-defined operator is defined — IR generation retroactively changes how
source text parses, a layering inversion. Here the parser owns the table and
registers `def binary| 5 (a b) ...` **at prototype-parse time**, before the
body is parsed:

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

Subsequent input — including the operator's own body, so it can be
recursive — parses with the right precedence; if the body fails to parse,
the parser unregisters the operator; codegen never mutates grammar state.
(The chapter track's parser does the same from Chapter6 on, with one
refinement — a failed *re*definition restores the previous precedence — that
is worth carrying here too.)

* **Advantages:** all parse errors in one run; a partial AST survives for
  tooling that wants it; recovery is ~15 lines because record boundaries in
  this grammar are unambiguous; one-directional dependency, and
  whole-module parsing becomes possible (which the staged driver needs).
* **Disadvantages:** panic-mode is crude — everything between the error and
  the boundary is skipped unexamined, so an error *inside* a `def` hides any
  further errors in that same `def`. One upstream quirk limits where errors
  land: `parseUnary` treats any ASCII token as a candidate unary operator,
  so in `x + ;` the `;` is consumed as an "operator" and the error is
  reported at whatever follows (see the note in `test/sema_test.cpp`).
  Fixing that would mean whitelisting operator characters — a language
  change, deliberately not made. And "the parser does a little semantic
  work" (registering operators) slightly blurs the Toy tutorial's "no
  semantic checks in the parser" rule; it is the price of user-extensible
  grammar.

### 5. DiagnosticEngine: severity + location + message, handler-based

A diagnostic is a struct; the engine stores every one, counts errors, and
forwards each to a handler:

**`ChapterA/include/toy/Diagnostics.h`**
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

The engine is also what makes **white-box error testing** possible. A unit
test installs a silent handler and asserts on severity, message, and *exact*
location — no subprocess, no LLVM IR:

**`ChapterA/test/sema_test.cpp`**
```cpp
TEST(SemaTest, UnknownVariable) {
  FrontendRun run("def f(x) y;");
  ASSERT_EQ(run.diags.errorCount(), 1u);
  EXPECT_EQ(run.diag(0).message, "unknown variable 'y'");
  EXPECT_EQ(run.diag(0).loc.line, 1);
  EXPECT_EQ(run.diag(0).loc.col, 10);
}
```

Contrast the tutorial's three near-identical `LogError*` helpers printing
`Error: ...` with no location, and its `assert`s that vanish in release
builds; the chapter track keeps those, which is why its error tests are
FileCheck matches on stderr.

* **Advantages:** errors are countable, interceptable, and testable as
  values; one rendering convention across all stages; warnings and notes
  become possible at all.
* **Disadvantages:** every stage carries a `DiagnosticEngine&` (constructor
  plumbing), and messages built eagerly into `std::string`s cost a little
  even when a handler would discard them. One sharp edge: a `llvm::Twine`
  must never be stored across statements (it references its operands), so
  `parseError` builds `std::string` eagerly — the same rule LLVM's own style
  guide states.

### 6. Sema resolves; Resolutions is its output

Resolving names inside IR generation — a scoped symbol table of allocas for
variables, a name→prototype registry for calls, checks for unknown names,
arity, redefinition and the `'='` rule inline in the emit methods — has
three consequences: the first error necessarily aborts the run, the checks
are untestable without constructing LLVM IR, and one class of bug is
diagnosed misleadingly (`extern f(x); def f(a b)` generates IR against the
*old* arity and then fails inside the body with `unknown variable 'b'`).

Here one scoped walk in `Sema.cpp` does two jobs: **check** (reporting every
error, recovering after each) and **resolve** (recording what the walk
learned). The result is a side table:

**`ChapterA/include/toy/Sema.h`**
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

Scoping lives here, and *only* here: `ScopedHashTable<StringRef, VarId>`
with one RAII scope per function body / for-loop / var-expr, initializers
resolved before their own name is inserted. Shadowing is decided at resolve
time by handing each declaration a fresh `VarId`:

**`ChapterA/src/Sema.cpp`**
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
lowers to a new, resolved tree). The side table was chosen deliberately: the
AST stays pure parse output (§2's principle survives — nodes never change
after parsing, and `-emit=ast` dumps exactly what the parser saw), at the
price of a `DenseMap` lookup per use and a lifetime rule (the `Resolutions`
must accompany the ModuleAST wherever the backend goes). For in-place
annotation the trade reverses: no side structure to carry, but nodes gain a
mutable "resolved" slot and every consumer must know whether resolution has
run yet. Both are legitimate; what matters is that resolution happens
*once*.

Sema also diagnoses two things inline codegen checks cannot say clearly:
the arity-conflict case above is
`conflicting declaration of 'f': 2 parameters, previously declared with 1`
plus a `note: previous declaration is here` (`error-redefine.k`), and a
duplicate parameter (`def f(x x)`) gets a *warning*. One check is
unreachable from source text and the code says so: an *unknown binary
operator* cannot survive to sema, because an unregistered operator token
never parses as a binary op in the first place (the parser owns the
precedence table — §4 paying off again).

* **Advantages:** all semantic errors in one run; scoping implemented
  exactly once; testable without LLVM IR (the whole `sema_test.cpp` suite,
  bindings included, runs in milliseconds); better messages for declaration
  conflicts.
* **Disadvantages:** the side table is address-keyed, so it silently pairs
  only with the ModuleAST it was computed from — hand the backend a
  mismatched pair and the asserts fire (debug) or bindings dangle
  (release). A `(ModuleAST, Resolutions)` wrapper type would make the
  pairing unforgeable; at this scale the constructor comment carries it.

### 7. IRGen consumes bindings — and becomes infallible

This is the payoff of §6, visible as *deleted* code. The backend requires a
clean `Resolutions` and trusts it, exactly as clang's CodeGen trusts Sema:

**`ChapterA/src/IRGen.h`**
```cpp
  /// Storage for every declared variable, indexed by VarId. A plain vector
  /// replaces a scoped symbol table: resolution already decided which
  /// declaration each use refers to, so there is nothing left to scope.
  std::vector<llvm::AllocaInst *> allocas;
```

**`ChapterA/src/IRGen.cpp`**
```cpp
llvm::Value *IRGen::emit(VariableExprAST &var) {
  llvm::AllocaInst *alloca = allocas[resolutions.boundVariable(var)];
  assert(alloca && "use emitted before its resolved declaration");
  return builder.CreateLoad(alloca->getAllocatedType(), alloca,
                            var.getName());
}
```

What that deletes, compared with an IR generator that resolves names itself
(the tutorial's, and this codebase's own first iteration — see
[Design history](#design-history)):

| Resolving inside codegen needs | IRGen here |
|---|---|
| a scoped symbol table (`ScopedHashTable` + RAII scope per function/loop/var-expr) | a vector indexed by `VarId`; shadowing already resolved |
| "insert the loop variable only *after* emitting the start expression" ordering rule | gone — the start's uses bind to the *outer* VarId, so publishing the alloca early is harmless |
| `unknown variable / function / operator`, arity, `'='` dyn_cast checks | gone (sema's job); contract violations are asserts |
| null-propagation on every emit call | gone — `emitExpr` cannot fail |
| cleanup for blocks leaked on error paths in if/then/else | gone — there are no error paths to leak on |
| a name→prototype registry kept by the session for JIT-mode redeclaration | gone — each call site carries its `PrototypeAST*`; the Resolutions table *is* the cross-module registry |

The one failure that remains is real: `verifyFunction` after emission, which
now means a compiler bug and is reported as
`internal error: function 'f' failed verification`
(`discardBrokenFunction` handles it without dangling call sites: a broken
body is dropped, and the function is erased only when unreferenced,
otherwise it reverts to a declaration).

Dispatch is a `switch` on the node kind with `llvm::cast<>`, exactly like
Toy's `MLIRGenImpl::mlirGen(ExprAST&)` — and unlike the dumper's
`TypeSwitch`, a `switch` on the enum gets `-Wswitch` coverage when a new kind
is added.

* **Advantages:** IRGen shrinks to the actual translation logic; a whole
  class of "checker and emitter disagree about scope" bugs becomes
  impossible; the alloca vector is O(1) per use with no hashing.
* **Disadvantages:** IRGen is unusable without a prior resolve — embedders
  lose the "just emit this AST" shortcut (that is the contract working as
  intended, but it is a real constraint); and `assert`-based contract
  enforcement vanishes in release builds, leaning on the debug-build test
  suite to catch misuse.

### 8. The backend behind a facade: pImpl session, three components

`include/toy/CodeGen.h` is a small facade with **no LLVM IR includes** — a
`CodeGenSession` with five methods and a pImpl:

**`ChapterA/include/toy/CodeGen.h`**
```cpp
struct CodeGenOptions {
  bool optimize = false;      ///< run the per-function pass pipeline
  bool emitDebugInfo = false; ///< attach DWARF debug info (single-module mode)
  std::string sourceFile = "<stdin>"; ///< filename for the debug compile unit
};

class CodeGenSession {
public:
  CodeGenSession(CodeGenOptions options, const Resolutions &resolutions,
                 DiagnosticEngine &diags);
  ~CodeGenSession();

  /// Generate IR for one module-level record (function definition, anonymous
  /// top-level expression, or extern declaration) into the current module.
  /// Returns the generated llvm::Function, or nullptr after reporting an
  /// internal error through the DiagnosticEngine.
  llvm::Function *emitRecord(RecordAST &record);

  /// Access the module being populated (e.g. to print or emit object code).
  llvm::Module &currentModule();

  /// Finalize the current module: complete debug info (if enabled) and run
  /// the LLVM verifier. Returns false if verification fails.
  bool finalize();

  /// For the JIT: hand off the current module and its context as a
  /// ThreadSafeModule and start a fresh module. The components referencing
  /// the context (IRGen's builder, the optimizer's instrumentation, the
  /// DIBuilder) are torn down before the context moves, so nothing dangles.
  llvm::orc::ThreadSafeModule takeModule();

  /// Set the data layout applied to the current and every future module
  /// (from the JIT or a TargetMachine).
  void setDataLayout(const llvm::DataLayout &layout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
```

Behind it, the work is split into injected components, one job each.
`IRGen` translates; `Optimizer` owns the pass managers (its member
*declaration order* encodes the teardown-order invariant; see the comment
in `Optimizer.h`); `DebugInfoEmitter` is a **null-object** — default-
constructed it is disabled and every hook is a no-op, so IRGen calls the
hooks unconditionally and contains not a single debug-info conditional:

**`ChapterA/src/IRGen.cpp`**
```cpp
  llvm::BasicBlock *bb = llvm::BasicBlock::Create(context, "entry", fn);
  builder.SetInsertPoint(bb);
  debug.functionBegin(proto, *fn, builder);   // no-op without -g
```

The trickiest invariant in a JIT-fed session — tear down everything
referencing the context *before* moving it into a `ThreadSafeModule` — is
one line per component:

**`ChapterA/src/CodeGen.cpp`**
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

* **Advantages:** compile-time firewall (touching backend internals rebuilds
  files under `src/`; consumers of the facade never see LLVM IR headers),
  each component small enough to read in one sitting, and a JIT hand-off
  without use-after-free hazards.
* **Disadvantages:** the pImpl adds indirection — tests cannot poke the
  builder or the pass managers, which is why the backend is tested end to
  end through FileCheck rather than white-box (the chapter track keeps
  white-box codegen tests through its own facade's return values). One
  lifetime rule is implicit: the session holds the `Resolutions` and the
  `ModuleAST` by reference, so **both must outlive the session** (the driver
  guarantees this; the header documents it).

### 9. Staged driver (`cl::opt`, ordered `Action` enum, sema gate)

One driver, one code path, behavior selected by flags declared with
`llvm::cl::opt` (the same command-line library every LLVM tool uses — it
generates `-help` for free):

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

The enum ordering is load-bearing: "emit stage N" means "run everything up
to N". `-emit=ast` short-circuits before any LLVM target machinery is
initialized (the frontend has zero backend dependency, same as Toy). Sema
gates every backend action, and every failing stage ends with the
clang-style summary:

**`ChapterA/src/toyc.cpp`**
```cpp
  // Semantic analysis gates every backend stage (all errors in one run)
  // and produces the name resolution the backend consumes.
  Resolutions resolutions = resolveModule(*moduleAST, diags);
  if (diags.hadError())
    return reportErrors(diags);          // "N errors generated."
```

JIT mode walks the module's records in source order: definitions each get
their own module (`takeModule()` per record) so they can be freed
independently; top-level expressions are compiled, executed, printed as
`Evaluated to ...`, and their resources released via a `ResourceTracker`:

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

* **Advantages:** no per-chapter code surgery, so no "tests written for a
  feature this directory just removed"; every stage is exercised by the same
  frontend; adding a stage is adding an enum value and a case.
* **Disadvantages:** a single binary carries all stages, so even `-emit=ast`
  links against the ORC JIT (larger binary, slower link). The Toy tutorial's
  per-chapter binaries are better as a *teaching* progression — you can diff
  Ch4 against Ch5 to see exactly what a feature costs. This design gives up
  that pedagogical diffability; the per-chapter story lives in `Chapter2–9`
  instead, which is why both trees exist.

### 10. Testing strategy: FileCheck for shape, gtest for the rest

The test pyramid follows what each tool measures well:

- **gtest** `lexer_test` — token-level facts and *locations*, fed from
  strings. No `freopen`, no temp files, safe under `ctest -j`.
- **gtest** `sema_test` — the `DiagnosticEngine` as a test fixture. Clean
  programs, every sema error with exact locations, warnings vs errors,
  notes, scoping edge cases (`for`-variable leaving scope, `var` initializer
  resolution), parser recovery counts, message spelling — and the resolver's
  output itself (shadowing yields distinct `VarId`s; every declaration form
  gets one).
- **lit + FileCheck** (`test/filecheck/*.k`) — everything observable from
  the outside. Success paths: AST structure and precedence (`ast.k`), raw
  and optimized IR shape with def-use captures like `[[MUL:%.*]]` (`ir.k`,
  `opt.k`, `controlflow.k`, `userops.k`, `mutablevars.k`), executed values
  (`jit.k`), object emission (`objfile.k`), debug metadata (`debuginfo.k`).
  Error paths, written as `RUN: not %toy ...`: `error-parse.k` (two parse
  errors, one run), `error-multiple.k` (three sema errors + summary),
  `error-redefine.k` (error + note), `error-arity.k`, `error-assign.k`,
  `error-undef-var.k`, `error-unknown-op.k`, and `warn-dup-param.k`
  (warning, exit 0).

Running the tests (lit mechanics are the same as the Chapter2–9 dirs — see
the [top-level README](../README.md#testing-the-two-schemes); `%toy` resolves
via `TOY_BIN`, defaulting to `./build/toyc`):

```sh
ctest --test-dir build                    # everything: both gtest suites + lit
ctest --test-dir build -R filecheck       # just the lit/FileCheck suite
./build/sema_test                         # the sema suite directly

lit -v test/filecheck                     # whole lit suite by hand
lit -v test/filecheck/ast.k               # one test (AST shape + locations)
lit -v test/filecheck/error-multiple.k    # the headline error demo
```

And the stages themselves, by hand:

```sh
./build/toyc test/filecheck/ast.k -emit=ast          # dump AST of a test input
echo 'def f(x) x+1; f(41);' | ./build/toyc -emit=jit -opt   # Evaluated to 42.000000
echo '1 = 2;' | ./build/toyc -emit=ir; echo "exit=$?"       # located error, non-zero exit
```

---

## Design history

This directory is the result of two design iterations, and the second one
is worth recording because it is the most instructive part.

The first iteration built §§1–4, 8 and 9 as they stand — buffer lexer,
pure-data AST, external dumper, parser-owned precedence, pImpl facade,
staged driver — and put *everything else* into one codegen `Impl`: a
`ScopedHashTable` symbol table with RAII scopes, inline checks for unknown
names, arity and the `'='` destination, a name→prototype registry for
JIT-mode redeclaration, the pass pipeline, and the DWARF state. Errors were
"print `Codegen error (line, col): ...` to stderr and return null". It
worked, and it fixed real upstream bugs (the `static_cast` UB on `1 = 2`,
the release-mode `assert` on unknown operators, a use-after-free when a
failing body erased a function that earlier code already called). Its own
README named two weaknesses: error handling with no engine, no recovery and
no way for an embedder to intercept it; and an `Impl` of some 570 lines
carrying four jobs, one of them a complete name-resolution walk.

The second iteration addressed exactly those two, and the fix for the
second turned out to be the fix for the first. Once name resolution moved
into its own pass and *recorded* its bindings (§6), the IR generator had
nothing left to check and therefore no way to fail (§7); with no user
errors reaching the backend, a diagnostic engine (§5) and parser recovery
(§4) could report every error in one run without ever having to unwind a
half-built module. The remaining `Impl` split into three components (§8),
and the facade kept its five-method shape — the driver barely noticed.

The success-path lit suites were carried across the second iteration
unchanged: the IR and the executed values did not move, which is the
regression proof that consuming bindings emits the same code the scoped
symbol table did. One test had no successor: the first iteration's
`error-broken-body.k` (a failing body must not erase a function that
earlier code already calls) — sema now rejects such a module before IR
generation begins, and expression emission cannot fail, so the only path
into `discardBrokenFunction` is a verifier rejection, which no source input
triggers. The hardening remains for that internal-error path.

Against the tutorial track in `Chapter2–9`, which has since adopted the
first iteration's codegen architecture chapter by chapter (kind-tagged AST,
pImpl facade, parser-owned precedence, a driver as composition root), what
remains distinctive here is: the buffer lexer with file names in every
location, whole-module parsing and the staged driver, the diagnostic engine
with recovery, and the sema pass that makes the backend infallible.

## Known limitations / future work

- **No interactive REPL** (see §1). Add a `LexerStdin` subclass and a
  record-at-a-time driver loop if wanted.
- **Recovery granularity is the record.** An error inside a `def` hides
  later errors in the same `def`; statement-level synchronization points
  don't exist in this grammar (§4).
- **The (ModuleAST, Resolutions) pairing is by convention.** The side table
  is address-keyed; nothing but the documented contract stops a caller
  handing the backend a table computed from a different tree. A wrapper
  type would make it unforgeable (§6).
- **Diagnostics still render eagerly** and hold no source *ranges* — a
  clang-style caret line would need the lexer to keep the current line text.
- **`-g` is only supported with `-emit=ir`/`-emit=obj`** (single-module
  mode); JIT mode ignores it (and says so with a warning), like upstream —
  which never combined the JIT with debug info either.
- Numbers still have no exponent syntax (`1.5e3` lexes as `1.5`, `e3`) —
  kept for language fidelity; the lexer test documents it.
- `def`-before-use across records is required for *calls at JIT time*
  (records are processed in source order), same as the upstream REPL.
