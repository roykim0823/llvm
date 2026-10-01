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
per whole **module**; a REPL (read-eval-print loop) wants per-function ("as the user types"), which
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
the codegen architecture — the `CodeGenSession` facade over a private `Impl`,
the kind-tag dispatch, the `Driver` — is in [Chapter3](../Chapter3/README.md).
This README covers what Chapter 4 adds.

Concretely, the additions land in exactly the two places Chapter3 set up for
them. The private `Impl` grows the optimizer and the prototype registry; the
public facade gains two methods; and the driver gains the JIT:

```
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │ toy::Driver  (driver.h / driver.cpp)                                         │
  │   + theJIT : orc::KaleidoscopeJIT   — owns all executed code (NEW)           │
  │   ctor: InitializeNativeTarget*, Create() JIT, codegen.setDataLayout(...)    │
  │                                                                              │
  │   handleDefinition:    emitFunction ─▶ print ─▶ addModule(takeModule())      │
  │   handleExtern:        emitPrototype ─▶ print                                │
  │   handleTopLevelExpr:  emitFunction ─▶ print ─▶ addModule(takeModule(), RT)  │
  │                        ─▶ lookup("__anon_expr") ─▶ call ─▶ "Evaluated to"    │
  └──────────────────────────────────────────────────────────────────────────────┘
                     │ emit*()                    │ takeModule()  ▲ setDataLayout()
                     ▼                            ▼               │
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │ toy::CodeGenSession  — facade (codegen.h)                                    │
  │   emitPrototype()  emitFunction()  currentModule()      (as in Chapter3)     │
  │   takeModule() ─▶ ThreadSafeModule    setDataLayout(DataLayout)   (NEW)      │
  ├──────────────────────────────────────────────────────────────────────────────┤
  │ CodeGenSession::Impl  (codegen.cpp)                                          │
  │   theContext / theModule / builder / namedValues        (as in Chapter3)     │
  │   theFPM + 4 analysis managers + instrumentation  — per-function opt  (NEW)  │
  │   functionProtos : map<string, PrototypeAST>      — prototype registry (NEW) │
  │   dataLayout     : optional<DataLayout>           — stamped on each module   │
  │   initializeModule()  — fresh module + fresh pass managers                   │
  │   take()              — tear down, move module+context out, initializeModule │
  │   getFunction(name)   — module lookup, else re-declare from the registry     │
  └──────────────────────────────────────────────────────────────────────────────┘
```

One design consequence dominates this chapter: **once a module is handed to
the JIT it is frozen** — the JIT owns it and nothing can be added to it. So
the REPL opens a **fresh module for every definition and every top-level
expression** (`takeModule()` moves the finished one out and `Impl` opens the
next), and cross-module calls are made to work by re-declaring known
functions into each new module from the `functionProtos` registry.

Where Chapter3's design pays off is *who knows what*. Upstream puts
`TheJIT` next to `TheModule` in the codegen globals and has the parser's
`Handle*` functions drive it. Here the code generator never sees a JIT: it
produces modules and hands them out through `takeModule()`, and the driver —
the one component that owns both — decides whether a module stays resident
(a definition) or is run once and freed (a top-level expression). The only
thing codegen needs *from* the JIT is its data layout, which the driver
passes in once with `setDataLayout()`. That is also why the public header
changes in this chapter and then stays fixed until Chapter 9: `takeModule()`
and `setDataLayout()` are the whole "codegen for a JIT" interface.

(A precise file-by-file diff against Chapter3 is in
[File-by-file](#file-by-file-what-changed-from-chapter3) near the end.)

## The optimizer: FunctionPassManager

First, what `IRBuilder` already buys. `def test(x) 1+2+x;` does *not* come
out as a literal transcription of the AST (which would be `fadd 2.0, 1.0`,
then a second `fadd` with `%x`) — Chapter 3's binary already prints one
pre-folded add:

```
ready> def test(x) 1+2+x;
Read function definition:
define double @test(double %x) {
entry:
  %addtmp = fadd double 3.000000e+00, %x
  ret double %addtmp
}
```

Constant folding is so common and so important that many language
implementors build it into their AST representation. With LLVM you don't
need to: every instruction is created through the builder, and the builder
itself checks each call for a folding opportunity. Upstream's advice is to
always generate code this way — there is no "syntactic overhead" (no
constant checks uglifying every codegen method), and it can dramatically
reduce the amount of IR emitted for constant-heavy inputs (think languages
with macro preprocessors).

The builder's limit is that all of its analysis happens *inline, at the
moment an instruction is created*. Feed it something that needs a view wider
than one instruction and it is stuck — this is Chapter 3's binary again:

```
ready> def test(x) (1+2+x)*(x+(1+2));
Read function definition:
define double @test(double %x) {
entry:
  %addtmp = fadd double 3.000000e+00, %x
  %addtmp1 = fadd double %x, 3.000000e+00   ; same value as %addtmp
  %multmp = fmul double %addtmp, %addtmp1
  ret double %multmp
}
```

Both operands of the multiply are the same `x+3`; we would like
`tmp = x+3; result = tmp*tmp`. No amount of *local* analysis gets there — it
takes two cooperating whole-function transformations: **reassociation**, to
normalize the two adds into one lexical form (`3+x` vs `x+3`), and **common
subexpression elimination (CSE)**, to delete the now-obvious duplicate.
That is exactly what LLVM's **passes** provide, and two upstream design
notes frame how they are used. First, LLVM refuses the "mistaken notion that
one set of optimizations is right for all languages and all situations" —
the implementor decides which passes run, in what order, in what situation.
Second, granularity is part of that choice: a REPL generating one function
at a time wants **per-function** passes run as the user types, while a
hypothetical *static* Kaleidoscope compiler would use exactly this code and
simply defer the optimizer until the whole file is parsed — whole-**module**
passes can then look across as much code as possible (at link time, a
substantial portion of the entire program).

The pass managers are members of the private `Impl`, next to the context and
module they operate on — the tutorial's `TheFPM`, `TheLAM`, ... globals:

**`Chapter4/src/codegen.cpp`**
```cpp
struct CodeGenSession::Impl {
  std::unique_ptr<llvm::LLVMContext> theContext;
  std::unique_ptr<llvm::Module> theModule;
  std::unique_ptr<llvm::IRBuilder<>> builder;
  std::map<std::string, llvm::Value *> namedValues;

  // Chapter 4.2 additions: the per-function optimization pipeline.
  std::unique_ptr<llvm::FunctionPassManager> theFPM;
  std::unique_ptr<llvm::LoopAnalysisManager> theLAM;
  std::unique_ptr<llvm::FunctionAnalysisManager> theFAM;
  std::unique_ptr<llvm::CGSCCAnalysisManager> theCGAM;
  std::unique_ptr<llvm::ModuleAnalysisManager> theMAM;
  std::unique_ptr<llvm::PassInstrumentationCallbacks> thePIC;
  std::unique_ptr<llvm::StandardInstrumentations> theSI;

  // Chapter 4.3 additions: cross-module support.
  std::map<std::string, PrototypeAST> functionProtos;
  std::optional<llvm::DataLayout> dataLayout;

  Impl() { initializeModule(); }
  // ...
};
```

The setup — upstream's `InitializeModuleAndPassManager()`, here
`initializeModule()`, called by the constructor and again after every
hand-off so the pipeline is rebuilt with every fresh module — opens the new
module, then creates the pass manager, the analysis managers, and the four
transform passes, and wires the analysis side up via `PassBuilder`:

**`Chapter4/src/codegen.cpp`**
```cpp
  void initializeModule() {
    // Open a new context and module.
    theContext = std::make_unique<llvm::LLVMContext>();
    theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);

    // set the data layout of the module to match the target machine's data layout.
    if (dataLayout)
      theModule->setDataLayout(*dataLayout);

    // Create a new builder for the module.
    builder = std::make_unique<llvm::IRBuilder<>>(*theContext);

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
  }
```

Why so much scaffolding for four passes: in LLVM's new pass manager,
transform passes don't own their analyses — they *request* them
(`FAM.getResult<...>`), and the four analysis managers (loop / function /
call-graph / module level) compute and cache the results. The
`crossRegisterProxies` call lets a pass at one level query analyses at
another. `StandardInstrumentations` + `PassInstrumentationCallbacks` hook the
standard debugging aids (`-print-after-all`-style logging) into the pipeline.
The four transform passes themselves are, in upstream's words, "a pretty
standard set of 'cleanup' optimizations that are useful for a wide variety
of code" — a good starting place, not a tuned pipeline.

(One line differs from upstream's version of this function: the data layout
comes from `dataLayout`, set by the driver, not from a JIT — see
[Deviations from upstream](#deviations-from-upstream).)

Running the pipeline is one line at the end of `emitFunction()`,
right after `verifyFunction` — every function is optimized the moment it is
generated:

**`Chapter4/src/codegen.cpp`**
```cpp
      // Validate the generated code, checking for consistency.
      llvm::verifyFunction(*TheFunction);

      // Optimize the function.
      theFPM->run(*TheFunction, *theFAM);
```

The `FunctionPassManager` optimizes and updates the `Function*` **in place**
— which is why the driver's later `print` shows optimized IR. With the
pipeline in, the motivating example comes out right:

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

— reassociation made the adds identical, CSE deleted one, saving a
floating-point add on every execution. To explore beyond these four passes,
upstream offers three pointers: the (admittedly incomplete) pass
documentation, the pass list Clang actually runs, and the `opt` tool, which
lets you experiment with pipelines from the command line.

A limit worth seeing: in `def foo2(x) sin(x)*sin(x) + cos(x)*cos(x);` the
optimized IR still contains **two** `sin` calls and **two** `cos` calls. GVN
deduplicates pure computation (`x*y + x*y` → one `fmul`), but a call to a
merely-`declare`d function might have side effects for all LLVM knows, so it
must conservatively keep both calls.

## The JIT: KaleidoscopeJIT

IR is LLVM's "common currency": the same module can be run through
optimization passes (as above), dumped as text or bitcode, compiled to an
assembly file for some target — or JIT-compiled. This chapter takes the last
door: the user keeps entering function bodies as before, but top-level
expressions are now evaluated immediately — type `1 + 2;`, see `3`.

`KaleidoscopeJIT` is the tutorial-provided ORC wrapper, copied from
`llvm/examples/Kaleidoscope/include/KaleidoscopeJIT.h` in the LLVM source
tree (upstream takes it as given here; the separate "Building a JIT"
tutorials dissect and extend it). It lives once at the
repo level (`include/KaleidoscopeJIT.h`, included as
`"../../include/KaleidoscopeJIT.h"`) since every later chapter reuses it. Its
API is three calls: `Create()`, `addModule(TSM)` (compile a module's
functions on first use), and `lookup(name)` (get a symbol's address). Two of
its internals matter to the driver:

- Its symbol resolution is layered: a lookup first searches everything
  already added to the JIT, newest module to oldest; only when nothing
  matches does it fall back to
  `DynamicLibrarySearchGenerator::GetForCurrentProcess` — i.e. **dlsym into
  the running process**. That is the entire "standard library" story:
  `extern sin(x);` works because the JIT'd call binds to libm's `sin` already
  loaded into the process. (Upstream teases where tweaking this rule leads:
  restricting the symbols JIT'd code may see for security, dynamic code
  generation keyed on symbol names, even lazy compilation.)
- `addModule` takes a `ThreadSafeModule`, which owns *both* the module and
  its `LLVMContext` — which is why `takeModule()` moves both out of the
  `Impl` and immediately rebuilds them.

### The driver owns the JIT

Upstream stores `TheJIT` beside the codegen globals and initializes it in
`main()`. Here it is a member of `Driver`, created in the driver's
constructor together with the target setup that all native codegen needs:

**`Chapter4/include/driver.h`**
```cpp
class Driver {
public:
    Driver();
    void mainLoop();

private:
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();

    Lexer lexer;
    Parser parser;
    CodeGenSession codegen;
    llvm::ExitOnError ExitOnErr;
    std::unique_ptr<llvm::orc::KaleidoscopeJIT> theJIT;
};
```

**`Chapter4/src/driver.cpp`**
```cpp
Driver::Driver() : parser(lexer) {
  // Set up the host target so the JIT can emit native code.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  // Initialize the JIT.  This takes ownership of the process control object, and will clean it up on destruction.
  theJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());

  // Every module the session opens must carry the JIT's data layout.
  codegen.setDataLayout(theJIT->getDataLayout());
}
```

The three `InitializeNativeTarget*` calls register the host CPU's backend
(the instruction selector, the assembly printer and parser) with LLVM —
without them, ORC has nothing to compile with. `ExitOnErr` is LLVM's
"unwrap or die" helper for `Expected<T>`/`Error`: a JIT that cannot be
created is not something a REPL can recover from, so upstream's policy of
terminating is kept. The last line is the only coupling between the code
generator and the JIT, and it flows in one direction: the session is told
which data layout (type sizes, alignments, pointer width) to stamp on each
module so the IR matches what the JIT will compile for. From here on the
driver treats `codegen` as a module factory.

### One module per function, and the prototype registry

Why separate modules at all? Upstream derives the design from a failure.
Suppose the anonymous expression shared one module with everything else —
then `RT->remove()`, freeing the anonymous expression after evaluation,
would delete *the whole module*, previously defined functions included, and
the next call to one of them would die:

```
ready> testfunc(4, 10);
Evaluated to 24.000000

ready> testfunc(5, 10);
ready> LLVM ERROR: Program used external function 'testfunc' which could not be resolved!
```

(a transcript of upstream's intermediate, single-module version — the
finished chapter never behaves this way). A **module is the JIT's unit of
allocation**, so the anonymous expression gets a module of its own and can
be freed without collateral damage. Upstream then goes one step further —
*every* function in its own module — because the tutorial's text (which
predates LLVM 9) touted a REPL nicety that required it: adding a function to
the JIT more than once, with `lookup` always returning the newest
definition, i.e. `def foo(x) x+1;` … `def foo(x) x+2;` re-defining `foo`.
**That feature no longer exists.** As upstream's own warning box says, since
LLVM 9 the OrcV2 APIs follow static/dynamic-linker rules and reject
duplicate symbols. In this chapter's binary, a second `def foo(x) ...`
still parses, codegens, and prints its IR — then `addModule` fails with
`Duplicate definition of symbol '_foo'` and `ExitOnErr` terminates the REPL.
The one-module-per-function layout survives as simple uniformity: the
anonymous-expression module has to be separate anyway, so every definition
takes the same path.

Since a JIT'd module is frozen, calling `foo` from a *later* module needs a
fresh `declare double @foo(double)` in that module — the same mechanism C
uses (a declaration in every translation unit, the linker binds them). The
`functionProtos` map keeps the latest prototype for every name, and a new
`getFunction()` helper replaces the plain module lookup everywhere:

**`Chapter4/src/codegen.cpp`**
```cpp
  llvm::Function *getFunction(const std::string &Name) {
    // First, see if the function has already been added to the current module.
    if (auto *F = theModule->getFunction(Name))
      return F;

    // If not, check whether we can codegen the declaration from some existing prototype.
    auto FI = functionProtos.find(Name);
    if (FI != functionProtos.end())
      return emitDeclaration(FI->second);   // re-declare it into the current module

    // If no existing prototype exists, return null.
    return nullptr;
  }
```

`emit(CallExprAST&)` now calls `getFunction(call.getCallee())` where Chapter
3 asked the module directly. The re-declaration itself is Chapter 3's
prototype codegen, renamed `emitDeclaration()` because it now has three
callers — externs, the head of every definition, and this helper:

**`Chapter4/src/codegen.cpp`**
```cpp
  // `extern`: declare it here and remember the prototype for later modules.
  llvm::Function *emitPrototype(PrototypeAST &proto) {
    llvm::Function *F = emitDeclaration(proto);
    functionProtos.insert_or_assign(proto.getName(), proto);
    return F;
  }
```

and `emitFunction()` resolves the `Function` to fill in, then registers its
prototype once the body is in:

**`Chapter4/src/codegen.cpp`**
```cpp
  llvm::Function *emitFunction(FunctionAST &fn) {
    const PrototypeAST &Proto = *fn.getProto();

    // Resolve the Function to fill in: reuse a declaration already in this
    // module (a prior `extern`), otherwise declare it now from this prototype.
    llvm::Function *TheFunction = theModule->getFunction(Proto.getName());
    if (!TheFunction)
      TheFunction = emitDeclaration(Proto);
    // ... entry block, namedValues, body, ret, verify, FPM -- as in Chapter3, then:

      functionProtos.insert_or_assign(Proto.getName(), Proto);   // register on success
      return TheFunction;
```

The registration comes *last*, after the body has been built, verified and
optimized — see [Deviations from upstream](#deviations-from-upstream) for
why that ordering differs from the tutorial's.

(Upstream's registry *takes* the prototype out of the AST with a
`std::move`; here it stores a copy — see
[Deviations from upstream](#deviations-from-upstream).)

Chapter 3's "Function cannot be redefined" guard is gone: with every
definition in a fresh module, the module lookup can only ever find a
*declaration* of the name, never a body. (The JIT is what refuses duplicates
now, as described above.) Chapter 3's extern-then-def argument-name bug
shrinks but does not vanish: an `extern foo(a);` in an *earlier* module is
now re-declared from the newest prototype, so a later `def foo(b) b;` in its
own module codegens against `b` — but an `extern` does not hand its module
to the JIT, so `extern foo(a); def foo(b) b;` typed back to back still share
one module, the declaration is reused, and `b` is still unknown, exactly as
upstream.

### Handing a module to the JIT: `takeModule()`

Upstream's driver does the hand-off inline — build a `ThreadSafeModule`
from the two globals, `addModule` it, call `InitializeModuleAndPassManager()`
— and relies on the fact that reassigning each pass-manager global happens
to destroy the old one *after* the context it referenced has already moved
away. That works, but the ordering is accidental. Here the hand-off is one
method with the ordering spelled out:

**`Chapter4/src/codegen.cpp`**
```cpp
  llvm::orc::ThreadSafeModule take() {
    theSI.reset();
    thePIC.reset();
    theFPM.reset();
    theLAM.reset();
    theFAM.reset();
    theCGAM.reset();
    theMAM.reset();
    builder.reset();
    namedValues.clear();   // held Value*s into the module that is leaving

    llvm::orc::ThreadSafeModule TSM(std::move(theModule), std::move(theContext));
    initializeModule();
    return TSM;
  }
```

Everything that holds a reference *into* the context — the instrumentation
(constructed with `*theContext`), the analysis managers with their cached
results, the builder with its insert point, the symbol table's `Value*`s —
is destroyed or cleared first; then the
module and context move into the `ThreadSafeModule` the JIT will own; then
`initializeModule()` opens the next module with a fresh pipeline. The caller
sees a single expression: `theJIT->addModule(codegen.takeModule())`. In
Chapter 9 the debug-info builder joins the teardown list, which is exactly
the kind of addition this shape is meant to absorb.

### Executing a top-level expression

The full REPL "evaluate" path in the driver, replacing Chapter 3's
`eraseFromParent()`:

**`Chapter4/src/driver.cpp`**
```cpp
      // Create a ResourceTracker to track JIT'd memory allocated to our
      // anonymous expression -- that way we can free it after executing.
      auto RT = theJIT->getMainJITDylib().createResourceTracker();

      // Hand the module to the JIT under the tracker; the session opens a fresh one.
      ExitOnErr(theJIT->addModule(codegen.takeModule(), RT));

      // Search the JIT for the __anon_expr symbol.
      auto ExprSymbol = ExitOnErr(theJIT->lookup("__anon_expr"));

      // Get the symbol's address and cast it to the right type (takes no
      // arguments, returns a double) so we can call it as a native function.
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
      fprintf(stderr, "Evaluated to %f\n", FP());

      // Delete the anonymous expression module from the JIT.
      ExitOnErr(RT->remove());
```

Step by step: the anonymous function's module is handed to the JIT under a
**ResourceTracker** (a handle to everything the JIT allocates for it);
`lookup` triggers actual compilation to native code and returns the address;
the cast to `double (*)()` matches the known signature (no args, returns
double — everything is a double) and works because the JIT emits code for
the **native platform ABI**: a JIT-compiled function is indistinguishable
from one statically linked into the binary, so a raw pointer cast and a
plain C call are all it takes; and
`RT->remove()` frees the JIT'd memory, since `__anon_expr` is
evaluate-once-and-discard. Function *definitions* take the same
`addModule(codegen.takeModule())` path but with no tracker — they must stay
resident so later expressions can call them:

**`Chapter4/src/driver.cpp`**
```cpp
void Driver::handleDefinition() {
  if (auto FnAST = parser.parseDefinition()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read function definition:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      // To Support JIT: hand the finished module to the JIT (it stays resident so
      // later expressions can call the function); the session opens a fresh one.
      ExitOnErr(theJIT->addModule(codegen.takeModule()));
    }
  } else {
    parser.getNextToken();   // error recovery
  }
}
```

`handleExtern()` is Chapter 3's unchanged: `emitPrototype()` now registers
the prototype itself, so there is nothing for the driver to do beyond
printing.

(Upstream ends the chapter with a host-side `putchard` the JIT'd code can
call; this repo adds it in Chapter5 — see
[Deviations from upstream](#deviations-from-upstream).)

## File-by-file: What changed from Chapter3

The JIT class comes from the shared repo-level `include/KaleidoscopeJIT.h`.
The exact split (established with `diff -rq ../Chapter3 .`):

**New files**

| File | Purpose |
| --- | --- |
| `test/jit_test.cpp` | Everything that needs a JIT: a fixture that plays the driver's part (JIT + session + `takeModule()` hand-off), a hand-built `add_two`, a cross-module call, a dlsym-resolved `sin`, and two parameterized suites — hand-built binary expressions and the full text → parse → codegen → JIT → `double` pipeline. |
| `test/filecheck/opt.k` | Replaces Chapter3's `codegen.k`: the printed IR is now *optimized* IR, so the checks assert on what the passes did. |
| `test/filecheck/jit.k` | The evaluate loop end-to-end: `Evaluated to ...` for constants, cross-module calls, and a dlsym-resolved extern — plus the error-recovery checks that Chapter3's `codegen-error.k` covered. |

**Same filename, byte-identical** — safe to skip when reading:
`include/ast.h`, `include/lexer.h`, `include/log.h`, `include/parser.h`,
`src/lexer.cpp`, `src/log.cpp`, `src/parser.cpp`, `src/main.cpp`,
`test/lexer_test.cpp`, `test/parser_test.cpp`, `test/filecheck/lit.cfg`,
`build.sh`. The whole frontend — and its tests — is untouched: neither the
parser nor `main` learned anything about optimization or JITs.

**Same filename, modified** — before → after:

`Chapter4/include/codegen.h` — two methods and two forward declarations:

```cpp
// Chapter3                              // Chapter4
namespace llvm {                          namespace llvm {
class Function;                           class DataLayout;                      // NEW
class Module;                             class Function;
}                                         class Module;
                                          namespace orc { class ThreadSafeModule; }  // NEW
                                          }
class CodeGenSession {                    class CodeGenSession {
  emitPrototype / emitFunction /            emitPrototype / emitFunction / currentModule /
  currentModule / eraseFunction               eraseFunction
  currentModule                             llvm::orc::ThreadSafeModule takeModule();     // NEW
                                            void setDataLayout(const llvm::DataLayout &); // NEW
```

`Chapter4/src/codegen.cpp` — the `Impl` grows the optimizer members, the
`functionProtos` registry and the `dataLayout`; the constructor's three
lines become `initializeModule()`; `take()` and `getFunction()` are new;
prototype codegen is renamed `emitDeclaration()` with `emitPrototype()`
becoming the registering wrapper; two lookups reroute through the registry;
the redefinition guard goes; and the FPM runs on every finished function:

```cpp
// Chapter3                                   // Chapter4
llvm::Function *CalleeF =                      llvm::Function *CalleeF =
    theModule->getFunction(call.getCallee());      getFunction(call.getCallee());

llvm::Function *TheFunction =                  functionProtos.insert_or_assign(Proto.getName(), Proto);
    theModule->getFunction(Proto.getName());   llvm::Function *TheFunction =
if (!TheFunction)                                  getFunction(Proto.getName());
  TheFunction = emitPrototype(Proto);
if (!TheFunction->empty())                     // (guard removed: one definition per module)
  return logErrorV("Function cannot be redefined.");

llvm::verifyFunction(*TheFunction);            llvm::verifyFunction(*TheFunction);
                                               theFPM->run(*TheFunction, *theFAM);
```

`Chapter4/include/driver.h` + `src/driver.cpp` — the driver gains the JIT
and a real constructor; `handleDefinition` and `handleTopLevelExpression`
gain their JIT duties (`handleExtern` and `mainLoop` are Chapter3's, except
that `mainLoop` prints one extra `ready> ` before bootstrapping the first
token, so the prompt appears before the REPL blocks on initial input):

```cpp
// Chapter3                            // Chapter4
Driver() : parser(lexer) {}             Driver();   // InitializeNativeTarget*, Create() JIT, setDataLayout
Lexer lexer;                            Lexer lexer;
Parser parser;                          Parser parser;
CodeGenSession codegen;                 CodeGenSession codegen;
                                        llvm::ExitOnError ExitOnErr;                          // NEW
                                        std::unique_ptr<llvm::orc::KaleidoscopeJIT> theJIT;   // NEW

handleDefinition:                       handleDefinition:
  emitFunction + print                    emitFunction + print
                                          theJIT->addModule(codegen.takeModule())   // stays resident
handleTopLevelExpression:               handleTopLevelExpression:
  emitFunction + print                    emitFunction + print
  FnIR->eraseFromParent()                 addModule(codegen.takeModule(), RT),
                                          lookup("__anon_expr"), call it,
                                          print "Evaluated to %f", RT->remove()
```

`Chapter4/test/codegen_test.cpp` — Chapter3's IR-shape tests carry over
unchanged (they now see optimized IR, which changes nothing they assert).
`FunctionRedefinition` is replaced by the cross-module cases this chapter is
about (plus `FailedBodyIsNotRegistered`): `takeModule()` yields the old module and opens an empty one, a call
into an earlier module gets a fresh declaration, an `extern` survives the
hand-off, a re-`def` in a fresh module is accepted, the newest prototype
wins the argument names, and GVN leaves one `fmul` in `x*y + x*y`.

`Chapter4/CMakeLists.txt` — links two more LLVM components
(`llvm_map_components_to_libnames(llvm_libs core orcjit native)`: the ORC
JIT library and the host-target backend the three `InitializeNativeTarget*`
calls rely on) and registers the `jit_test` executable.

`Chapter4/cmd.txt` — new demo input: the chapter's optimization showcase
plus JIT-evaluated calls and libm externs.

## Deviations from upstream

Chapter2–9 keep the tutorial's behavior, quirks included. This chapter
departs from it in three places, collected here so the main narrative above
can follow the tutorial's order. None of them changes what the REPL prints.

### The prototype registry stores copies, and only of functions that exist

Upstream's registry is `std::map<std::string, std::unique_ptr<PrototypeAST>>`
and `FunctionAST::codegen()` *donates* its prototype to it **before**
generating the body — with a reference kept before the move, because the AST
no longer owns its own prototype after that line:

```cpp
// upstream (LangImpl04)
Function *FunctionAST::codegen() {
  // Transfer ownership of the prototype to the FunctionProtos map, but keep a
  // reference to it for use below.
  auto &P = *Proto;
  FunctionProtos[Proto->getName()] = std::move(Proto);
  Function *TheFunction = getFunction(P.getName());
```

and `HandleExtern()` does the same with `std::move(ProtoAST)` after
codegen. Here the registry is `std::map<std::string, PrototypeAST>` — it
stores **copies**:

**`Chapter4/src/codegen.cpp`**
```cpp
    functionProtos.insert_or_assign(Proto.getName(), Proto);   // copy, newest wins
```

— and it does so **after** the body succeeded, as the last step before
returning.

Why copies: `emitFunction(FunctionAST&)` takes the AST by reference, and a
code generator that quietly hollows out the tree it was handed is a trap
for every other consumer (the driver still holds the `FunctionAST`; the
tests inspect it afterwards). A prototype is just a name and a vector of
argument names, so copying is cheap, and `insert_or_assign` gives the
"newest wins" semantics upstream got from `operator[]` assignment without
needing a default constructor on `PrototypeAST`.

Why register last: upstream registers first because its `getFunction()` is
also how the head of a definition finds or creates its own `Function`. The
price is that a definition whose *body* fails stays registered — and the
next call to it re-declares the name into a fresh module, hands that module
to the JIT, and dies at `lookup` with `Symbols not found`, taking the REPL
with it. Here the head of a definition resolves its `Function` directly
(module lookup, else `emitDeclaration`), so nothing needs the registry until
the function actually exists; a failed body leaves no trace, and a later
call is an ordinary `Error: Unknown function referenced` — `jit.k` checks
exactly that. For successful definitions the two orders are
indistinguishable.

| | upstream (`unique_ptr`, moved, registered first) | here (by value, copied, registered last) |
| --- | --- | --- |
| after `emitFunction`, `fn.getProto()` | dangling — `Proto` is null | still valid |
| registry entry for a re-`def`ined name | replaced | replaced |
| `extern` registration | in `HandleExtern` after codegen | inside `emitPrototype` |
| `def bad(x) y;` then `bad(1.0);` | JIT error, REPL exits | `Error: Unknown function referenced`, REPL continues |
| lifetime of the registered prototype | as long as the registry | as long as the registry |

### The data layout is set from outside, not read from a JIT

Upstream's `InitializeModuleAndPassManager()` reaches into the JIT global for
the layout every module must carry:

```cpp
// upstream (LangImpl04)
static void InitializeModuleAndPassManager() {
  // Open a new context and module.
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("KaleidoscopeJIT", *TheContext);
  TheModule->setDataLayout(TheJIT->getDataLayout());
  ...
```

Here the `Impl` has no JIT to ask. The driver passes the layout in once,
through `setDataLayout()`, and `initializeModule()` applies it to every
module it opens — if it has been given one:

**`Chapter4/src/codegen.cpp`**
```cpp
    // set the data layout of the module to match the target machine's data layout.
    if (dataLayout)
      theModule->setDataLayout(*dataLayout);
```

Why: the code generator should not know a JIT exists (Chapter 8 will drive
the same session with a `TargetMachine` instead), and the unit tests build a
`CodeGenSession` with no JIT at all — their modules simply keep the default
layout, which is fine for checking IR shape. In the REPL the driver always
calls `setDataLayout()` before the first `emit*()`, so every module the JIT
receives carries the JIT's layout exactly as upstream's do.

|                                  | upstream                              | refactored                                   |
| -------------------------------- | ------------------------------------- | -------------------------------------------- |
| where the layout comes from      | `TheJIT->getDataLayout()` in codegen   | `Driver` → `setDataLayout()` once            |
| module opened before any JIT     | impossible (JIT created first)         | default layout (unit tests only)             |
| modules the JIT sees             | JIT's layout                           | JIT's layout                                 |

### `putchard` arrives in Chapter 5, not here

Upstream closes the chapter by extending the language from the *host* side:
since unresolved symbols fall back to dlsym on the running process, any
`extern "C"` function compiled into the interpreter binary is callable from
Kaleidoscope. Its example:

```cpp
// upstream (LangImpl04)
#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}
```

With that in the binary, `extern putchard(x); putchard(120);` prints a
lowercase `x` (ASCII 120) — and the same trick could implement file I/O,
console input, and so on. This repo defers that code to
[Chapter5](../Chapter5/README.md)'s `src/extern_d.cpp` (together with
`printd`), where the chapter's examples first actually call it — Chapter 4's
binary has **no** `putchard`. Upstream's two platform notes, for when you
get there: on Windows the explicit `DLLEXPORT` is required because the
dynamic loader finds symbols via `GetProcAddress`, and on Linux the host
binary must be linked with `-rdynamic` so its symbols stay visible to dlsym
(macOS executables export them by default).

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
"observable through the tool ↔ needs the API" line — and, new this chapter,
a third gtest binary so that each test file matches one component:

- **Numeric results live in gtest, in `jit_test.cpp`.** Executing JIT'd
  code yields a typed `double` in-process — perfect for `EXPECT_DOUBLE_EQ`,
  awkward for textual matching. The fixture does what `Driver` does (create
  the JIT, `setDataLayout`, `takeModule()` into `addModule`), and the tests
  are pure consumers of the facade: a hand-built `add_two` called with a
  real argument, a call across two modules, `sin(0.0)` resolved by dlsym,
  a table of hand-built binary expressions, and a table of *source strings*
  run through the full pipeline (`"2.0 + 3.0 * 4.0"` → `14.0`). That last
  suite is the one place a test touches the parser and the code generator
  together — which is why it is here and not in `parser_test.cpp`, which
  stays byte-identical to Chapter3's (and Chapter2's).
- **Cross-module codegen behavior lives in `codegen_test.cpp`**, with no
  JIT involved: `takeModule()` opens an empty module, a call into an earlier
  module re-declares through the registry, externs survive the hand-off, a
  re-`def` is accepted at the codegen level, and the newest prototype wins.
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
./build/jit_test                   # JIT execution, incl. the full-pipeline table
./build/codegen_test               # IR shape + cross-module codegen
lit -v test/filecheck/opt.k        # the optimized-IR checks
lit -v test/filecheck/jit.k        # the evaluate-loop checks
```
