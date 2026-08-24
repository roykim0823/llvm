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
happens *after* IR exists.
Reference: [Chapter 8: Compiling to Object Code](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl08.html).

(A precise file-by-file diff against Chapter7 is in
[File-by-file](#file-by-file-what-changed-from-chapter7) near the end.)

## The JIT leaves

From `note.txt`: *"Remove JIT from Chapter7 to avoid confusing
TargetMachine."* The JIT and the object emitter are alternative *backends*
for the same IR, and they fight over the same knobs — each wants to own the
module's `DataLayout` and lifetime. So this chapter comments the JIT out
(`theJIT` is never created; the `addModule`/`lookup`/`Evaluated to` blocks
in the handlers are commented) rather than juggling both. Consequences:

- **One module accumulates everything.** Without the JIT's
  freeze-and-reopen cycle, `InitializeModuleAndPassManager()` runs once and
  every definition codegens into the same module — which is exactly what the
  object emitter wants: the whole program in one place, emitted once at EOF.
- **The REPL becomes a batch frontend.** Definitions still print their
  optimized IR as they parse, but nothing executes; when stdin drains,
  `main()` hands the accumulated module to `compile_obj()`.
- **Top-level expressions are vestigial.** They still codegen into
  `__anon_expr`, but nothing calls it — and a *second* top-level expression
  hits a quirk: `getFunction` finds the existing `__anon_expr` (body and
  all), appends a fresh entry block to it, and the FPM's SimplifyCFG then
  deletes that block as unreachable — later expressions are silently
  discarded. Harmless for this chapter's `def`-only workflow, but worth
  knowing; upstream Chapter 8 has the same behavior and the same
  indifference to it.

## `compile_obj()`: IR → `output.o`

The new function in `main.cpp`, run after `mainLoop()` returns:

**`Chapter8/src/main.cpp`**
```cpp
int compile_obj(toy::IRGenContext& ctx) {
  auto TargetTriple = llvm::sys::getDefaultTargetTriple();
  ctx.theModule->setTargetTriple(TargetTriple);

  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
  if (!Target) {                        // bad triple, or targets not initialized
    llvm::errs() << Error;
    return 1;
  }

  auto CPU = "generic";
  auto Features = "";
  llvm::TargetOptions opt;
  auto TheTargetMachine = Target->createTargetMachine(
      TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_);

  ctx.theModule->setDataLayout(TheTargetMachine->createDataLayout());

  auto Filename = "output.o";
  std::error_code EC;
  llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);
  ...
  llvm::legacy::PassManager pass;
  auto FileType = llvm::CodeGenFileType::ObjectFile;
  if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
    llvm::errs() << "TheTargetMachine can't emit a file of this type";
    return 1;
  }

  pass.run(*ctx.theModule);             // the entire backend runs here
  dest.flush();
  llvm::outs() << "Wrote " << Filename << "\n";
  return 0;
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
- **`setDataLayout` / `setTargetTriple` on the module** — in the JIT
  chapters the *JIT's* layout was stamped on each module; now the
  TargetMachine's is. Same principle: IR-level decisions (alignment,
  struct layout) must match the machine that will run the code. Upstream
  notes this configuration isn't strictly necessary for emission to work,
  but the frontend performance guide recommends it: optimizations benefit
  from knowing the target and data layout.
- **`legacy::PassManager`** — a deliberate anachronism: the *optimization*
  pipeline moved to the new pass manager (Chapter 4), but LLVM's **code
  generation** still runs on the legacy interface, so
  `addPassesToEmitFile()` populates a legacy PM. One `pass.run(module)`
  executes the whole backend — instruction selection, register allocation,
  scheduling, object emission.
- **`CodeGenFileType::ObjectFile`** — switching this to `AssemblyFile`
  would emit `.s` text instead; same pipeline, different final writer.

### Deviation from upstream: Native-only target initialization

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
chapters"). This refactor instead keeps the three `InitializeNativeTarget*`
calls unchanged from the JIT chapters (`src/main.cpp`), so
`CMakeLists.txt` stays byte-identical to Chapter7's
(`core orcjit native` — `orcjit` is now linked but unused). The doc's own
rationale cuts both ways: LLVM doesn't require linking in all target
functionality — a JIT needs no assembly printers, and a compiler targeting
only some architectures links only those.

|                            | upstream                        | refactored                                  |
| -------------------------- | ------------------------------- | ------------------------------------------- |
| initialized backends       | every one LLVM was built with   | host architecture only                      |
| link requirement           | `llvm-config --libs all`        | unchanged from Chapter7 (`core orcjit native`) |
| `lookupTarget(<non-native>)` | succeeds → cross-compile      | fails (prints the `lookupTarget` error)     |
| link time / binary size    | slower / larger                 | faster / smaller                            |

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

**New files**

| File | Purpose |
| --- | --- |
| `note.txt` | Why the JIT is removed in this chapter. |
| `example/average.txt`, `example/main.cpp` | The Kaleidoscope + C++ link demo. |
| `test/filecheck/objfile.k` | Emission check (see Tests). |
| `test/filecheck/errors.k` | Error recovery without a JIT: errors report, the driver keeps going, and emission still happens at EOF. |

**Same filename, byte-identical** — the frontend is untouched:
`CMakeLists.txt`, `build.sh`, `cmd.txt`, all of `include/` except
`ir_gen_ctx.h`, all of `src/` except `main.cpp`/`parser.cpp`,
`mem2reg_ex/*`, `test/lexer_test.cpp`, and every previous `.k` file
(`opt.k`, `controlflow.k`, `userops.k`, `mutablevars.k`, `lit.cfg`).

**Same filename, modified** — before → after:

`Chapter8/include/ir_gen_ctx.h` — two lines commented out: the JIT is never
created, and modules no longer take the JIT's data layout
(`compile_obj()` stamps the TargetMachine's instead).

`Chapter8/src/parser.cpp` — the three handlers' JIT blocks are commented
out (kept in place, visibly, rather than deleted — the chapter is "Chapter7
minus JIT"):

```cpp
// Chapter7                              // Chapter8
handleDefinition:                         handleDefinition:
  codegen + print                           codegen + print
  theJIT->addModule(...)                    // (commented out — module accumulates)
  InitializeModuleAndPassManager()
handleTopLevelExpression:                 handleTopLevelExpression:
  codegen + print                           codegen + print
  addModule / lookup / call / remove        // (commented out — nothing executes)
```

`Chapter8/src/main.cpp` — gains the target-init calls and `compile_obj()`;
`main` ends with `return compile_obj(ctx);` instead of `return 0;`.

`Chapter8/run.sh` — now the full AOT pipeline: run `toy` on the example,
`nm -g output.o`, link with `clang++`, execute.

`Chapter8/test/parser_test.cpp` / `test/codegen_test.cpp` — the JIT
execution suites from Chapters 4–7 are **removed**, with a comment
explaining why: `theJIT` is a never-initialized `unique_ptr` here, so those
tests would dereference null and crash. JIT behavior stays covered by the
earlier chapters' suites; this chapter's end-to-end story is `objfile.k`.

## Build and run

```sh
./build.sh      # configure + build
./run.sh        # toy < example/average.txt → output.o → nm → clang++ link → ./output
```

(Output as shown above; `output.o` and the linked `output` land in `build/`.)

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
- **JIT suites removed** from both gtest binaries (see File-by-file) — a
  reminder that tests encode a chapter's *architecture*, not just its code:
  with no JIT in the process, "evaluate and compare" is no longer a
  meaningful operation here.
- **`test/filecheck/errors.k`** — what *replaces* the earlier chapters'
  `jit.k`: with nothing to evaluate, the surviving driver contract is that
  codegen errors are reported, parsing continues, and a failed definition
  doesn't block the final `Wrote output.o`.

```sh
ctest --test-dir build              # everything
lit -v test/filecheck/objfile.k     # emission check
```
