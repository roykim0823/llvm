# Chapter 4 — JIT and Optimizer Support

[Chapter3](../Chapter3/README.md) produced correct but naive LLVM IR and
only *printed* it. This chapter makes the language real in two independent
ways — the IR gets **optimized**, and then it gets **executed**. Two general
concepts first:

**Optimization passes.** An optimizer is not one monolithic algorithm but a
pipeline of **passes**, each a self-contained IR-to-IR transformation
(constant folding, dead code removal, ...) that leaves the program's meaning
intact. Passes compose: one pass's cleanup exposes the next pass's
opportunity, so order matters. `IRBuilder`'s constant folding (seen in
Chapter 3) is the weakest form — it is *local*, looking only at the one
instruction being created. A pass sees a whole function at once, so it can
spot what no single-instruction view can: that `(1+2+x)*(x+(1+2))` computes
the same `x+3` twice. Passes come with a granularity — per **function** or
per whole **module**; a REPL wants per-function ("as the user types"), which
is what a `FunctionPassManager` runs. Alongside *transform* passes LLVM has
*analysis* passes (dominators, alias info, ...) whose results transforms
consume and which a set of **analysis managers** computes and caches.

**JIT compilation.** A classical (ahead-of-time) compiler writes native code
to an executable for later. A **JIT** (just-in-time) compiler translates IR
to native machine code *in memory, inside the running process*, and hands
back a raw function pointer — callable like any C function. That collapses
the whole compile-link-run cycle into one REPL keystroke: type `4+5;`, the
expression becomes `__anon_expr`, the JIT compiles it, the driver calls it
and prints `Evaluated to 9.000000`. LLVM's JIT library is **ORC** (On-Request
Compilation); the tutorial wraps it in a ~100-line `KaleidoscopeJIT` class.

```
              Chapter 3                    │             Chapter 4
                                           │
  AST ──codegen──▶ raw IR ──▶ print        │   AST ──codegen──▶ raw IR
                                           │         │ FunctionPassManager
                                           │         ▼ (4 transform passes)
                                           │   optimized IR ──▶ print
                                           │         │ KaleidoscopeJIT (ORC)
                                           │         ▼
                                           │   native code ──call──▶ "Evaluated to ..."
```

Reference: [Chapter 4: Adding JIT and Optimizer Support](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl04.html).
Lexer, parser, and the AST are untouched (see [Chapter2](../Chapter2/README.md));
the codegen basics are in [Chapter3](../Chapter3/README.md). This README
covers what Chapter 4 adds.

Concretely, the additions all live in the codegen layer and the driver:

```
  handleDefinition / handleTopLevelExpression (parser.cpp)
        │ AST->codegen(ctx)
        ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ toy::IRGenContext  — grown considerably this chapter             │
  │   theContext/theModule/builder/namedValues   (as in Chapter3)   │
  │   theFPM + 4 analysis managers  — per-function optimization      │
  │   theJIT : orc::KaleidoscopeJIT — owns all executed code         │
  │   functionProtos : map<string, PrototypeAST> — prototype registry│
  │   InitializeModuleAndPassManager() — fresh module + fresh FPM    │
  └──────────────────────────────────────────────────────────────────┘
        │ FunctionAST::codegen ends with theFPM->run(F)   (optimize)
        ▼
  module handed to theJIT (addModule) ── fresh module opened ──▶ next input
        │ lookup("__anon_expr") → double(*)()
        ▼
  fprintf("Evaluated to %f\n", FP())
```

One design consequence dominates this chapter: **once a module is handed to
the JIT it is frozen** — the JIT owns it and nothing can be added to it. So
the REPL opens a **fresh module for every definition and every top-level
expression** (`InitializeModuleAndPassManager()`), and cross-module calls are
made to work by re-declaring known functions into each new module from the
`functionProtos` registry.

(A precise file-by-file diff against Chapter3 is in
[File-by-file](#file-by-file-what-changed-from-chapter3) near the end.)

## The optimizer: FunctionPassManager

The motivating example — `IRBuilder` folded `1+2` to `3.0` already in
Chapter 3, but only a real pass pipeline can notice that the two operands of
the multiply are *the same value*:

```
ready> def test(x) (1+2+x)*(x+(1+2));
Read function definition:
define double @test(double %x) {
entry:
  %addtmp = fadd double %x, 3.000000e+00
  %multmp = fmul double %addtmp, %addtmp     ; one add, squared — not two
  ret double %multmp
}
```

The setup (in `InitializeModuleAndPassManager()`, so it is rebuilt with every
fresh module) creates the pass manager, the analysis managers, and the four
transform passes, then wires the analysis side up via `PassBuilder`:

**`Chapter4/include/ir_gen_ctx.h`**
```cpp
// Create new pass and analysis managers.
theFPM = std::make_unique<llvm::FunctionPassManager>();
theLAM = std::make_unique<llvm::LoopAnalysisManager>();
theFAM = std::make_unique<llvm::FunctionAnalysisManager>();
theCGAM = std::make_unique<llvm::CGSCCAnalysisManager>();
theMAM = std::make_unique<llvm::ModuleAnalysisManager>();
thePIC = std::make_unique<llvm::PassInstrumentationCallbacks>();
theSI = std::make_unique<llvm::StandardInstrumentations>(*theContext,
                                             /*DebugLogging*/ true);
theSI->registerCallbacks(*thePIC, theMAM.get());

// Add transform passes.
theFPM->addPass(llvm::InstCombinePass());   // peephole, bit-twiddling
theFPM->addPass(llvm::ReassociatePass());   // reorder by ranks: x+3 == 3+x
theFPM->addPass(llvm::GVNPass());           // value numbering: kill duplicates
theFPM->addPass(llvm::SimplifyCFGPass());   // delete dead blocks, merge blocks

// Register analysis passes used in these transform passes.
llvm::PassBuilder PB;
PB.registerModuleAnalyses(*theMAM);
PB.registerFunctionAnalyses(*theFAM);
PB.crossRegisterProxies(*theLAM, *theFAM, *theCGAM, *theMAM);
```

Why so much scaffolding for four passes: in LLVM's new pass manager,
transform passes don't own their analyses — they *request* them
(`FAM.getResult<...>`), and the four analysis managers (loop / function /
call-graph / module level) compute and cache the results. The
`crossRegisterProxies` call lets a pass at one level query analyses at
another. `StandardInstrumentations` + `PassInstrumentationCallbacks` hook the
standard debugging aids (`-print-after-all`-style logging) into the pipeline.

Running the pipeline is one line at the end of `FunctionAST::codegen()`,
right after `verifyFunction` — every function is optimized the moment it is
generated:

**`Chapter4/src/codegen.cpp`**
```cpp
    // Validate the generated code, checking for consistency.
    llvm::verifyFunction(*TheFunction);

    // Optimize the function.
    ctx.theFPM->run(*TheFunction, *ctx.theFAM);
```

A limit worth seeing: in `def foo2(x) sin(x)*sin(x) + cos(x)*cos(x);` the
optimized IR still contains **two** `sin` calls and **two** `cos` calls. GVN
deduplicates pure computation (`x*y + x*y` → one `fmul`), but a call to a
merely-`declare`d function might have side effects for all LLVM knows, so it
must conservatively keep both calls.

## The JIT: KaleidoscopeJIT

`KaleidoscopeJIT` is the tutorial-provided ORC wrapper — it lives once at the
repo level (`include/KaleidoscopeJIT.h`, included as
`"../../include/KaleidoscopeJIT.h"`) since every later chapter reuses it. Its
API is three calls: `Create()`, `addModule(TSM)` (compile a module's
functions on first use), and `lookup(name)` (get a symbol's address). Two of
its internals matter to the driver:

- It resolves symbols it doesn't own via
  `DynamicLibrarySearchGenerator::GetForCurrentProcess` — i.e. **dlsym into
  the running process**. That is the entire "standard library" story:
  `extern sin(x);` works because the JIT'd call binds to libm's `sin` already
  loaded into the process.
- `addModule` takes a `ThreadSafeModule`, which owns *both* the module and
  its `LLVMContext` — which is why the driver moves both out of the
  `IRGenContext` and immediately rebuilds them.

The JIT machinery is initialized once in the `IRGenContext` constructor —
including the target setup that all native codegen needs:

**`Chapter4/include/ir_gen_ctx.h`**
```cpp
IRGenContext() {
    llvm::InitializeNativeTarget();            // the host's codegen backend
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    theJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
    InitializeModuleAndPassManager();          // open the first module
}
```

`InitializeModuleAndPassManager()` also stamps the JIT's data layout onto
each fresh module (`theModule->setDataLayout(theJIT->getDataLayout())`) so
the IR is generated with the exact type sizes/alignment the JIT will compile
for.

### One module per function, and the prototype registry

Since a JIT'd module is frozen, calling `foo` from a *later* module needs a
fresh `declare double @foo(double)` in that module — the same mechanism C
uses (a declaration in every translation unit, the linker binds them). The
`functionProtos` map keeps the latest `PrototypeAST` for every name, and a
new `getFunction()` helper replaces the plain module lookup everywhere:

**`Chapter4/src/codegen.cpp`**
```cpp
static llvm::Function *getFunction(std::string Name, IRGenContext &ctx) {
  // First, see if the function has already been added to the current module.
  if (auto *F = ctx.theModule->getFunction(Name))
    return F;

  // If not, check whether we can codegen the declaration from some existing prototype.
  auto FI = ctx.functionProtos.find(Name);
  if (FI != ctx.functionProtos.end())
    return FI->second->codegen(ctx);   // re-declare it into the current module

  // If no existing prototype exists, return null.
  return nullptr;
}
```

`FunctionAST::codegen()` now *donates* its prototype to the registry before
generating the body (note the reference kept before the move — the AST no
longer owns its own prototype after this line):

**`Chapter4/src/codegen.cpp`**
```cpp
  auto &P = *Proto;
  ctx.functionProtos[Proto->getName()] = std::move(Proto);
  llvm::Function *TheFunction = getFunction(P.getName(), ctx);
```

and `handleExtern()` registers extern prototypes the same way. A pleasant
side effect: because lookups now go through the registry (newest prototype
wins) and each definition lives in its own module, Chapter 3's
extern-then-def argument-name bug is gone.

### Executing a top-level expression

The full REPL "evaluate" path, replacing Chapter 3's `eraseFromParent()`:

**`Chapter4/src/parser.cpp`**
```cpp
// Create a ResourceTracker to track JIT'd memory allocated to our
// anonymous expression -- that way we can free it after executing.
auto RT = ctx.theJIT->getMainJITDylib().createResourceTracker();

auto TSM = llvm::orc::ThreadSafeModule(std::move(ctx.theModule), std::move(ctx.theContext));
ctx.ExitOnErr(ctx.theJIT->addModule(std::move(TSM), RT));
ctx.InitializeModuleAndPassManager();          // open a fresh module for what's next

// Search the JIT for the __anon_expr symbol.
auto ExprSymbol = ctx.ExitOnErr(ctx.theJIT->lookup("__anon_expr"));

// Get the symbol's address and cast it to the right type (takes no
// arguments, returns a double) so we can call it as a native function.
double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
fprintf(stderr, "Evaluated to %f\n", FP());

// Delete the anonymous expression module from the JIT.
ctx.ExitOnErr(RT->remove());
```

Step by step: the anonymous function's module is handed to the JIT under a
**ResourceTracker** (a handle to everything the JIT allocates for it);
`lookup` triggers actual compilation to native code and returns the address;
the cast to `double (*)()` matches the known signature (no args, returns
double — everything is a double); the call executes real machine code; and
`RT->remove()` frees the JIT'd memory, since `__anon_expr` is
evaluate-once-and-discard. Function *definitions* take the same
`addModule` + fresh-module path but with no tracker — they must stay resident
so later expressions can call them.

## File-by-file: what changed from Chapter3

No new source files — the JIT class comes from the shared repo-level
`include/KaleidoscopeJIT.h` (taken from upstream LLVM). The exact split:

**New files**

| File | Purpose |
| --- | --- |
| `test/filecheck/opt.k` | Replaces Chapter3's `codegen.k`: the printed IR is now *optimized* IR, so the checks assert on what the passes did. |
| `test/filecheck/jit.k` | The evaluate loop end-to-end: `Evaluated to ...` for constants, cross-module calls, and a dlsym-resolved extern — plus the error-recovery checks that Chapter3's `codegen-error.k` covered. |

**Same filename, byte-identical** — safe to skip when reading:
`include/ast.h`, `include/parser.h`, `include/lexer.h`, `include/log.h`,
`src/lexer.cpp`, `src/log.cpp`, `src/main.cpp` (all Chapter 4 state hides
inside `IRGenContext`, so even `main` is unchanged), `test/lexer_test.cpp`,
`test/filecheck/lit.cfg`, `build.sh`.

**Same filename, modified** — before → after:

`Chapter4/include/ir_gen_ctx.h` — the big one. `IRGenContext` grows the
optimizer members (`theFPM` + four analysis managers + instrumentation), the
JIT members (`theJIT`, `ExitOnErr`), the `functionProtos` registry, and the
`InitializeModuleAndPassManager()` method; the constructor now initializes
the native target and creates the JIT:

```cpp
// Chapter3: 4 members,                // Chapter4: + optimizer, JIT, registry
// ctor opens one module forever        // ctor opens the FIRST of many modules
theContext / theModule /                theContext / theModule / builder / namedValues
builder / namedValues                   theFPM, theLAM, theFAM, theCGAM, theMAM, thePIC, theSI
                                        theJIT, ExitOnErr
                                        functionProtos
                                        InitializeModuleAndPassManager()
```

`Chapter4/src/codegen.cpp` — adds the `getFunction()` helper; two lookups
reroute through it, and the FPM runs on every finished function:

```cpp
// Chapter3                                   // Chapter4
llvm::Function *CalleeF =                      llvm::Function *CalleeF =
    ctx.theModule->getFunction(Callee);            getFunction(Callee, ctx);

llvm::Function *TheFunction =                  auto &P = *Proto;
    ctx.theModule->getFunction(                ctx.functionProtos[P.getName()] = std::move(Proto);
        Proto->getName());                     llvm::Function *TheFunction =
if (!TheFunction)                                  getFunction(P.getName(), ctx);
  TheFunction = Proto->codegen(ctx);

llvm::verifyFunction(*TheFunction);            llvm::verifyFunction(*TheFunction);
                                               ctx.theFPM->run(*TheFunction, *ctx.theFAM);
```

`Chapter4/src/parser.cpp` — the three `handle*` wrappers gain their JIT
duties (all `parse*` methods untouched):

```cpp
// Chapter3                            // Chapter4
handleDefinition:                       handleDefinition:
  codegen + print                         codegen + print
                                          theJIT->addModule(module)   // stays resident
                                          InitializeModuleAndPassManager()
handleExtern:                           handleExtern:
  codegen + print                         codegen + print
                                          functionProtos[name] = std::move(ProtoAST)
handleTopLevelExpression:               handleTopLevelExpression:
  codegen + print                         codegen + print
  FnIR->eraseFromParent()                 addModule under ResourceTracker,
                                          lookup("__anon_expr"), call it,
                                          print "Evaluated to %f", RT->remove()
```

(`mainLoop()` also prints one extra `ready> ` before bootstrapping the first
token, so the prompt appears before the REPL blocks on initial input.)

`Chapter4/test/codegen_test.cpp` — adds a `JIT` test (build `add_two` by
hand, `addModule`, `lookup`, call `FP(5.5)`, expect `7.5`). The Chapter3
substring checks for *what instructions look like* stay, but a comment notes
that optimization-shape assertions moved to `opt.k`, where FileCheck
expresses instruction counts and def-use structure directly.

`Chapter4/test/parser_test.cpp` — adds `JITExecutionParamTest`: a
parameterized suite running the **full pipeline** (parse → codegen → JIT →
execute) per expression and comparing the numeric result
(`EXPECT_DOUBLE_EQ`), e.g. `"2.0 + 3.0 * 4.0"` → `14.0`.

`Chapter4/CMakeLists.txt` — links two more LLVM components:
`llvm_map_components_to_libnames(llvm_libs core orcjit native)` (the ORC JIT
library and the host-target backend the three `InitializeNativeTarget*`
calls rely on).

`Chapter4/cmd.txt` — new demo input: the chapter's optimization showcase
plus JIT-evaluated calls and libm externs.

## Build and run

Same recipe (`./build.sh`, or cmake + Ninja by hand). The REPL now answers
back — a session with the highlights of `cmd.txt` (every `def`/`extern` also
prints its IR; a few repetitive entries are elided):

```
ready> def test(x) (1+2+x)*(x+(1+2));
Read function definition:
define double @test(double %x) {
entry:
  %addtmp = fadd double %x, 3.000000e+00
  %multmp = fmul double %addtmp, %addtmp
  ret double %multmp
}

ready> 4+5;
Read top-level expression:
define double @__anon_expr() {
entry:
  ret double 9.000000e+00
}

Evaluated to 9.000000

ready> def testfunc(x y) x + y*2;
Read function definition:
define double @testfunc(double %x, double %y) {
entry:
  %multmp = fmul double %y, 2.000000e+00
  %addtmp = fadd double %x, %multmp
  ret double %addtmp
}

ready> testfunc(4, 10);
Read top-level expression:
define double @__anon_expr() {
entry:
  %calltmp = call double @testfunc(double 4.000000e+00, double 1.000000e+01)
  ret double %calltmp
}

Evaluated to 24.000000                   ; cross-module call: testfunc lives in
                                         ; an earlier, already-JIT'd module
ready> extern sin(x);
Read extern:
declare double @sin(double)

ready> sin(1.0);
Read top-level expression:
define double @__anon_expr() {
entry:
  %calltmp = call double @sin(double 1.000000e+00)
  ret double 0x3FEAED548F090CEE          ; LLVM knows libm's sin(): the RESULT
}                                        ; folds to a constant, but no pass in
                                         ; this pipeline deletes the (possibly
Evaluated to 0.841471                    ; side-effecting) call itself; sin
                                         ; binds via dlsym into libm at JIT time
ready> def foo2(x) sin(x)*sin(x) + cos(x)*cos(x);
Read function definition:
define double @foo2(double %x) {
entry:
  %calltmp = call double @sin(double %x)
  %calltmp1 = call double @sin(double %x)
  %multmp = fmul double %calltmp, %calltmp1
  %calltmp2 = call double @cos(double %x)
  %calltmp3 = call double @cos(double %x)
  %multmp4 = fmul double %calltmp2, %calltmp3
  %addtmp = fadd double %multmp, %multmp4    ; note: GVN kept all four calls —
  ret double %addtmp                         ; extern calls may have side effects
}

ready> foo2(4.0);
Read top-level expression:
define double @__anon_expr() {
entry:
  %calltmp = call double @foo2(double 4.000000e+00)
  ret double %calltmp
}

Evaluated to 1.000000
```

(Piping non-interactively interleaves extra `ready>` prompts, as noted in
the [top-level README](../README.md#build-and-run).)

## Tests

Same two-scheme setup — see the [top-level
README](../README.md#testing-the-two-schemes) for the rationale and lit
mechanics. What Chapter 4 changes is *which layer checks what*, exactly along the
"observable through the tool ↔ needs the API" line:

- **Numeric results moved into gtest.** Executing JIT'd code yields a typed
  `double` in-process — perfect for `EXPECT_DOUBLE_EQ`, awkward for textual
  matching. `parser_test.cpp`'s `JITExecutionParamTest` runs
  parse → codegen → JIT → call for a table of expressions;
  `codegen_test.cpp`'s `JIT` test does the same for a hand-built function
  with a real argument.
- **IR shape moved into FileCheck.** `opt.k` asserts on the *optimized* IR
  the driver prints: GVN merging `x*y + x*y` into a single `fmul`
  (`CHECK: fmul` ... `CHECK-NEXT: fadd` of the same capture), commuted adds
  deduplicating, and all-constant expressions folding to a bare `ret` —
  enforced with `CHECK-NOT: fadd`, FileCheck's way of asserting an
  instruction is *absent*. Chapter3's raw-IR checks would all fail here;
  that is the point — the printed IR now has the passes' fingerprints on it.
- **The evaluate loop itself stays in FileCheck.** `jit.k` pins the
  user-visible REPL contract: `Evaluated to 9.000000` for a constant, a
  cross-module call through the `functionProtos` re-declaration path, a
  dlsym-resolved `sin(1.0)`, and — carried over from Chapter3's
  `codegen-error.k` — that codegen errors are reported without killing the
  driver. (Numeric *precision* is the gtest layer's job; the `.k` file checks
  the six-decimal strings the driver actually prints.)

```sh
ctest --test-dir build             # everything
./build/parser_test                # includes the JIT execution table
lit -v test/filecheck/opt.k        # the optimized-IR checks
lit -v test/filecheck/jit.k        # the evaluate-loop checks
```
