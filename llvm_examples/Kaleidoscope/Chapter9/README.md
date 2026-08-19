# Chapter 9 — Adding Debug Information

The last chapter teaches compiled Kaleidoscope programs to be **debugged at
the source level** — set a breakpoint on `fib`, step by line, print `x`. Two
general concepts first:

**Debug information.** A debugger works on machine state — a program counter
and memory. Source-level debugging needs a *map* from that state back to the
source: which file/line does this PC correspond to, which stack slot holds
the variable named `x`, what scope is active. Compilers emit this map as
**DWARF** (the standard debug format on Unix-family systems), carried in the
object file alongside the code. Crucially, debug info is pure *metadata* — it
must never change what the program computes. It also explains a tradeoff
every `-O2 -g` user has felt: optimization scrambles the map (instructions
reordered and merged, variables promoted out of memory), which is exactly why
this chapter **turns the Chapter 4 optimizer off** — the FPM is gone, and the
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

One driver consequence up front: a REPL has no meaningful "source file", so
the chapter finishes turning `toy` into a plain **batch compiler** — prompts
removed, per-definition IR printing removed, the anonymous top-level function
renamed `__anon_expr` → **`main`**, and the whole module (code + metadata)
printed once at exit. That dump is valid LLVM assembly, so
`toy < fib.ks 2>&1 | clang -x ir -` produces a debuggable native executable.
(The JIT object returns from its Chapter 8 exile for exactly one duty:
supplying a data layout — upstream does the same.)

Reference: [Chapter 9: Adding Debug Information](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl09.html).
Language features are frozen at [Chapter7](../Chapter7/README.md)'s state.

(A precise file-by-file diff against Chapter8 is in
[File-by-file](#file-by-file-what-changed-from-chapter8) near the end.)

## Tracking source locations

The lexer stops calling `getchar()` directly; every character now flows
through `advance()`, which maintains a line/column counter. In this refactor
that lives in a small `DebugInfoManager` class the `Lexer` holds a reference
to (upstream uses free functions and globals):

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
(upstream adds the same; useful for debugging the compiler rather than the
program).

One deviation from upstream to know about: upstream declares
`ExprAST(SourceLocation Loc = CurLoc)` — the default argument reads the
*global lexer position*, so every node gets a real location. Here the
default is `{0, 0}`, so nodes not explicitly given a location (numbers,
unary/for/var expressions) emit `DILocation(line: 0, ...)` — visible in the
module dump below. Debuggers treat line 0 as "no source line", so stepping
still works, just with coarser granularity on those constructs.

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

    DBuilder = std::make_unique<llvm::DIBuilder>(*ctx.theModule);

    KSDbgInfo.TheCU = DBuilder->createCompileUnit(
      llvm::dwarf::DW_LANG_C, DBuilder->createFile("fib.ks", "."),
      "Kaleidoscope Compiler", false, "", 0);

    parser.mainLoop();

    DBuilder->finalize();                       // required before dumping
    ctx.theModule->print(llvm::errs(), nullptr);
```

Details: the compile unit claims language `DW_LANG_C` so debuggers apply
their C defaults (Kaleidoscope isn't on DWARF's language list); the filename
is hardcoded `"fib.ks"` since stdin has no name; and `finalize()` must run
before output — `DIBuilder` defers constructing some metadata cycles until
then. The `KSDbgInfo` struct (in `debug.h`) is the chapter's little state
bundle: the compile unit, a cached `DIType` for double
(`createBasicType("double", 64, DW_ATE_float)`), and a stack of lexical
scopes.

## Functions: DISubprogram, prologue, and parameters

`FunctionAST::codegen()` grows a metadata mirror of what it already did for
code:

**`Chapter9/src/codegen.cpp`**
```cpp
  llvm::DISubprogram *SP = DBuilder->createFunction(
      FContext, P.getName(), llvm::StringRef(), Unit, LineNo,
      CreateFunctionType(TheFunction->arg_size()), ScopeLine,
      llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
  TheFunction->setSubprogram(SP);

  KSDbgInfo.LexicalBlocks.push_back(SP);   // enter the function's scope

  // Unset the location for the prologue emission (leading instructions with no
  // location in a function are considered part of the prologue and the debugger
  // will run past them when breaking on a function)
  KSDbgInfo.emitLocation(nullptr, ctx);
```

Three ideas in those lines:

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

(In the dump this appears as `#dbg_declare(ptr %x1, ...)` — modern LLVM's
"debug record" syntax, replacing the older `call void @llvm.dbg.declare`
intrinsic form the tutorial text shows.)

Finally, `KSDbgInfo.emitLocation(this, ctx)` is sprinkled at the top of
every expression `codegen()` — it points the builder at the node's
`DILocation`, and the builder stamps that onto each instruction it creates
until the location changes:

**`Chapter9/src/debug.cpp`**
```cpp
void DebugInfo::emitLocation(ExprAST *AST, IRGenContext &ctx) {
  if (!AST)
    return ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
  llvm::DIScope *Scope = LexicalBlocks.empty() ? (llvm::DIScope*)TheCU
                                               : LexicalBlocks.back();
  ctx.builder->SetCurrentDebugLocation(llvm::DILocation::get(
      Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}
```

## File-by-file: what changed from Chapter8

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

`./build.sh`, then `./run.sh`. The dumped IR for `fib.ks` — unoptimized
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
```

Note the metadata graph at the bottom mirroring the source: compile unit →
subprogram `fib` → parameter `x` → per-instruction locations (and the
`line: 0` entries from the default-location deviation noted above). Piping
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

```sh
ctest --test-dir build                 # just the filecheck tests now
lit -v test/filecheck/debuginfo.k
```
