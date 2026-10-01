# Chapter 9 — Adding Debug Information

The last chapter teaches compiled Kaleidoscope programs to be **debugged at
the source level** — set a breakpoint on `fib`, step by line, print `x`. Two
general concepts first:

**Debug information.** A debugger works on machine state — a program counter
and memory. Source-level debugging needs a *map* from that state back to the
source: which file/line does this PC correspond to, which stack slot holds
the variable named `x`, what scope is active. Compilers emit this map as
**DWARF** (the standard debug format on Unix-family systems) — a compact
encoding representing types, source locations, and variable locations —
carried in the object file alongside the code. Crucially, debug info is pure
*metadata* — it must never change what the program computes. It also explains
a tradeoff every `-O2 -g` user has felt: debug info is a hard problem mostly
because of *optimized* code. LLVM keeps the original source location of each
IR instruction on the instruction itself, and passes are supposed to
preserve them — but when instructions get *merged*, the result can keep only
one location, which is why stepping through optimized code jumps around. And
optimization moves variables: optimized out entirely, sharing memory with
other variables, or otherwise hard to track. Which is exactly why this
chapter **turns the Chapter 4 optimizer off** — and the dumped IR suddenly
shows raw allocas and loads again.

**Debug info in LLVM: metadata + DIBuilder.** LLVM carries the map inside
the IR itself, as metadata that shadows the code structure: a
`DICompileUnit` (per source file) contains `DISubprogram`s (per function),
which scope `DILocalVariable`s (per parameter/variable), and every
instruction can carry a `!dbg` attachment pointing at a
`DILocation(line, col, scope)`. The **`DIBuilder`** class constructs this
graph, mirroring how `IRBuilder` constructs instructions. The backend then
translates it all into DWARF when emitting object code:

```
  source text ──lexer──▶ SourceLocation ──parser──▶ AST nodes carry {line, col}
                (advance() counts                        │ emitExpr()
                 lines/columns)                          ▼
                                     builder->SetCurrentDebugLocation(...)
                                                         │
                                                         ▼
                        %calltmp = call double @fib(...), !dbg !12
                        !12 = !DILocation(line: 5, scope: !4 /*fib*/)
                                                         │ backend
                                                         ▼
                                         DWARF in the object file → debugger
```

One driver consequence up front, with upstream's own caveat: **for now we
can't debug via the JIT**, so the program must become something small and
standalone that a debugger can load. The chapter therefore finishes turning
`toy` into a plain **ahead-of-time batch compiler** — prompts removed,
per-definition IR printing removed, the anonymous top-level function renamed
`__anon_expr` → **`main`**, and the whole module (code + metadata) printed
once at exit. That dump is valid LLVM assembly, so
`toy < fib.ks 2>&1 | clang -x ir -` produces a debuggable native executable.
Upstream also notes the accepted limitation: only **one top-level command
per program**, to keep the changes small — every top-level expression
becomes a `main`, so a second one collides with the first.

Reference: [Chapter 9: Adding Debug Information](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl09.html).
Language features are frozen at [Chapter7](../Chapter7/README.md)'s state;
the driver is [Chapter8](../Chapter8/README.md)'s batch compiler with a
different last step; the codegen architecture is
[Chapter3](../Chapter3/README.md)'s facade, which changes its header here
for the second and last time.

Where the pieces land:

```
  lexer.h/.cpp                ast.h                     parser.cpp
  advance() counts line/col   ExprAST{Kind, Loc}        stamps lexer.getTokLoc()
  getTokLoc() = token start ─▶ getLine()/getCol()   ◀── into every node it builds
                              PrototypeAST::getLine()   top-level expr → "main"

  codegen.h  (facade, 2nd and last change)     codegen.cpp  (Impl)
    CodeGenOptions{optimize, emitDebugInfo,      dbuilder, debugCU, doubleTy, lexicalBlocks
                   sourceFile}                   initializeDebugInfo(): flags, CU
    CodeGenSession(options)                      emitExpr(): emitLocation(&node) first
    finalize()                                   emitFunction(): DISubprogram, prologue,
                                                   parameter variables, pops
                                                 theFPM->run(...) only if options.optimize
  driver.cpp
    codegen({optimize=false, emitDebugInfo=true, "fib.ks"})
    mainLoop() without prompts or printing; dumpModule(): finalize + print
```

(A precise file-by-file diff against Chapter8 is in
[File-by-file](#file-by-file-what-changed-from-chapter8) near the end.)

## Tracking source locations

The lexer stops calling `getchar()` directly; every character now flows
through `advance()`, which maintains a line/column counter. Both the counter
and the snapshot of "where the current token started" are members of the
`Lexer` — the token's position is a lexical fact, so the lexer owns it:

**`Chapter9/include/lexer.h`**
```cpp
/// A position in the source text, 1-based line and column (Chapter 9).
struct SourceLocation {
  int Line;
  int Col;
};

class Lexer {
    public:
    int gettok();
    double getNumVal() const { return numVal; }
    std::string getIdentifierStr() const { return identifierStr; }

    /// Where the most recently returned token starts (Chapter 9). The parser
    /// stamps this into the AST nodes it builds.
    SourceLocation getTokLoc() const { return curLoc; }

    private:
    /// Read one character from stdin, keeping the line/column counter current.
    int advance();

    std::string identifierStr; // Filled in if tok_identifier
    double numVal = 0.0;       // Filled in if tok_number
    int lastChar = ' ';  // Used by gettok

    SourceLocation lexLoc = {1, 0};  // position of the last character read
    SourceLocation curLoc = {1, 0};  // position where the current token started
};
```

**`Chapter9/src/lexer.cpp`**
```cpp
int Lexer::advance() {
    int LastChar = getchar();

    if (LastChar == '\n' || LastChar == '\r') {
        lexLoc.Line++;
        lexLoc.Col = 0;
    } else
        lexLoc.Col++;

    return LastChar;
}
```

`gettok()` snapshots `curLoc = lexLoc` right after skipping whitespace — at
that moment `lastChar` is the first character of the token, so the snapshot
is the token's start. The lookahead discipline from Chapter 2 makes the
rest fall out: the parser always looks at exactly one token, so
`lexer.getTokLoc()` is always the position of `curTok`. Upstream keeps the
same two positions as file-scope globals (`LexLoc` and `CurLoc`); here they
sit next to `identifierStr` and `numVal`, which they resemble — side data
about the current token, read through a getter.

The AST grows a location per node — the base class stores it, the concrete
nodes take it as their first constructor argument, and the prototype records
the line its function was declared on:

**`Chapter9/include/ast.h`**
```cpp
class ExprAST {
public:
  ...
  // Chapter 9: where this node came from.
  int getLine() const { return Loc.Line; }
  int getCol() const { return Loc.Col; }
  SourceLocation getLoc() const { return Loc; }

protected:
  ExprAST(ExprASTKind Kind, SourceLocation Loc) : Kind(Kind), Loc(Loc) {}

private:
  const ExprASTKind Kind;
  SourceLocation Loc;
};

class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(SourceLocation Loc, double Val) : ExprAST(Expr_Num, Loc), Val(Val) {}
  ...
};
```

`ast.h` includes `lexer.h` now — for the `SourceLocation` struct alone — and
still nothing from LLVM. The parser does the stamping. Where upstream
captures a location it captures the same one here (`LitLoc` for a variable
or call, `IfLoc`, `BinLoc`, `FnLoc`); where upstream relies on `ExprAST`'s
default argument `Loc = CurLoc`, the parser passes `lexer.getTokLoc()` at
the same moment, so every node ends up with the location upstream would
have given it:

**`Chapter9/src/parser.cpp`**
```cpp
    std::string idName = lexer.getIdentifierStr();

    SourceLocation LitLoc = lexer.getTokLoc();  // Ch9: where the identifier is

    getNextToken(); // eat identifier

    if (curTok != '(')  // Simple variable ref.
        return std::make_unique<VariableExprAST>(LitLoc, idName);  // Ch9
```

That default-argument behavior has a consequence worth knowing, and it is
kept on purpose: for a **number** the "current token" at construction *is*
the literal, so `3` gets its own position — but a **unary, for or var**
node is constructed *after* its operands have been parsed, so it gets the
position of the token that *follows* it. And every `emitExpr()` re-aims the
builder at its node's location *before* emitting, while a parent's
instruction is created *after* its children run — so in `x < 3` the last
re-aim before the `fcmp` is the `NumberExprAST` for `3`, and the `fcmp`
carries *that* location. Every instruction still lands on a real
line:column (the golden `fib` dump below has no line-0 locations), which is
what makes line-stepping work; the exact columns are upstream's.

## The DIBuilder in the session

Upstream assembles the metadata skeleton in `main()` around two file-scope
globals (`DBuilder`, `KSDbgInfo`) and `extern`s them into the codegen file.
Here the skeleton is part of the code generator, because it *is* codegen
state: it writes into the session's module, must be torn down with it, and
must never leak into the frontend. The facade grows its second and last set
of changes to say so:

**`Chapter9/include/codegen.h`**
```cpp
/// Options controlling IR generation (Chapter 9).
struct CodeGenOptions {
  bool optimize = true;         ///< run the per-function pass pipeline (Chapter 4)
  bool emitDebugInfo = false;   ///< attach DWARF debug info to everything emitted
  std::string sourceFile = "<stdin>";  ///< file name recorded in the debug compile unit
};

class CodeGenSession {
public:
  explicit CodeGenSession(CodeGenOptions options = {});
  ...
  /// Complete the current module's debug metadata (a no-op without
  /// emitDebugInfo). Call before printing or emitting the module.
  void finalize();
```

The defaults reproduce Chapters 4–8 exactly (optimize, no debug info) —
`CodeGenSession()` with no arguments still means what it meant, and the
tests that check optimized IR shapes keep using it. This chapter's driver
asks for the opposite corner:

**`Chapter9/src/driver.cpp`**
```cpp
Driver::Driver()
    : parser(lexer),
      // Chapter 9: no optimization (it scrambles the line mapping), debug info
      // on. The source arrives on stdin, so the compile unit is named after the
      // chapter's example, as upstream hardcodes it.
      codegen(CodeGenOptions{/*optimize=*/false, /*emitDebugInfo=*/true, "fib.ks"}) {
```

Inside the `Impl`, the debug state is upstream's `KSDbgInfo` plus the
builder, initialized right after the module they belong to:

**`Chapter9/src/codegen.cpp`**
```cpp
  // Chapter 9: debug-info state (upstream's DBuilder + KSDbgInfo), only with
  // options.emitDebugInfo.
  std::unique_ptr<llvm::DIBuilder> dbuilder;
  llvm::DICompileUnit *debugCU = nullptr;
  llvm::DIType *debugDoubleTy = nullptr;
  std::vector<llvm::DIScope *> lexicalBlocks;  // innermost scope at the back
  ...
  void initializeDebugInfo() {
    // Add the current debug info version into the module.
    theModule->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                             llvm::DEBUG_METADATA_VERSION);

    // Darwin only supports dwarf2.
    if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
      theModule->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

    // Construct the DIBuilder, we do this here because we need the module.
    dbuilder = std::make_unique<llvm::DIBuilder>(*theModule);

    // Create the compile unit for the module. The source arrives on stdin, so
    // the file name is whatever the driver was told (upstream hardcodes "fib.ks").
    debugCU = dbuilder->createCompileUnit(
        llvm::dwarf::DW_LANG_C, dbuilder->createFile(options.sourceFile, "."),
        "Kaleidoscope Compiler", /*isOptimized=*/options.optimize, "", /*RV=*/0);
    debugDoubleTy = nullptr;
    lexicalBlocks.clear();
  }
```

`DIBuilder` is to debug metadata what `IRBuilder` is to instructions — a 1:1
correspondence with the IR, "but with nicer names". Upstream's honest note:
using it demands more DWARF-terminology fluency than `IRBuilder` demanded
instruction fluency; the [Metadata Format
docs](https://llvm.org/docs/SourceLevelDebugging.html) fill that in. It is
constructed *from the module* (metadata lives inside the module), so
`initializeModule()` calls `initializeDebugInfo()` last — and `take()`
resets `dbuilder` along with the builder and pass managers before the
module moves out, for the same reason those are reset: it holds a reference
into the module that is leaving.

Details: the **compile unit** is DWARF's top-level container — the type and
function data for one translation unit, i.e. one source file. It claims
language `DW_LANG_C`, and not out of laziness: a debugger can't know the
calling conventions or ABI of a language it has never heard of, and since
Kaleidoscope's codegen follows the C ABI anyway, claiming C is the closest
thing to accurate — it's what lets you *call* `fib(5)` from the debugger
prompt and have it execute. The filename comes from the options; the driver
passes `"fib.ks"` because the source arrives by shell redirection and stdin
has no name (a real front end would put its input filename here — the
option exists so that a driver which *has* one can). `finalize()` must run
before output — `DIBuilder` defers constructing some metadata cycles until
then — and the facade method of that name is all the driver calls. The
cached `DIType` for double (`createBasicType("double", 64, DW_ATE_float)`)
and the stack of lexical scopes complete upstream's little state bundle.

## Functions: DISubprogram, prologue, and parameters

`emitFunction()` grows a metadata mirror of what it already did for code,
every step guarded by `if (dbuilder)` so a session without debug info emits
exactly what Chapter 8 did:

**`Chapter9/src/codegen.cpp`**
```cpp
    // Chapter 9: create a subprogram DIE for this function.
    llvm::DISubprogram *SP = nullptr;
    llvm::DIFile *Unit = nullptr;
    unsigned LineNo = Proto.getLine();
    if (dbuilder) {
      Unit = dbuilder->createFile(debugCU->getFilename(), debugCU->getDirectory());
      llvm::DIScope *FContext = Unit;
      unsigned ScopeLine = LineNo;
      SP = dbuilder->createFunction(
          FContext, Proto.getName(), llvm::StringRef(), Unit, LineNo,
          createFunctionType(TheFunction->arg_size()), ScopeLine,
          llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
      TheFunction->setSubprogram(SP);

      // Push the current scope.
      lexicalBlocks.push_back(SP);

      // Unset the location for the prologue emission (leading instructions with no
      // location in a function are considered part of the prologue and the debugger
      // will run past them when breaking on a function)
      emitLocation(nullptr);
    }
```

The context comes first: a `DIFile` (asked from the compile unit for the
current directory and filename) serves as the subprogram's scope. Upstream's
§9.6 prose passes `LineNo = 0` at this point in the chapter's story — "since
our AST doesn't currently have source location information" *yet* — and
upgrades it once §9.7 adds locations; this code (like upstream's final
listing) is the upgraded form, `Proto.getLine()` from the prototype's real
position. Three more ideas in those lines:

- **`createFunctionType(n)`** builds the `DISubroutineType` — in a
  one-type language that's just "double, n+1 times" (result + args).
- **The scope stack.** `lexicalBlocks` is pushed on function entry and
  popped on both the success and failure paths; `emitLocation()` uses its
  top as the scope for every `DILocation`. With only function-level scopes
  the stack is depth ≤ 1, but the structure is what real lexical blocks
  (`{}` in C) would need. The unit test `FailedBodyPopsScope` checks the
  failure-path pop: after a definition whose body failed, the next
  function's locations are scoped to *its* subprogram.
- **The prologue trick.** Setting a *null* location first means the
  argument-alloca setup gets no line info, so a breakpoint on `fib` stops
  *after* the boilerplate, at the first user line.

Each parameter gets a `DILocalVariable` plus a declare record binding it to
its alloca — this is what lets the debugger `print x`:

**`Chapter9/src/codegen.cpp`**
```cpp
      // Chapter 9: create a debug descriptor for the variable.
      if (dbuilder) {
        llvm::DILocalVariable *D = dbuilder->createParameterVariable(
            SP, Arg.getName(), ++ArgIdx, Unit, LineNo, getDoubleTy(), true);

        dbuilder->insertDeclare(Alloca, D, dbuilder->createExpression(),
                                llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
                                builder->GetInsertBlock());
      }
```

The `createParameterVariable` call gives the variable its scope (`SP`),
name, source location, type, and — since it is a parameter — its argument
index; `insertDeclare` then plants a **`#dbg_declare` record** in the IR
saying "this variable lives in that alloca", stamped with a location for the
beginning of the scope. (`#dbg_declare` is modern LLVM's "debug record"
syntax; older LLVM printed the same thing as a
`call void @llvm.dbg.declare` intrinsic.)

Finally, locations on instructions. Upstream sprinkles
`KSDbgInfo.emitLocation(this)` at the top of every expression `codegen()`;
here every expression enters through `emitExpr()`, so one call covers all
eight node kinds:

**`Chapter9/src/codegen.cpp`**
```cpp
  llvm::Value *emitExpr(ExprAST &expr) {
    emitLocation(&expr);  // Chapter 9: instructions for this node get its location
    switch (expr.getKind()) {
    ...

  /// Aim the builder's debug location at `AST` (or clear it for nullptr): every
  /// instruction created until the next call carries that location.
  void emitLocation(const ExprAST *AST) {
    if (!dbuilder)
      return;
    if (!AST)
      return builder->SetCurrentDebugLocation(llvm::DebugLoc());
    llvm::DIScope *Scope;
    if (lexicalBlocks.empty())
      Scope = debugCU;
    else
      Scope = lexicalBlocks.back();
    builder->SetCurrentDebugLocation(llvm::DILocation::get(
        Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
  }
```

The scope for a location is the innermost entry of the `lexicalBlocks`
stack, falling back to the compile unit outside any function. And the
prologue trick is a two-step: after the null location suppresses line info
for the argument setup, `emitFunction()` re-arms the builder with
`emitLocation(fn.getBody())` right before generating the body, so the first
*user* instruction is where the debugger lands. The pipeline run at the end
of `emitFunction()` is now `if (options.optimize)` — the FPM code from
Chapter 4 is still there for a session that wants it, but this driver does
not.

## File-by-file: What changed from Chapter8

The exact split (established with `diff -rq ../Chapter8 .`). The lit suites
change wholesale — a driver that no longer prints per-definition optimized IR
cannot satisfy Chapter 8's checks — while the gtest suites carry forward
with additions.

**New files**

| File | Purpose |
| --- | --- |
| `example/fib.ks` | The chapter's demo: `fib` + a top-level `fib(10)` that becomes `main`. |
| `example/cmd.txt` | The Chapter7/8 demo input, moved under `example/`, kept runnable. |
| `test/filecheck/debuginfo.k` | Metadata checks (see Tests). |
| `test/filecheck/toplevel-limit.k` | The one-top-level-command rule, as an error (see Deviations). |

**Removed**: the previous `.k` suites (`opt.k`, `controlflow.k`,
`userops.k`, `mutablevars.k`, `objfile.k`), `mem2reg_ex/`, Chapter 8's
`example/average.txt` + `example/main.cpp`, `note.txt`, and the top-level
`cmd.txt` (moved under `example/`).

**Same filename, byte-identical**: `include/parser.h`, `include/log.h`,
`src/log.cpp`, `src/extern_d.cpp`, `build.sh`, `test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter9/include/lexer.h` / `src/lexer.cpp` — `SourceLocation`, the
`advance()` counter, `curLoc` snapshot and `getTokLoc()`:

```cpp
// Chapter8                       // Chapter9
lastChar = getchar();              lastChar = advance();          // everywhere
                                   curLoc = lexLoc;               // after the whitespace skip
```

`Chapter9/include/ast.h` — `ExprAST` gains `Loc` with
`getLine()`/`getCol()`/`getLoc()`; every node's constructor takes a
`SourceLocation` first; `PrototypeAST` records and exposes its line:

```cpp
// Chapter8                                // Chapter9
VariableExprAST(const std::string &Name)    VariableExprAST(SourceLocation Loc,
    : ExprAST(Expr_Var), Name(Name) {}                      const std::string &Name)
                                                : ExprAST(Expr_Var, Loc), Name(Name) {}
```

`Chapter9/src/parser.cpp` — captures `lexer.getTokLoc()` before building
located nodes (`LitLoc`, `IfLoc`, `BinLoc`, `FnLoc`, or inline at
construction); `parseTopLevelExpr` names the function **`main`**. No other
change — `parser.h` is byte-identical.

`Chapter9/include/codegen.h` — the facade's second and last change:
`CodeGenOptions`, the options constructor, `finalize()`.

`Chapter9/src/codegen.cpp` — the `Impl` gains the options and the debug
state, `initializeDebugInfo()`, `getDoubleTy()`, `createFunctionType()`,
`emitLocation()`; `emitExpr()` sets the location first; `emitFunction()`
grows the subprogram, prologue, parameter variables and scope pops; the FPM
run becomes conditional; `take()` tears down the debug builder;
`finalize()` implemented. `diff` shows three removed lines: the old
constructor, the unconditional `theFPM->run`, and the old facade constructor.

`Chapter9/include/driver.h` / `src/driver.cpp` — `emitObjectFile()` becomes
`dumpModule()` (finalize + print); the constructor passes
`CodeGenOptions{false, true, "fib.ks"}` and no longer sets a target triple;
the handlers report errors only; `mainLoop()` loses its prompts.

`Chapter9/src/main.cpp` — `return driver.emitObjectFile("output.o");`
becomes `return driver.dumpModule();`.

`Chapter9/CMakeLists.txt` — the lit registration's comment; otherwise
Chapter 8's (library + three gtest binaries + lit).

`Chapter9/run.sh` — the new pipeline: dump IR, hand it to clang:

```sh
./build/toy < example/fib.ks 2>&1 | clang -o ./build/a.out -x ir -
./build/a.out
```

`Chapter9/test/lexer_test.cpp` — `LexerLocationTest`: token start columns
on one line, and line/column after comments and blank lines.

`Chapter9/test/parser_test.cpp` — `NodesCarryLocations` (every node of
`a + b * c` knows its column) and `PrototypeLineAndMainName`.

`Chapter9/test/codegen_test.cpp` — the hand-built AST helpers take a
location (`at(line, col)`, defaulting to 0:0); `DebugInfoTest` builds a
session with the driver's options and checks the subprogram, `!dbg`
attachments and scopes, the location-free prologue, the compile unit's file
name, and the failure-path scope pop; `DefaultSessionHasNoDebugInfo`
checks the default corner.

`Chapter9/test/filecheck/errors.k` — the surviving definition is checked in
the module dump rather than as a per-definition print.

## Deviations from upstream

Chapter2–9 keep the tutorial's behavior, quirks included. This chapter's
module dump for `fib.ks` — every instruction, every metadata node, every
line and column — is byte-identical to what the earlier, upstream-shaped
version of the chapter produced. The departures below are about *where*
things live and about one input upstream never feeds its own example.

### The data layout comes from a `TargetMachine`, not a resurrected JIT

Upstream's Chapter 9 keeps creating the `KaleidoscopeJIT` from Chapter 4
even though nothing is JIT-compiled any more:

```cpp
// upstream (LangImpl09)
int main() {
  ...
  TheJIT = ExitOnErr(KaleidoscopeJIT::Create());

  InitializeModule();   // ... TheModule->setDataLayout(TheJIT->getDataLayout());
```

— the JIT exists only to answer `getDataLayout()`. Chapter 8 already had a
better source for that answer, and this driver keeps it:

**`Chapter9/src/driver.cpp`**
```cpp
  // Set up the host target -- it exists here only to supply a data layout.
  ...
  theTargetMachine.reset(Target->createTargetMachine(
      TargetTriple, "generic", "", opt, llvm::Reloc::PIC_));

  codegen.setDataLayout(theTargetMachine->createDataLayout());
```

Why: a batch compiler that dumps IR for `clang` has no business owning an
ORC execution session, and the layout string is the same either way — the
JIT's `JITTargetMachineBuilder` detects the host and creates a
`TargetMachine` internally; asking the host `TargetMachine` directly skips
the detour. The `target datalayout` line in the dump is identical.

|                              | upstream                           | here                              |
| ---------------------------- | ---------------------------------- | --------------------------------- |
| source of the data layout    | `TheJIT->getDataLayout()`          | `TargetMachine::createDataLayout()` |
| ORC session alive at runtime | yes, unused                        | no                                |
| `target datalayout` in dump  | host layout                        | same string                       |

### Debug state lives in the session, not in globals

Upstream declares `static std::unique_ptr<DIBuilder> DBuilder;` and
`static DebugInfo KSDbgInfo;` at file scope (the earlier version of this
chapter kept them global too, `extern`'d between `debug.cpp`, `codegen.cpp`
and `main.cpp`, and threaded a `DebugInfoManager` through the lexer for the
line counter). Here `dbuilder`, `debugCU`, `debugDoubleTy` and
`lexicalBlocks` are members of `CodeGenSession::Impl`, created by
`initializeModule()` when `options.emitDebugInfo` is set, and the line
counter is the lexer's own. Two consequences: a session without debug info
pays nothing and emits Chapter 8's IR unchanged (the default-constructed
sessions in the unit tests prove it), and the metadata builder is torn down
with the module it writes into — `take()` resets it first, so a JIT-style
driver could use this session too. The `CodeGenOptions` struct is the
public face of that choice: it is how the driver, and only the driver, says
"debug info, no optimizer, this file name".

### No `dump()` methods on the AST

Upstream's §9.7 also gives every node a `dump(raw_ostream&, int)` method — an
indenting pretty-printer — which pulls `llvm/Support/raw_ostream.h` into
`ast.h`. This chapter does not: nothing in the tutorial calls `dump()`, and
the AST has been deliberately LLVM-free since Chapter 2 so the frontend
never depends on the backend. A tree printer belongs *outside* the nodes,
walking them through the kind tags and getters exactly as the code
generator does — ChapterA's external `ASTDumper` is that design.

### A second top-level expression is an error

Every top-level expression becomes `main`, and `main` stays in the module.
Upstream, having dropped Chapter 3's redefinition guard in Chapter 4, lets a
second one through: `emitFunction` finds the existing `main`, appends a new
`entry1` block to it, and the dump shows a function with two entry blocks,
the second unreachable. Chapter 8 brought the guard back, so here the second
expression is refused and the first `main` is untouched:

```
1 + 2;
3 + 4;
→ Error: Function cannot be redefined.
  define double @main() { entry: ret double 3.000000e+00 }
```

`toplevel-limit.k` pins this — it is the one lit input whose output differs
from the earlier version of the chapter, and it documents upstream's
"one top-level command per program" limitation as a reported error rather
than a silent dead block.

## Build and run

`./build.sh`, then `./run.sh`. The chapter's sample program — upstream's
fibonacci, one function definition plus the single top-level command that
becomes `main`:

**`Chapter9/example/fib.ks`**
```
def fib(x)
  if x < 3 then
    1
  else
    fib(x-1)+fib(x-2);

fib(10)
```

The dumped IR for it — unoptimized
again (allocas are back!), with `!dbg` on nearly every instruction:

```
define double @fib(double %x) !dbg !4 {
entry:
  %x1 = alloca double, align 8
    #dbg_declare(ptr %x1, !9, !DIExpression(), !10)
  store double %x, ptr %x1, align 8
  %x2 = load double, ptr %x1, align 8, !dbg !11
  %cmptmp = fcmp ult double %x2, 3.000000e+00, !dbg !12
  ...
}

!llvm.dbg.cu = !{!2}
!2 = distinct !DICompileUnit(language: DW_LANG_C, file: !3,
         producer: "Kaleidoscope Compiler", isOptimized: false, ...)
!4 = distinct !DISubprogram(name: "fib", scope: !3, file: !3, line: 1, ...)
!9 = !DILocalVariable(name: "x", arg: 1, scope: !4, file: !3, line: 1, type: !7)
!11 = !DILocation(line: 2, column: 6, scope: !4)
!12 = !DILocation(line: 2, column: 10, scope: !4)
```

Note the metadata graph at the bottom mirroring the source: compile unit →
subprogram `fib` → parameter `x` → per-instruction locations, every one a
real line:column (the `fcmp`'s `!12` is the `3` literal's position — the
inherited-location effect described above). Piping this through
`clang -x ir -` yields a native `a.out` you can open in lldb: `b fib`,
`run`, `print x`.

## Tests

Same two-scheme setup — rationale and lit mechanics in the [top-level
README](../README.md#testing-the-two-schemes). Both layers carry forward
from Chapter 8; what Chapter 9 adds:

- **`lexer_test.cpp`** — the line/column counter, at the unit level:
  `def foo(x)` puts `foo` at column 5 and `(` at 8; after a comment line and
  a blank line, `if` is on line 3, column 3.
- **`parser_test.cpp`** — every node of `a + b * c` knows where its token
  was (the `+` at column 3, the `*` at 7); a `def` on line 3 records line 3;
  a top-level expression is named `main`.
- **`codegen_test.cpp`** — `DebugInfoTest` runs a session with the driver's
  options and checks what the metadata mirror must contain: a
  `DISubprogram` named and lined after the prototype, `!dbg` attachments on
  the body's instructions scoped to it, *no* location on the prologue, a
  compile unit carrying the `sourceFile` option, and — on a failed body —
  the scope stack popped so the next function is not mis-scoped. A
  default-constructed session must produce no debug metadata at all, which
  is what keeps the earlier chapters' IR-shape tests valid here unchanged.

**`Chapter9/test/filecheck/debuginfo.k`**
```
# CHECK: define double @fib(double %x)
# CHECK: !dbg
# CHECK: !llvm.dbg.cu
# CHECK-DAG: DICompileUnit(language: DW_LANG_C
# CHECK-DAG: DISubprogram(name: "fib"{{.*}}line: 6
# CHECK-DAG: DILocation(line: 7, column: 6
```

New FileCheck idiom: **`CHECK-DAG`** matches lines in *any order* within the
region — metadata nodes are numbered and ordered by construction history,
which is an implementation detail no test should pin down. Note the checks
pin the *exact* line numbers: `def fib` sits on line 6 of the `.k` file
itself and its body on line 7 (the `RUN:`/comment lines above it advance the
lexer's counter too, since the whole file is the compiler's stdin) — which
makes this one test also the end-to-end regression test for `advance()`'s
line/column tracking.

`errors.k` carries the error-recovery contract into the batch-compiler era:
errors are reported, parsing continues, and the surviving `def good`
appears in the final module dump.

`toplevel-limit.k` pins the one-top-level-command limitation as an error
(see Deviations).

```sh
ctest --test-dir build                 # gtest suites + the filecheck tests
lit -v test/filecheck/debuginfo.k
./build/codegen_test --gtest_filter='DebugInfoTest.*'
```
