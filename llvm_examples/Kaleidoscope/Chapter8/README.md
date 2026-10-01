# Chapter 8 — Compiling to Object Files

Everything so far lived and died inside one process: IR was built in memory,
JIT'd, executed, and thrown away. This chapter covers the missing last mile
of a real compiler — emitting an **object file** that the system linker can
combine with code from any other language. Two general concepts first:

**Ahead-of-time compilation and object files.** An object file (`.o`) is
compiled machine code plus the metadata the linker needs: a **symbol table**
(which functions/globals this file defines and which it expects someone else
to provide) and **relocations** (placeholders the linker patches once final
addresses are known). It is the interchange format of the whole systems
world — once Kaleidoscope can produce one, its functions are callable from
C++ exactly like any C library, with no LLVM anywhere at runtime. The
compile-link-run cycle the JIT collapsed comes back apart into its classic
stages:

```
   average.k ──toy──▶ output.o ──clang++ (linker)──▶ ./output ──▶ runs natively
                        │                  ▲
                        │  T _average      │ main.cpp: extern "C" double average(double, double);
                        └──────────────────┘
```

**Targets, triples, and the TargetMachine.** Native code generation must
know *which machine* — instruction set, calling convention, type
sizes/alignment. LLVM identifies a target with a **target triple** of the
form `<arch><sub>-<vendor>-<sys>-<abi>` (see LLVM's
[cross-compilation guide](https://clang.llvm.org/docs/CrossCompilation.html)).
`clang --version | grep Target` shows the host's triple — upstream sees
`x86_64-unknown-linux-gnu`, this machine says `arm64-apple-darwin25.5.0`,
yours may differ again — and `sys::getDefaultTargetTriple()` returns the
same answer programmatically, so nothing has to be hard-coded to target the
current machine. The triple selects a backend in the
`TargetRegistry`, which builds a **`TargetMachine`** — the object that owns
everything target-specific, including the **`DataLayout`** stamped onto the
module so the mid-level IR agrees with the backend about sizes and
alignment. This is the same machinery behind cross-compilation: pass a
different triple, get a different architecture's `.o` from the same IR.

Prerequisites: the language is frozen at
[Chapter7](../Chapter7/README.md)'s state — this chapter changes only what
happens *after* IR exists — and the driver/facade split from
[Chapter3](../Chapter3/README.md) and [Chapter4](../Chapter4/README.md) is
what makes that a small change.
Reference: [Chapter 8: Compiling to Object Code](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl08.html).

The whole chapter is a **driver** change. In Chapter 4 the driver owned a
JIT and fed it one module per definition; here it owns a `TargetMachine`,
lets the session's single module accumulate the whole program, and runs the
backend over it once stdin is drained. The code generator does not change
its interface at all — `codegen.h` is byte-identical to Chapter7's — and the
only thing it needs *from* the backend is, as before, a data layout:

```
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │ toy::Driver  (driver.h / driver.cpp)                                         │
  │   theTargetMachine : llvm::TargetMachine  — the host backend  (replaces JIT) │
  │   ctor: InitializeNativeTarget*, lookupTarget(host triple),                  │
  │         codegen.setDataLayout(TM->createDataLayout()), module triple         │
  │                                                                              │
  │   handleDefinition:      emitFunction ─▶ print        (module keeps growing) │
  │   handleExtern:          emitPrototype ─▶ print                              │
  │   handleTopLevelExpr:    emitFunction ─▶ print ─▶ codegen.eraseFunction()    │
  │   emitObjectFile(name):  legacy PM ◀─ TM->addPassesToEmitFile ─▶ output.o    │
  └──────────────────────────────────────────────────────────────────────────────┘
                     │ emit*() / eraseFunction()      │ currentModule()  ▲ setDataLayout()
                     ▼                                ▼                  │
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │ toy::CodeGenSession  — facade (codegen.h), unchanged since Chapter4          │
  │   emitPrototype()  emitFunction()  currentModule()  eraseFunction()          │
  │   takeModule()  setDataLayout()      (takeModule is never called here)       │
  ├──────────────────────────────────────────────────────────────────────────────┤
  │ CodeGenSession::Impl  (codegen.cpp)  — as Chapter7, plus the redefinition    │
  │   guard from Chapter3 (one module again, so a name can already have a body)  │
  └──────────────────────────────────────────────────────────────────────────────┘
```

(A precise file-by-file diff against Chapter7 is in
[File-by-file](#file-by-file-what-changed-from-chapter7) near the end.)

## The JIT leaves

From `note.txt`: *"Remove JIT from Chapter7 to avoid confusing
TargetMachine."* The JIT and the object emitter are alternative *backends*
for the same IR, and they fight over the same knobs — each wants to own the
module's `DataLayout` and lifetime. So this chapter replaces the one with
the other in the driver rather than juggling both. Consequences:

- **One module accumulates everything.** The driver never calls
  `takeModule()`, so the session's first module is also its last: every
  definition codegens into it — which is exactly what the object emitter
  wants, the whole program in one place, emitted once at EOF.
- **The REPL becomes a batch frontend.** Definitions still print their
  optimized IR as they parse, but nothing executes; when stdin drains,
  `main()` asks the driver to emit the accumulated module.
- **Top-level expressions are vestigial.** They still codegen into
  `__anon_expr` and print, but nothing calls them and an object file has no
  use for them, so — as in Chapter 3, before there was a JIT to run them —
  the driver erases each one after printing. (Upstream leaves them in the
  module, which has a consequence for the *second* one; see
  [Deviations from upstream](#deviations-from-upstream).)
- **Redefinition is an error again.** With one module, a second
  `def foo` finds a `foo` that already has a body. Chapter 3's guard,
  dropped in Chapter 4 when every definition got its own module, comes
  back — the only line that changes in `codegen.cpp`.

The lexer, parser, AST and the code generator's public interface are
untouched; so is `main.cpp`'s shape — it constructs the driver, runs the
loop, and now returns the emitter's exit code.

## The driver owns a `TargetMachine`

Where Chapter 4's constructor created a JIT and asked it for a data layout,
this one looks the host backend up and asks *it*:

**`Chapter8/src/driver.cpp`**
```cpp
Driver::Driver() : parser(lexer) {
  // Set up the host target (used by emitObjectFile's object emission).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  // Pick the machine we are running on, and look its backend up in the
  // TargetRegistry.
  auto TargetTriple = llvm::sys::getDefaultTargetTriple();

  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

  // Print an error and exit if we couldn't find the requested target.
  // This generally occurs if we've forgotten to initialise the
  // TargetRegistry or we have a bogus target triple.
  if (!Target) {
    llvm::errs() << Error;
    std::exit(1);
  }

  auto CPU = "generic";
  auto Features = "";

  llvm::TargetOptions opt;
  theTargetMachine.reset(Target->createTargetMachine(
      TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_));

  // Configure the module we are about to fill for this machine, up front, so
  // the optimizer works with the right sizes and alignments.
  codegen.setDataLayout(theTargetMachine->createDataLayout());
  codegen.currentModule().setTargetTriple(TargetTriple);
}
```

Walking the choices:

- **`CPU = "generic"`, empty features** — these are the two knobs for
  targeting a specific CPU (upstream's example: Intel's Sandylake) or
  feature set (such as SSE); `generic` with no features assumes neither.
  Upstream's way to browse the choices:
  `llvm-as < /dev/null | llc -march=x86 -mattr=help` prints every CPU and
  every feature LLVM knows for a target (`llc -mcpu=help` also works).
- **`Reloc::PIC_`** — position-independent code, required to link into
  modern executables/shared libraries on macOS and most Linux setups.
- **`setDataLayout` / `setTargetTriple`** — in the JIT chapters the
  *JIT's* layout was stamped on each module; now the TargetMachine's is,
  through the very same `setDataLayout()` the Chapter 4 facade grew for the
  JIT. Same principle: IR-level decisions (alignment, struct layout) must
  match the machine that will run the code. Upstream notes this
  configuration isn't strictly necessary for emission to work, but the
  frontend performance guide recommends it: optimizations benefit from
  knowing the target and data layout — which is why it happens here, before
  the first function is generated, rather than just before emission.
- **A missing backend is fatal in the constructor.** A compiler that cannot
  find the machine it is running on has nothing to do, so the driver treats
  it like Chapter 4 treated a JIT that could not be created.

The three handlers are Chapter 7's minus the JIT lines. `handleDefinition`
prints and stops — the module keeps the function; `handleTopLevelExpression`
prints and then removes the anonymous function through the session:

**`Chapter8/src/driver.cpp`**
```cpp
void Driver::handleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = parser.parseTopLevelExpr()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read top-level expression:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      // Remove the anonymous expression. There is no JIT to run it, an object
      // file has no use for it, and leaving it would make the next top-level
      // expression a redefinition of __anon_expr. Through the session, so the
      // analyses cached for it go too.
      codegen.eraseFunction(FnIR);
    }
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}
```

`eraseFunction()` rather than a bare `FnIR->eraseFromParent()` matters
here in a way it did not in Chapter 3: this session runs the optimizer, and
the pass managers cache analyses (the dominator tree, for one) keyed by
`Function*`. Erasing the function behind the session's back would leave
those entries dangling, and the next function to be allocated at the same
address would be optimized against a stale tree — mem2reg then dereferences
freed blocks. The session knows about its caches; the driver does not. That
is the whole reason the removal is a facade method.

## `emitObjectFile()`: IR → `output.o`

The new driver method, run by `main()` after `mainLoop()` returns:

**`Chapter8/src/driver.cpp`**
```cpp
int Driver::emitObjectFile(const char *Filename) {
  // Emit the object code to a file.
  std::error_code EC;
  llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

  if (EC) {
    llvm::errs() << "Could not open file: " << EC.message();
    return 1;
  }

  // Code generation still runs on the legacy pass manager: addPassesToEmitFile
  // fills it with the whole backend (isel, regalloc, scheduling, emission).
  llvm::legacy::PassManager pass;
  auto FileType = llvm::CodeGenFileType::ObjectFile;

  if (theTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
    llvm::errs() << "TheTargetMachine can't emit a file of this type";
    return 1;
  }

  pass.run(codegen.currentModule());   // the entire backend runs here
  dest.flush();

  llvm::outs() << "Wrote " << Filename << "\n";
  return 0;
}
```

**`Chapter8/src/main.cpp`**
```cpp
int main() {
    toy::Driver driver;

    // Run the main "interpreter loop" now.
    driver.mainLoop();

    // Then compile everything it accumulated to an object file.
    return driver.emitObjectFile("output.o");
}
```

Two things worth knowing:

- **`legacy::PassManager`** — a deliberate anachronism: the *optimization*
  pipeline moved to the new pass manager (Chapter 4), but LLVM's **code
  generation** still runs on the legacy interface, so
  `addPassesToEmitFile()` populates a legacy PM. One `pass.run(module)`
  executes the whole backend — instruction selection, register allocation,
  scheduling, object emission. The module it runs over is the session's,
  reached through `currentModule()` — the facade's read-only window is
  enough; emission does not need to own the module.
- **`CodeGenFileType::ObjectFile`** — switching this to `AssemblyFile`
  would emit `.s` text instead; same pipeline, different final writer.

## The payoff: Calling Kaleidoscope from C++

`example/average.txt` is the Kaleidoscope side, `example/main.cpp` the
consumer — `extern "C"` matching how Kaleidoscope emits unmangled names:

**`Chapter8/example/main.cpp`**
```cpp
#include <iostream>

extern "C" {
  double average(double, double);
}

int main() {
  std::cout << "average of 3.0 and 4.0: " << average(3.0, 4.0) << std::endl;
}
```

`./run.sh` performs the whole classic pipeline; a real session (typed
interactively — upstream's `^D` ends the input; piped through
`< example/average.txt`, the same output appears with the prompts bunched
up, as noted in the [top-level README](../README.md#build-and-run)):

```
$ ./build/toy
ready> def average(x y) (x+y) * 0.5;
ready> Read function definition:
define double @average(double %x, double %y) {
entry:
  %addtmp = fadd double %x, %y
  %multmp = fmul double %addtmp, 5.000000e-01
  ret double %multmp
}

ready> ^D
ready> Wrote output.o

$ nm -g output.o
0000000000000000 T _average          # Mach-O prepends '_' to C symbols

$ clang++ example/main.cpp output.o -o output
$ ./output
average of 3.0 and 4.0: 3.5
```

That last line is C++ calling machine code that a compiler we wrote emitted
— no LLVM libraries, no JIT, no interpreter at runtime.

## File-by-file: What changed from Chapter7

The exact split (established with `diff -rq ../Chapter7 .`):

**New files**

| File | Purpose |
| --- | --- |
| `note.txt` | Why the JIT is removed in this chapter. |
| `example/average.txt`, `example/main.cpp` | The Kaleidoscope + C++ link demo. |
| `test/filecheck/objfile.k` | Emission check (see Tests). |
| `test/filecheck/errors.k` | Error recovery without a JIT: errors report, the driver keeps going, and emission still happens at EOF. |

**Removed**: `test/jit_test.cpp` and `test/filecheck/jit.k` — there is no
JIT to execute anything; `errors.k` takes over the error-recovery checks
`jit.k` carried.

**Same filename, byte-identical** — the frontend and the codegen interface
are untouched: `include/ast.h`, `include/codegen.h`, `include/lexer.h`,
`include/log.h`, `include/parser.h`, `src/lexer.cpp`, `src/log.cpp`,
`src/parser.cpp`, `src/extern_d.cpp`, `build.sh`, `cmd.txt`, `mem2reg_ex/*`,
`test/lexer_test.cpp`, `test/parser_test.cpp`, and every previous `.k` file
(`opt.k`, `controlflow.k`, `userops.k`, `mutablevars.k`, `lit.cfg`).

**Same filename, modified** — before → after:

`Chapter8/include/driver.h` / `src/driver.cpp` — the JIT member becomes a
`TargetMachine`, the constructor looks the host backend up instead of
creating a JIT, `handleDefinition` no longer hands the module off,
`handleTopLevelExpression` erases instead of executing, and
`emitObjectFile()` is new:

```cpp
// Chapter7                                       // Chapter8
llvm::ExitOnError ExitOnErr;                       std::unique_ptr<llvm::TargetMachine> theTargetMachine;
std::unique_ptr<orc::KaleidoscopeJIT> theJIT;      int emitObjectFile(const char *filename);

ctor: theJIT = Create();                           ctor: lookupTarget(host) → createTargetMachine
      codegen.setDataLayout(theJIT->getDataLayout()) codegen.setDataLayout(TM->createDataLayout())
handleDefinition:                                  handleDefinition:
  emitFunction + print                               emitFunction + print
  theJIT->addModule(codegen.takeModule())            (module keeps growing)
handleTopLevelExpression:                          handleTopLevelExpression:
  emitFunction + print                               emitFunction + print
  addModule(takeModule(), RT), lookup, call, remove   codegen.eraseFunction(FnIR)
```

`Chapter8/src/main.cpp` — `return 0;` becomes
`return driver.emitObjectFile("output.o");`.

`Chapter8/src/codegen.cpp` — one addition, the redefinition guard in
`emitFunction()` (see Deviations); the header comment records why.

`Chapter8/CMakeLists.txt` — the `jit_test` target is gone; the LLVM
component list is unchanged, with a comment on why `orcjit` stays.

`Chapter8/run.sh` — now the full AOT pipeline: run `toy` on the example,
`nm -g output.o`, link with `clang++`, execute.

`Chapter8/test/codegen_test.cpp` — `RedefinitionInSameModuleIsRefused`
added beside the fresh-module case; the header comment points to
`objfile.k`/`errors.k` for end-to-end coverage now that there is no
`jit_test.cpp`.

## Deviations from upstream

Chapter2–9 keep the tutorial's behavior, quirks included. This chapter
departs in three places. The first is a build-and-link choice; the other
two fix what upstream's Chapter 8 does with input its own example never
feeds it — `cmd.txt` here does, and the object file it produces now
contains exactly the functions the source defines.

### Native-only target initialization

Upstream initializes every backend LLVM was built with, so that *any* triple
works:

```cpp
// upstream (LangImpl08)
InitializeAllTargetInfos();
InitializeAllTargets();
InitializeAllTargetMCs();
InitializeAllAsmParsers();
InitializeAllAsmPrinters();
```

— which is also why its build line changes to `llvm-config --libs all`
("note that the arguments to llvm-config are different to the previous
chapters"). This repo instead keeps the three `InitializeNativeTarget*`
calls from the JIT chapters (`src/driver.cpp`), so the LLVM components
linked stay the same as Chapter7's (`core orcjit native` — `orcjit` is now
linked only because the facade still carries `takeModule()`). The doc's own
rationale cuts both ways: LLVM doesn't require linking in all target
functionality — a JIT needs no assembly printers, and a compiler targeting
only some architectures links only those.

|                            | upstream                        | refactored                                  |
| -------------------------- | ------------------------------- | ------------------------------------------- |
| initialized backends       | every one LLVM was built with   | host architecture only                      |
| link requirement           | `llvm-config --libs all`        | unchanged from Chapter7 (`core orcjit native`) |
| `lookupTarget(<non-native>)` | succeeds → cross-compile      | fails (prints the `lookupTarget` error)     |
| link time / binary size    | slower / larger                 | faster / smaller                            |

### Top-level expressions are erased after printing

Upstream's Chapter 8 handler just generates the anonymous function and
leaves it in the module:

```cpp
// upstream (LangImpl08)
static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
    FnAST->codegen();
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}
```

Here the driver prints it and then calls `codegen.eraseFunction(FnIR)`
(shown above), as Chapter 3 did.

Why: with one module for the whole session, leaving `__anon_expr` behind
means the *second* top-level expression finds a function of that name that
already has a body. Upstream (and the earlier version of this chapter)
then appended a fresh entry block to it, the FPM's SimplifyCFG deleted that
block as unreachable, and the REPL printed the *first* expression's body
again — every top-level expression after the first was silently discarded,
and the object file shipped a stray `__anon_expr` nobody could call. Now
each one is generated, printed and removed; `cmd.txt`'s `fibi(10);` prints
`call double @fibi(...)`, and `nm output.o` lists only the definitions.

|                                  | upstream / earlier version                   | here                                      |
| -------------------------------- | -------------------------------------------- | ----------------------------------------- |
| first top-level expression       | printed, kept in module                      | printed, erased                           |
| second top-level expression      | prints the **first** one's body; own body dropped | prints its own body, erased           |
| `__anon_expr` in `output.o`      | present (`T ___anon_expr`)                   | absent                                    |

### The redefinition guard returns

Chapter 3 refused `def foo` twice ("Function cannot be redefined."); Chapter
4 dropped the check because each definition lived in its own module and the
JIT rejected duplicate symbols instead. Upstream's Chapter 8 keeps Chapter
4's code, so with everything back in one module a second definition silently
corrupts the first — same mechanism as above: a second `entry` block is
appended and then deleted as unreachable, and the printed IR is the old
body. Here the check is back:

**`Chapter8/src/codegen.cpp`**
```cpp
    // One module holds the whole program now, so a name that already has a
    // body here is a genuine redefinition (Chapter 3's guard, gone since the
    // one-module-per-function JIT chapters).
    if (!TheFunction->empty())
      return (llvm::Function *)logErrorV("Function cannot be redefined.");
```

|                                | upstream                                   | here                                        |
| ------------------------------ | ------------------------------------------ | ------------------------------------------- |
| `def f(x) x; def f(x) x+1;`    | second prints the first body; no error     | `Error: Function cannot be redefined.`; first kept |
| `extern foo(a); def foo(b) b;` | declaration filled in (as in Chapter 3)    | same                                        |

`codegen_test.cpp`'s `RedefinitionInSameModuleIsRefused` pins the first row
and checks the original body is untouched.

## Build and run

```sh
./build.sh      # configure + build
./run.sh        # toy < example/average.txt → output.o → nm → clang++ link → ./output
```

(Output as shown above; `output.o` and the linked `output` land in `build/`
— `toy` writes `output.o` into the current working directory.)

## Tests

Same two-scheme setup — rationale and lit mechanics in the [top-level
README](../README.md#testing-the-two-schemes). What Chapter 8 changes:

- **`test/filecheck/objfile.k`** — checks both halves of the chapter's
  contract with two `RUN:` lines:

  ```
  # RUN: %toy < %s 2>&1 | FileCheck %s
  # RUN: test -f output.o
  ```

  The first verifies the IR and the `Wrote output.o` message; the second is
  a plain shell predicate — lit `RUN:` lines are just commands judged by
  exit status, so `test -f` asserts the file actually appeared (in lit's
  scratch directory, thanks to `test_exec_root`).
- **`jit_test.cpp` is gone** — a reminder that tests encode a chapter's
  *architecture*, not just its code: with no JIT in the process, "evaluate
  and compare" is no longer a meaningful operation here. The parser and
  lexer suites are byte-identical to Chapter7's.
- **`codegen_test.cpp`** — Chapter7's suite plus the two module-lifetime
  cases this chapter turns on: `RedefinitionInSameModuleIsRefused` (the
  guard) and `EraseFunctionRemovesItAndItsCachedAnalyses`, which erases an
  optimized function and keeps emitting into the same module fifty times so
  that a stale pass-manager cache entry would be hit.
- **`test/filecheck/errors.k`** — what *replaces* the earlier chapters'
  `jit.k`: with nothing to evaluate, the surviving driver contract is that
  codegen errors are reported, parsing continues, and a failed definition
  doesn't block the final `Wrote output.o`.

```sh
ctest --test-dir build              # everything
lit -v test/filecheck/objfile.k     # emission check
./run.sh                            # the link demo end to end
```
