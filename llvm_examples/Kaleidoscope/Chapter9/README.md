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
chapter **turns the Chapter 4 optimizer off** — the FPM is gone, and the
dumped IR suddenly shows raw allocas and loads again.

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
                (advance() counts                        │ codegen
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
(The JIT object returns from its Chapter 8 exile for exactly one duty:
supplying a data layout.) Upstream also notes the accepted limitation: only
**one top-level command per program**, to keep the changes small — every
top-level expression becomes a `main`, so a second one would collide with
the first.

Reference: [Chapter 9: Adding Debug Information](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl09.html).
Language features are frozen at [Chapter7](../Chapter7/README.md)'s state.

(A precise file-by-file diff against Chapter8 is in
[File-by-file](#file-by-file-what-changed-from-chapter8) near the end.)

## Tracking source locations

The lexer stops calling `getchar()` directly; every character now flows
through `advance()`, which maintains a line/column counter. That lives in a
small `DebugInfoManager` class the `Lexer` holds a reference to:

**`Chapter9/src/debug.cpp`**
```cpp
int DebugInfoManager::advance() {
  int LastChar = getchar();

  if (LastChar == '\n' || LastChar == '\r') {
    lexLoc.Line++;
    lexLoc.Col = 0;
  } else
    lexLoc.Col++;

  return LastChar;
}
```

`gettok()` snapshots `lexLoc` into `CurLoc` at each token start (`CurLoc`
is still a file-scope global with a `// Temp Global variable` comment — the
TODO in `debug.h` notes it should become a member). The parser then stamps
locations into the AST: `ExprAST` gains a `SourceLocation Loc` plus
`getLine()`/`getCol()`, and the nodes where locations matter most get them
via constructor (`VariableExprAST`, `CallExprAST`, `BinaryExprAST`,
`IfExprAST`, `PrototypeAST`). Every node also gains a `dump()` method — an
indenting AST pretty-printer that prints each node with its `line:col`
(useful for debugging the compiler rather than the program).

The nodes *without* an explicit location parameter (numbers, unary/for/var
expressions) are not locationless: as upstream does, the base-class default
argument reads the global lexer position at construction time —
`ExprAST(SourceLocation Loc = CurLoc)` — so every node carries a real
location. This default matters more than it looks: every `codegen()`
re-aims the builder at its own location *before* emitting, and a parent's
instruction is created *after* its children run — in `x < 3` the last
re-aim before the `fcmp` is the `NumberExprAST` for `3`, so the `fcmp`
inherits *that* node's location. With the `CurLoc` default every such
inherited location is a real line:column; if the default were `{0, 0}`
instead, comparisons, calls, and arithmetic would all land on line 0 and
line-stepping in a debugger would go coarse.

## The DIBuilder setup

`main()` now assembles the metadata skeleton around the parse loop:

**`Chapter9/src/main.cpp`**
```cpp
    // Add the current debug info version into the module.
    ctx.theModule->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);

    // Darwin only supports dwarf2.
    if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
      ctx.theModule->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

    // Construct the DIBuilder, we do this here because we need the module.
    DBuilder = std::make_unique<llvm::DIBuilder>(*ctx.theModule);

    // Create the compile unit for the module.
    // Currently down as "fib.ks" as a filename since we're redirecting stdin
    // but we'd like actual source locations.
    KSDbgInfo.TheCU = DBuilder->createCompileUnit(
      llvm::dwarf::DW_LANG_C, DBuilder->createFile("fib.ks", "."),
      "Kaleidoscope Compiler", false, "", 0);

    // Run the main "interpreter loop" now.
    parser.mainLoop();

    // Finalize the debug info.
    DBuilder->finalize();

    // Print out all of the generated code.
    ctx.theModule->print(llvm::errs(), nullptr);
```

`DIBuilder` is to debug metadata what `IRBuilder` is to instructions — a 1:1
correspondence with the IR, "but with nicer names". Upstream's honest note:
using it demands more DWARF-terminology fluency than `IRBuilder` demanded
instruction fluency; the [Metadata Format
docs](https://llvm.org/docs/SourceLevelDebugging.html) fill that in. It is
constructed *from the module* (metadata lives inside the module), so it must
be created right after the module — and, like upstream, `DBuilder` and
`KSDbgInfo` are file-scope globals ("to make it a bit easier to use", in
`src/debug.cpp`, `extern`'d where needed).

Details: the **compile unit** is DWARF's top-level container — the type and
function data for one translation unit, i.e. one source file. It claims
language `DW_LANG_C`, and not out of laziness: a debugger can't know the
calling conventions or ABI of a language it has never heard of, and since
Kaleidoscope's codegen follows the C ABI anyway, claiming C is the closest
thing to accurate — it's what lets you *call* `fib(5)` from the debugger
prompt and have it execute. The filename is hardcoded `"fib.ks"` because the
source arrives by shell redirection and stdin has no name (a real front end
would put its input filename here). And `finalize()` must run before output
— `DIBuilder` defers constructing some metadata cycles until then. The
`KSDbgInfo` struct (in `debug.h`) is the chapter's little state bundle: the
compile unit, a cached `DIType` for double
(`createBasicType("double", 64, DW_ATE_float)`), and a stack of lexical
scopes.

## Functions: DISubprogram, prologue, and parameters

`FunctionAST::codegen()` grows a metadata mirror of what it already did for
code:

**`Chapter9/src/codegen.cpp`**
```cpp
  // Create a subprogram DIE for this function.
  llvm::DIFile *Unit = DBuilder->createFile(KSDbgInfo.TheCU->getFilename(),
                                      KSDbgInfo.TheCU->getDirectory());
  llvm::DIScope *FContext = Unit;
  unsigned LineNo = P.getLine();
  unsigned ScopeLine = LineNo;
  llvm::DISubprogram *SP = DBuilder->createFunction(
      FContext, P.getName(), llvm::StringRef(), Unit, LineNo,
      CreateFunctionType(TheFunction->arg_size()), ScopeLine,
      llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
  TheFunction->setSubprogram(SP);

  // Push the current scope.
  KSDbgInfo.LexicalBlocks.push_back(SP);

  // Unset the location for the prologue emission (leading instructions with no
  // location in a function are considered part of the prologue and the debugger
  // will run past them when breaking on a function)
  KSDbgInfo.emitLocation(nullptr, ctx);
```

The context comes first: a `DIFile` (asked from the compile unit for the
current directory and filename) serves as the subprogram's scope. Upstream's
§9.6 prose passes `LineNo = 0` at this point in the chapter's story — "since
our AST doesn't currently have source location information" *yet* — and
upgrades it once §9.7 adds locations; this code (like upstream's final
listing) is the upgraded form, `P.getLine()` from the prototype's real
position. Three more ideas in those lines:

- **`CreateFunctionType(n)`** builds the `DISubroutineType` — in a
  one-type language that's just "double, n+1 times" (result + args).
- **The scope stack.** `LexicalBlocks` is pushed on function entry and
  popped on both the success and failure paths; `emitLocation()` uses its
  top as the scope for every `DILocation`. With only function-level scopes
  the stack is depth ≤ 1, but the structure is what real lexical blocks
  (`{}` in C) would need.
- **The prologue trick.** Setting a *null* location first means the
  argument-alloca setup gets no line info, so a breakpoint on `fib` stops
  *after* the boilerplate, at the first user line.

Each parameter gets a `DILocalVariable` plus a declare record binding it to
its alloca — this is what lets the debugger `print x`:

**`Chapter9/src/codegen.cpp`**
```cpp
    llvm::DILocalVariable *D = DBuilder->createParameterVariable(
        SP, Arg.getName(), ++ArgIdx, Unit, LineNo, KSDbgInfo.getDoubleTy(),
        true);

    DBuilder->insertDeclare(Alloca, D, DBuilder->createExpression(),
                            llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
                            ctx.builder->GetInsertBlock());
```

The `createParameterVariable` call gives the variable its scope (`SP`),
name, source location, type, and — since it is a parameter — its argument
index; `insertDeclare` then plants a **`#dbg_declare` record** in the IR
saying "this variable lives in that alloca", stamped with a location for the
beginning of the scope. (`#dbg_declare` is modern LLVM's "debug record"
syntax; older LLVM printed the same thing as a
`call void @llvm.dbg.declare` intrinsic.)

Finally, `KSDbgInfo.emitLocation(this, ctx)` is sprinkled at the top of
every expression `codegen()` — it points the builder at the node's
`DILocation`, and the builder stamps that onto each instruction it creates
until the location changes:

**`Chapter9/src/debug.cpp`**
```cpp
void DebugInfo::emitLocation(ExprAST *AST, IRGenContext &ctx) {
  if (!AST)
    return ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
  llvm::DIScope *Scope;
  if (LexicalBlocks.empty())
    Scope = TheCU;
  else
    Scope = LexicalBlocks.back();
  ctx.builder->SetCurrentDebugLocation(llvm::DILocation::get(
      Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}
```

The scope for a location is the innermost entry of the `LexicalBlocks`
stack, falling back to the compile unit outside any function. And the
prologue trick is a two-step: after the null location suppresses line info
for the argument setup, `FunctionAST::codegen` re-arms the builder with
`KSDbgInfo.emitLocation(Body.get(), ctx)` right before generating the body,
so the first *user* instruction is where the debugger lands.

## File-by-file: What changed from Chapter8

This chapter also downsizes the harness: gtest is gone entirely (see Tests),
and the previous chapters' `.k` suites don't apply to a driver that no
longer prints per-definition optimized IR.

**New files**

| File | Purpose |
| --- | --- |
| `include/debug.h`, `src/debug.cpp` | `DebugInfo` (KSDbgInfo), `SourceLocation`, `DebugInfoManager::advance()`, `CreateFunctionType()`. |
| `example/fib.ks` | The chapter's demo: `fib` + a top-level `fib(10)` that becomes `main`. |
| `example/cmd.txt` | The Chapter7/8 demo input, kept runnable. |
| `test/filecheck/debuginfo.k` | Metadata checks (see Tests). |
| `test/filecheck/errors.k` | The batch driver still recovers from codegen errors; surviving definitions reach the module dump. |

**Removed**: all three gtest files (`lexer_test.cpp`, `parser_test.cpp`,
`codegen_test.cpp`), the previous `.k` suites (`opt.k`, `controlflow.k`,
`userops.k`, `mutablevars.k`, `objfile.k`), `mem2reg_ex/`, Chapter 8's
`example/average.txt` + `example/main.cpp`, `note.txt`, and the top-level
`cmd.txt` (moved under `example/`).

**Same filename, byte-identical**: `build.sh`, `include/log.h`,
`src/log.cpp`, `src/extern_d.cpp`, `test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter9/include/lexer.h` / `src/lexer.cpp` — the lexer takes a
`DebugInfoManager&`; every `getchar()` becomes `dbgMgr.advance()`, and
`gettok()` snapshots `CurLoc = dbgMgr.lexLoc` after whitespace.

`Chapter9/include/ast.h` — `ExprAST` gains `Loc`/`getLine()`/`getCol()` and
virtual `dump()`; located constructors added:

```cpp
// Chapter8                                // Chapter9
VariableExprAST(const std::string &Name)    VariableExprAST(SourceLocation Loc,
    : Name(Name) {}                                         const std::string &Name)
                                                : ExprAST(Loc), Name(Name) {}
```

`Chapter9/include/ir_gen_ctx.h` — the JIT returns (created again, its data
layout stamped on the module), and the **entire FPM block is deleted** — no
pass managers, no optimization, in service of debuggability.

`Chapter9/src/parser.cpp` — captures `CurLoc` before building located
nodes; `parseTopLevelExpr` names the function **`main`**; the handlers stop
printing IR (errors only); `mainLoop` loses its `ready> ` prompts.

`Chapter9/src/main.cpp` — Chapter 8's `compile_obj()` is gone; replaced by
the DIBuilder setup/finalize around `mainLoop()` and a final whole-module
print.

`Chapter9/CMakeLists.txt` — rewritten small: one `toy` executable from all
sources (no `toy_core` library, no FetchContent/gtest); only the lit test
registration remains, with a comment giving the rationale.

`Chapter9/run.sh` — the new pipeline: dump IR, hand it to clang:

```sh
./build/toy < example/fib.ks 2>&1 | clang -o ./build/a.out -x ir -
./build/a.out
```

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
real line:column thanks to the `CurLoc` default (the `fcmp`'s `!12` is the
`3` literal's position — the inherited-location effect described above). Piping
this through `clang -x ir -` yields a native `a.out` you can open in lldb:
`b fib`, `run`, `print x`.

## Tests

Chapter 9 keeps only the lit/FileCheck layer, and `CMakeLists.txt` says why:
the chapter's observable behavior — DWARF metadata in the module dump — *is
text*, exactly what FileCheck matches; there is no numeric result to compare
(gtest's JIT tables) and no API contract that changed (the frontend is
frozen). The two-scheme rationale from the [top-level
README](../README.md#testing-the-two-schemes) cuts the other way here, and
the harness follows it.

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
makes this one test also the end-to-end regression test for
`DebugInfoManager::advance()`'s line/column tracking.

`errors.k` carries the error-recovery contract into the batch-compiler era:
errors are reported, parsing continues, and the surviving `def good`
appears in the final module dump.

`toplevel-limit.k` pins the one-top-level-command limitation described in
the intro: a second top-level expression is not rejected — with no
redefinition guard (dropped in Chapter4, faithful to upstream), its body is
appended to the existing `main()` as a dead `entry1` block. The test exists
so that any change in this behavior is noticed, not to bless it.

```sh
ctest --test-dir build                 # just the filecheck tests now
lit -v test/filecheck/debuginfo.k
```
