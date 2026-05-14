# tinylang (Chapter 4) — deltas vs. Chapter 3

This README only documents what Chapter 4 **adds or changes** relative to
[Chapter 3](../../Chapter03/tinylang/README.md). For the frontend pipeline
(Lexer → Parser → Sema → AST), test layout, Doxygen comment conventions,
etc., read the Chapter 3 README first.

Chapter 4 is the chapter where the project gains an LLVM **back end**: a
parsed-and-checked AST is lowered to LLVM IR, then handed to the LLVM
codegen pipeline to produce assembly or an object file.

```
   Ch3 frontend                        Ch4 additions (this chapter)
 ─────────────────────             ──────────────────────────────────────
 .mod ─► Lexer ─► Parser ─► Sema ─► CodeGenerator ─► CGModule ─► CGProcedure ─► llvm::Module ─► back end
                                                                                                  │
                                                                                                  ├─► .s
                                                                                                  ├─► .o
                                                                                                  └─► .ll
```

## Summary of changes

| Area | Status | What changed |
| --- | --- | --- |
| `include/tinylang/CodeGen/` | **new** | `CodeGenerator.h`, `CGModule.h`, `CGProcedure.h` |
| `lib/CodeGen/` | **new** | `CodeGenerator.cpp`, `CGModule.cpp`, `CGProcedure.cpp`, `CMakeLists.txt` |
| `tools/driver/Driver.cpp` | rewritten | adds back-end pipeline: `TargetMachine` setup, `-o` / `-mtriple` / `--emit-llvm` flags, legacy `PassManager` emission |
| `tools/driver/CMakeLists.txt` | modified | links the LLVM back-end components + `tinylangCodeGen` |
| `lib/CMakeLists.txt` | modified | adds `add_subdirectory(CodeGen)` |
| `include/tinylang/AST/AST.h` | modified | `AssignmentStatement::Var` widened from `VariableDeclaration *` to `Decl *` (so VAR formal parameters can appear as the LHS); typo fix `ConstantAccess::geDecl` → `getDecl` |
| `include/tinylang/Basic/Diagnostic.def` | modified | `err_not_yet_implemented` now takes a `{0}` placeholder so one ID serves every NYI site |
| `lib/Sema/Sema.cpp` | modified | `actOnAssignment` gains a `FormalParameterDeclaration` branch + emits `err_not_yet_implemented("other assignments")`; `actOnImport` passes `"module imports"` |
| `test/` | **new** | `CodeGenerator_test.cpp`, `CGModule_test.cpp`, `CGProcedure_test.cpp` |
| `test/CMakeLists.txt` | modified | wires up the three new gtest executables (with extra LLVM libraries via `llvm_map_components_to_libnames`) |
| `examples/` | renamed | `example/` → `examples/`, adds `callgcd.c` (driver for the generated object file) and `ir_example/` |
| `gen_ir.sh` | **new** | one-liner to dump LLVM IR for the GCD example |
| `run.sh` | modified | now builds `Gcd.o`, links against `examples/callgcd.c` via `clang`, and runs the resulting binary |
| `lib/Parser/tinylang.g` | removed | the reference grammar is no longer shipped |

## CodeGen — the new module

The code generator is split into three classes mirroring the frontend's
"thin façade + module + procedure" layering:

| Class | File | Role |
| --- | --- | --- |
| `CodeGenerator` | `include/tinylang/CodeGen/CodeGenerator.h` | Public façade. Owns an `llvm::LLVMContext` and a `TargetMachine`. `run(ModuleDecl, FileName)` creates a fresh `llvm::Module`, sets its triple/data-layout, and delegates to `CGModule`. |
| `CGModule` | `include/tinylang/CodeGen/CGModule.h` | Per-module state: cached primitive LLVM types (`VoidTy`, `Int1Ty`, `Int32Ty`, `Int64Ty`, `Int32Zero`), the `Decl* → GlobalObject*` map, `convertType` (`INTEGER → i64`, `BOOLEAN → i1`), and `mangleName` (length-prefixed, e.g. `Gcd.GCD` → `_t3Gcd3GCD`). |
| `CGProcedure` | `include/tinylang/CodeGen/CGProcedure.h` | Lowers one procedure to an `llvm::Function`. Builds SSA **on the fly** using Braun et al.'s algorithm — see `readLocalVariable` / `readLocalVariableRecursive` / `sealBlock` / `optimizePhi` — so no `alloca`+`mem2reg` detour is needed. Emits IR for expressions (`emitInfixExpr`, `emitPrefixExpr`, `emitExpr`) and statements (`emitStmt` overloads for assignment / IF / WHILE / RETURN; procedure calls are still a `report_fatal_error` stub). |

### SSA construction at a glance

SSA = Static Single Assignment: every value is "written" exactly once.
Source code like `x := x + 1` becomes a *new* SSA value each time. When
control flow merges (after an `IF`, at a `WHILE` header), the two
candidate values for `x` are merged via a **phi** instruction. The trick
used here is that we never first emit allocas + loads/stores and let
`mem2reg` clean it up — instead we build the phis *while we are emitting*
the IR, using a per-block map of "what value does each tinylang variable
currently hold?".

Vocabulary used in the code:

- **`CurrentDef[BB].Defs`** — the per-block map: tinylang `Decl` → its
  current SSA value in basic block `BB`.
- **sealed block** — a block whose every predecessor edge has already
  been emitted. Sealing makes it safe to wire up phi operands.
- **incomplete phi** — a phi created in a block that is not sealed yet;
  its operands are filled in later by `sealBlock`.

```
readVariable(BB, D)
   ├─ local  ─► readLocalVariable(BB, D)
   │              ├─ hit in CurrentDef[BB].Defs  ─► return it
   │              └─ miss ─► readLocalVariableRecursive
   │                            ├─ unsealed     ─► insert *incomplete* phi
   │                            ├─ 1 pred       ─► recurse into pred
   │                            └─ N preds      ─► insert phi + addPhiOperands
   │                                                            └─ optimizePhi
   ├─ global ─► CGM.getGlobal(D) + load/store
   └─ VAR FP ─► load/store via FormalParams[FP]
```

`sealBlock(BB)` is called once every predecessor of `BB` has been emitted —
at that point any `IncompletePhi` is filled in by `addPhiOperands`.

### Worked example — WHILE

`WHILE Cond DO Body END` shows why the seal/unseal dance matters:

```
         Curr                   (block before the loop)
           │
           ▼
      ┌────────┐    false
      │ Cond   │──────────────────┐
      └────────┘                  │
           │ true                 │
           ▼                      ▼
      ┌────────┐            ┌───────────┐
      │ Body   │            │ AfterLoop │
      └────────┘            └───────────┘
           │  back edge to Cond
           └──>──>──>
```

`WhileCondBB` has **two** predecessors: `Curr` (the entry edge) and the
back-edge from `WhileBodyBB`. We cannot seal it until both edges are
emitted, which is why `sealBlock(WhileCondBB)` is called *after* the body
has been lowered. Any variable read inside the body that comes from
"either before the loop or the previous iteration" therefore goes through
the unsealed → IncompletePhi → fixed-in-sealBlock path.

## Driver — what's new

The Chapter-3 driver was a frontend smoke-test: read file(s), parse,
discard. The Chapter-4 driver wires the back end up:

1. registers LLVM's `codegen` flags via `RegisterCodeGenFlags` (this is what
   makes `--filetype=`, `-mcpu`, `-mattr`, `--relocation-model`, etc. work),
2. adds tinylang-specific flags: `-o <file>`, `-mtriple=<triple>`,
   `--emit-llvm`,
3. installs a `printVersion` handler that lists the registered targets,
4. `createTargetMachine` builds the `TargetMachine` from the chosen
   triple/CPU/features,
5. on a successful parse (`numErrors() == 0`), spins up a `CodeGenerator`
   and calls `emit`,
6. `emit` derives an output name from the input (`Foo.mod` → `Foo.s` / `.o`
   / `.ll`), then runs a legacy `PassManager`:
   - `--filetype=asm --emit-llvm` → `createPrintModulePass` (textual IR),
   - otherwise → `TargetMachine::addPassesToEmitFile` (asm or object).

A few `__clang_major__` guards bridge LLVM 17 → 21 API drift
(`endswith` → `ends_with`, `CGFT_*` → `CodeGenFileType::*`, `NoCapture`
attribute → `Captures`, `Triple.getTriple()` → `Triple`).

## Tests added

| File | Coverage |
| --- | --- |
| `CodeGenerator_test.cpp` | End-to-end: hand-builds a `ModuleDeclaration` (no parser), runs `CodeGenerator::run`, checks module name, target triple, data layout, global count, function count. Includes a `LLVMInitializer` singleton that runs `InitializeNativeTarget*` once at process start. |
| `CGModule_test.cpp` | `initialize()` sets the cached primitive types; `convertType()` maps `INTEGER` / `BOOLEAN` to the right widths; `mangleName()` follows the `_t<len><name>…` scheme up the `Decl` chain; `run()` creates one `GlobalVariable` per top-level `VariableDeclaration`, retrievable via `getGlobal()`. |
| `CGProcedure_test.cpp` | Parametric tables for: function signatures (value + VAR params, void / `INTEGER` return), every infix operator's emitted instruction (`add nsw` … `icmp sge`, `and`, `or`), and the basic-block labels for IF / IF-ELSE / WHILE. Each function is run through `llvm::verifyFunction`. |

The CodeGen tests pull in extra LLVM libraries via
`llvm_map_components_to_libnames(... Support Core IRReader Target AsmParser
native)` (see `test/CMakeLists.txt`).

## AST / Sema / Diagnostic changes

- **`AssignmentStatement::Var`** is now `Decl *`. The old type was
  `VariableDeclaration *`, which doesn't cover VAR formal parameters — and
  Ch04 code generation needs to lower `param := expr` (a store through the
  reference) the same way it lowers an ordinary variable store.
- **`Sema::actOnAssignment`** grew a `FormalParameterDeclaration` branch
  that does the same type compatibility check and builds an
  `AssignmentStatement(FP, E)`. The previous Ch03 silent `// TODO`
  fallthrough is now an explicit
  `err_not_yet_implemented("other assignments")`.
- **`Sema::actOnImport`** now reports
  `err_not_yet_implemented("module imports")` instead of the bare ID
  (`Diagnostic.def` was updated so the same ID takes the `{0}` argument).
- **`ConstantAccess::geDecl()` typo** is fixed to `getDecl()` (the Ch03
  spelling is retained as-is for backward compatibility with the Ch03
  tests).

## Build & run

```bash
# from Chapter04/tinylang
./build.sh                # configures and builds via CMake/Ninja
./gen_ir.sh               # emit textual IR for examples/Gcd.mod to stdout
./run.sh                  # build Gcd.o, link with examples/callgcd.c, run
./test.sh                 # ctest in ./build/test (includes the 3 new suites)
```

Useful flags for `./build/tools/driver/tinylang`:

```
  -o <file>            output filename (default: derived from input)
  -mtriple=<triple>    override the target triple
  --filetype=asm|obj   output format (default: asm)
  --emit-llvm          with --filetype=asm, write textual LLVM IR (.ll)
  --version            tinylang version + default target + registered targets
```

## Where to read next

Once the Doxygen output has been generated, these are the entry points
worth following in order:

- `tinylang::CodeGenerator` — the façade entry point used by the driver.
- `tinylang::CGModule` — module-level state, type cache, name mangling.
- `tinylang::CGProcedure` — per-procedure lowering and SSA construction.
- `tinylang::CGProcedure::readLocalVariableRecursive` — the heart of the
  SSA algorithm.
- `tinylang::CGProcedure::optimizePhi` — trivial-phi elimination.
- `tinylang::AssignmentStatement` — read the `\note` about Ch04's
  `Decl *` widening.

## Doxygen

The Chapter-3 Doxygen setup is reused. The new `docs/Doxyfile` differs only
in `PROJECT_NAME` and `PROJECT_BRIEF`; everything else (`INPUT`,
`EXCLUDE_SYMBOLS`, `JAVADOC_AUTOBRIEF`, `HAVE_DOT`, …) is identical, and
`docs/.gitignore` excludes the generated `html/` / `latex/` etc.

```bash
# from Chapter04/tinylang
doxygen docs/Doxyfile
open docs/html/index.html      # macOS
```

Only the **new or changed** code carries Doxygen comments in this chapter:

- `include/tinylang/CodeGen/*.h` — `\file`, class brief, member briefs.
- `lib/CodeGen/*.cpp` — `\file` headers; key SSA-construction methods get a
  one-line brief.
- `tools/driver/Driver.cpp` — `\file` brief describing the back-end
  pipeline added in Ch04, plus briefs on `printVersion`,
  `createTargetMachine`, `emit`.
- `include/tinylang/AST/AST.h` — `AssignmentStatement` carries a `\note`
  about the `Decl *` widening.
- `include/tinylang/Basic/Diagnostic.def` — note on the new `{0}`
  placeholder for `err_not_yet_implemented`.
- `lib/Sema/Sema.cpp` — `actOnAssignment` notes the new FP branch.
- `test/CGModule_test.cpp`, `test/CGProcedure_test.cpp`,
  `test/CodeGenerator_test.cpp` — `\file` summaries of what each test
  fixture covers.

Files inherited unchanged from Ch03 (Lexer, Parser, Scope, …) are *not*
re-annotated here; their documentation lives in the Chapter 3 tree.
