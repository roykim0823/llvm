# Kaleidoscope — refactoring the LLVM tutorial

This directory holds two refactoring tracks of the LLVM
[Kaleidoscope tutorial](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/)
("My First Language Frontend"), plus the upstream reference code they are
measured against. Kaleidoscope is a tiny procedural language with exactly one
type (every value is a 64-bit double); the tutorial grows a complete compiler
for it — lexer, parser, LLVM IR codegen, optimizer, JIT, object emission,
debug info — one chapter at a time.

## Directory map

| Directory | What it is |
| --- | --- |
| `Chapter2` | Chapters 1–2: lexer, parser, AST. The driver reports what it parsed. |
| `Chapter3` | Chapter 3: codegen to LLVM IR (`IRGenContext`, `codegen()` per node). |
| `Chapter4` | Chapter 4: optimizer (FPM) + ORC JIT — the REPL evaluates (`jit.k` starts here). |
| `Chapter5` | Chapter 5: control flow — `if/then/else` and `for` as phi-based CFG. |
| `Chapter6` | Chapter 6: user-defined operators; precedence table moves into `IRGenContext`. |
| `Chapter7` | Chapter 7: mutable variables — alloca/load/store + `mem2reg` (`PromotePass`). |
| `Chapter8` | Chapter 8: object files via `TargetMachine`; deliberately **no JIT**. |
| `Chapter9` | Chapter 9: DWARF debug info; batch compiler dumping one module at exit. |
| `ChapterA` | The redesigned counterpart: one binary (`toyc -emit=ast\|ir\|obj\|jit`), MLIR-Toy-style frontend design. Its own conventions — see [its README](ChapterA/README.md). |
| `BuildingAJIT` | Upstream reference code for the ["Building a JIT"](https://llvm.org/docs/tutorial/BuildingAJIT1.html) tutorial series (ORC, one chapter per layer) — kept as-is, not refactored. |
| `include/KaleidoscopeJIT.h` | The tutorial's ORC JIT wrapper, shared by Chapter4+ and ChapterA. |

Each `ChapterN` corresponds to tutorial chapter N (`LangImpl0N.html`) and
builds on the previous directory; every directory has its own README that
covers **what that chapter adds**, with links back for everything inherited.

## The two tracks

**Chapter2–9 stay faithful to upstream.** The single-file `toy.cpp` is
reorganized into a `src/` + `include/` + `test/` layout with classes in
`namespace toy` (globals become members), but the *behavior* is upstream's —
including its quirks, which are kept deliberately. The rare deviations are
intentional and each gets its own documented section in the chapter README
(e.g. Chapter2's leading-dot number validation, Chapter7's reverse-order
scope restoration). When code differs from the tutorial snippet, treat it as
deliberate, not as a bug to fix.

**ChapterA is the "designed deliberately" counterpart**: a single codebase
applying the MLIR Toy tutorial's frontend architecture (buffer-based lexer,
pure-data AST with kind tags, pImpl codegen session, staged driver), fixing
the upstream quirks as located errors instead of UB/asserts. Nothing in the
common sections below is binding for it; it documents itself.

## Common architecture (Chapter2–9)

The tutorial's globals map onto classes the same way in every chapter:

| Upstream (`toy.cpp` globals) | Refactored |
| --- | --- |
| `IdentifierStr`, `NumVal`, `LastChar`, `gettok()` | `toy::Lexer` (`include/lexer.h`, `src/lexer.cpp`) |
| `CurTok`, `BinopPrecedence`, `Parse*`/`Handle*`/`MainLoop` | `toy::Parser` (`include/parser.h`, `src/parser.cpp`) |
| AST node classes | `include/ast.h` |
| `TheContext`, `TheModule`, `Builder`, `NamedValues`, FPM, `TheJIT`, `FunctionProtos` | `toy::IRGenContext` (`include/ir_gen_ctx.h`) — from Chapter3 on |
| `codegen()` bodies | `src/codegen.cpp` — from Chapter3 on |
| `LogError*` helpers | `include/log.h`, `src/log.cpp` |
| `putchard`/`printd` | `src/extern_d.cpp` — from Chapter5 on (listed on the executables, not in the archive, so the linker can't drop the symbols the JIT resolves) |

## Language notes and gotchas

These hold across Chapter2–9 (ChapterA fixes the first two):

- **Leading dot** (deliberate deviation from upstream, since Chapter2): `.5`
  lexes as 0.5; a lone `.` is a plain ASCII token. Upstream turns any leading
  `.` into the number 0.0.
- **Identifiers do not allow `_`**: `my_var` lexes as `my`, `_`, `var`.
- **No scientific notation**: `1.5e3` lexes as `1.5`, then `e3`.
- **Multi-dot numbers**: the lexer consumes all of `3.14.15`; `strtod` stops
  at the second dot, yielding 3.14 (upstream quirk, kept).
- **`for` is do-while shaped**: the end condition is tested *after* the body
  with the pre-increment variable, so `for i = 1, i < n` runs the body for
  `i = 1..n` — n times — and always at least once.
- **Prototype parameters are space-separated** (`def foo(x y)`); call
  arguments use commas (`foo(a, b)`).
- The chapters' demos define `binary: 1 (x y) y` as a **sequencing operator**
  — precedence 1, evaluates both sides, yields the right one.

## Build and run

Requirements: LLVM via Homebrew (`llvm-config` on PATH), CMake ≥ 3.20, Ninja;
`lit` on PATH for running FileCheck tests by hand (ctest finds it itself).

```sh
cd ChapterN
cmake -B build -G Ninja -S .     # or ./build.sh (clean build; some chapters
cmake --build build              #  also pipe the demo input through the binary)
./build/toy < cmd.txt            # each chapter's demo input (see its README)
```

Notes that apply everywhere:

- The CMake files pin `CMAKE_OSX_DEPLOYMENT_TARGET` to the **host** macOS
  release (via `sw_vers`) so the objects agree with the Homebrew LLVM
  libraries.
- Chapters 2–7 are REPLs reading stdin. When input is **piped** instead of
  typed, the output interleaves extra `ready>` prompts (the prompt prints
  before each read, and nothing echoes the input) — the READMEs' example
  sessions show the interactive form.
- GoogleTest is fetched by CMake (`FetchContent`, pinned release); binaries
  land as `./build/lexer_test` etc.

## Testing: the two schemes

The test setup deliberately mirrors how the LLVM project itself is tested.
LLVM uses two complementary schemes:

- **Unit tests** (`llvm/unittests/`, GoogleTest): C++ tests that link against
  LLVM's libraries and exercise APIs directly — data structures like `APInt`
  or `StringMap`, support utilities, anything whose contract lives at the C++
  level and has no convenient textual surface. They are the minority of
  LLVM's tests.
- **Regression tests** (`llvm/test/`, lit + FileCheck): the bulk of LLVM's
  testing — thousands of small `.ll`/`.mir`/`.s` files, each with `RUN:`
  lines that feed the file through a real tool (`opt`, `llc`, `clang`, ...)
  and FileCheck-verify the textual output. Every bug fix and every new
  transformation lands with one of these. `ninja check-llvm` runs them all
  through lit.

The community's rule of thumb: **if behavior is observable by running a tool
on an input file and inspecting its output, write a lit test; drop down to a
unit test only when it isn't** — because compilers are pipelines of
text-to-text (or IR-to-IR) tools, almost everything interesting *is*
observable that way, which is why the lit scheme dominates. Unit tests earn
their place where the textual interface doesn't reach: internal containers,
edge cases of a single class, exact in-memory values.

Every Chapter directory is a miniature of that layout — `toy` (or ChapterA's
`toyc`) plays the role of `opt`/`clang`, the `.k` files under
`test/filecheck/` play the role of `llvm/test/*.ll`, and the gtest binaries
cover class-level contracts. Both layers are wired into CTest. lit tests are
particularly convenient for a compiler because a `.k` file plays **three
roles at once**: it is the shell command (`RUN:` line), the program fed to
the binary (`#` comments are skipped by the lexer, so the annotations are
invisible), and the expected output (`# CHECK:` lines, matched in order).

What each chapter's tests pin down is described in that chapter's README;
the coverage also *travels*: `.k` suites are carried forward chapter to
chapter as long as the behavior they pin still exists, with documented
hand-offs where the architecture changes —

```
parse.k, parse-error.k   (2)   driver reports parses; error recovery
opt.k                    (4→8) optimized-IR shape
jit.k                    (4→7) "Evaluated to ...", error recovery, per-chapter additions
controlflow.k            (5→8) CFG shape (adapted for mem2reg in 7)
userops.k                (6→8) operator precedence end-to-end
mutablevars.k            (7→8) "no alloca survives" mem2reg contract
objfile.k, errors.k      (8)   emission; error recovery replaces jit.k (no JIT)
debuginfo.k, errors.k    (9)   DWARF metadata with exact line/col; batch error recovery
```

One infrastructure note: `gtest_discover_tests` registers each gtest case as
its own ctest entry, so `ctest -j` runs cases of one binary concurrently in
the same working directory — which is why the stdin-redirecting fixtures in
Chapter2–9 write **pid-suffixed** temp files. (ChapterA's lexer reads from
memory buffers and needs none of this.)

### How lit works with `lit.cfg`

lit (LLVM Integrated Tester) is not tied to any test's contents — it is a
generic driver whose behavior is configured per test suite by a `lit.cfg`
file. The flow for `lit -v test/filecheck/parse.k` is:

1. **Discovery.** lit walks up from the given path until it finds a
   `lit.cfg`; the directory containing it becomes the test suite root. The
   config file is plain Python, executed with a `config` object that lit
   provides and the script fills in.
2. **Test collection.** Every file under the suite root whose extension is in
   `config.suffixes` (here `['.k']`) is a test. Dropping a new `.k` file into
   `test/filecheck/` makes it a test — no registration anywhere.
3. **Execution.** `config.test_format = lit.formats.ShTest(True)` selects the
   "shell test" format: lit scans the test file for `RUN:` lines (anything
   after the `RUN:` marker, regardless of the comment syntax around it),
   applies substitutions, and executes each line as a shell pipeline. The
   test passes iff every `RUN:` line exits 0 — since FileCheck exits non-zero
   on a failed match, the pipeline's exit status *is* the verdict. (Error
   tests invert this with `not`, which ships in LLVM's bindir:
   `RUN: not %toy ...`.)
4. **Substitution.** Before running, lit textually replaces `%`-patterns.
   `%s` (built in) is the absolute path of the test file itself; `%toy` is
   defined by each suite's config.

Annotated `lit.cfg` (identical in every directory, modulo the binary name):

```python
config.name = 'Kaleidoscope'          # suite name in lit's output
config.test_format = lit.formats.ShTest(True)  # execute RUN: lines via shell
config.suffixes = ['.k']                       # which files are tests
config.test_source_root = os.path.dirname(__file__)   # where tests live
config.test_exec_root = os.environ.get(        # where lit writes scratch output;
    'LIT_EXEC_ROOT', ...)                      # ctest points this at build/lit
                                               # so the source tree stays clean

# %toy → the binary under test. ctest sets TOY_BIN to the freshly built
# target ($<TARGET_FILE:toy>); running lit by hand falls back to ./build/toy.
toy = os.environ.get('TOY_BIN', '.../build/toy')
config.substitutions.append(('%toy', toy))

# RUN: lines invoke FileCheck by bare name, so prepend LLVM's bindir
# (from `llvm-config --bindir`) to PATH for the child processes.
config.environment['PATH'] = bindir + os.pathsep + path
```

The indirection through `TOY_BIN` / `LIT_EXEC_ROOT` is how CTest and lit
compose: each `CMakeLists.txt` registers one ctest test named `filecheck`
that invokes lit with those environment variables pointing at the build
tree, so `ctest` always tests the binary it just built. (Hand-runs without
`LIT_EXEC_ROOT` write scratch output to `test/filecheck/Output/`, which is
gitignored.)

Commands, from any Chapter directory:

```sh
ctest --test-dir build                 # everything (gtest + lit)
ctest --test-dir build -R filecheck    # just the lit suite
./build/lexer_test                     # a gtest binary directly

lit -v test/filecheck/<file>.k         # one lit test by hand
TOY_BIN=$PWD/build/toy lit -v test/filecheck/<file>.k   # explicit binary
```

## README conventions

Each chapter README follows one shape (Chapter2's is the reference):
concepts the chapter introduces → pipeline/architecture diagrams drawn from
the refactored classes → a file-by-file diff against the previous chapter
(new / byte-identical / modified, established by actually running `diff`) →
tutorial-style walkthrough of the new code with real snippets → a dedicated
subsection for every deliberate deviation from upstream → example session
with real output → what this chapter's tests pin down, linking here for the
shared mechanics.
