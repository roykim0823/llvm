# MLIR Tutorial

A hands-on, example-driven tour of **MLIR** (Multi-Level Intermediate
Representation), the compiler infrastructure inside the LLVM project. Each
chapter pairs a short explanation with runnable `.mlir` code and `build.sh`
scripts you can execute and inspect step by step.

> This series is based on Stephen Diehl's *"MLIR"* tutorial articles (see
> [`reference/`](reference/)) and is adapted/expanded for a macOS +
> Apple-Silicon, Homebrew `llvm@20` setup. For installation instructions, see
> [**MLIR_SETUP.md**](MLIR_SETUP.md).

---

## What is MLIR?

LLVM is a modular compiler infrastructure: a front-end translates a source
language into **LLVM IR** — a low-level, assembly-like representation — and LLVM
then optimizes that IR and generates machine code for many target architectures
(x86, ARM, RISC-V, WASM, NVPTX, …).

LLVM IR is powerful but **low-level**. Expressing a high-level concept such as a
loop, a tensor operation, or a parallel region directly in LLVM IR is tedious
and loses structure the optimizer could have exploited. Historically, each
language invented its own *mid-level IR* to bridge this gap — Rust has MIR,
Swift has SIL, TensorFlow had XLA HLO. They all solved the same problem
independently.

**MLIR** generalizes that idea. Instead of one fixed IR, MLIR provides a
framework for *many* coexisting IRs, called **dialects**, at different levels of
abstraction — from high-level tensor math down to hardware-specific
instructions. You write code in a high-level dialect and progressively **lower**
it, one pass at a time, until you reach the `llvm` dialect, which maps directly
onto LLVM IR.

```
  Source        Mid-level IR         Unified backend        Targets
  ──────        ────────────         ───────────────        ───────
  C / C++ ─┐                                              ┌─ x86
  Rust   ──┤    (MIR, SIL, …)                             ├─ ARM
  Swift  ──┤         ──►        MLIR dialects ──► LLVM IR ─┤─ RISC-V
  Julia  ──┤                    (lowering passes)         ├─ NVPTX (GPU)
  PyTorch ─┘                                              └─ WASM
```

### Why it matters

Modern AI workloads run across a mix of CPUs, GPUs, TPUs, and custom ASICs.
Traditional compilers weren't built with tensors, neural networks, or
heterogeneous accelerators in mind. MLIR's dialect system lets you represent and
optimize operations at the *right* level of abstraction for the domain — and
reuse the same battle-tested LLVM optimization and code-generation machinery
underneath. That makes it the de facto infrastructure for ML compilers and
"weird domain" compilers alike (signal processing, quantum, FHE, FPGAs).

### The modern compiler pipeline

A typical MLIR-based compiler weaves these stages together:

1. Source code is parsed into a surface language.
2. The surface language is translated to a core language.
3. The core language is optimized by core-to-core transformations (optionally
   using techniques like **e-graphs** / equality saturation).
4. The core language is lowered to MLIR.
5. MLIR performs high-level optimizations.
6. MLIR is lowered to LLVM IR.
7. LLVM performs low-level optimizations and code generation.

---

## Key MLIR concepts

A quick glossary; you'll see all of these in the examples.

**Identifier sigils**

| Sigil | Meaning | Example |
| --- | --- | --- |
| `%` | SSA value | `%result` |
| `@` | Function (symbol) | `@main` |
| `^` | Basic block | `^bb0` |
| `#` | Attribute alias | `#map_1d_identity` |
| `!` | Type alias | `!avx_m128 = vector<4xf32>` |
| `x` | Shape/type delimiter | `tensor<10xf32>` |
| `:` / `->` | Type annotation / result type | `%r : i32`, `(i32) -> i32` |

**Structural building blocks**

- **Module** — the top-level container: `module { ... }`.
- **Function** — `func.func @name(%a: i32) -> i32 { ... }`.
- **Operation** — the basic unit of work, namespaced by dialect:
  `%0 = arith.addf %a, %b : f32`.
- **Basic block** — a straight-line sequence of ops with one entry; in MLIR
  blocks can take **arguments** (`^bb1(%x: i32):`), which replaces LLVM's phi
  nodes.
- **Region** — a `{ ... }` group of blocks attached to an op; used for loop and
  conditional bodies.
- **Type** — `i32`, `f32`, `index`, `tensor<10xf32>`, `memref<4x4xf32>`, …
- **Pass** — a transformation applied by `mlir-opt`, typically lowering one
  dialect toward `llvm` (e.g. `--convert-scf-to-cf`). **Pass order matters.**

**Dialects used in this series**

| Level | Dialect | Purpose |
| --- | --- | --- |
| High | `tensor` | side-effect-free multi-dimensional arrays |
| High | `linalg` | structured linear-algebra ops |
| High | `affine` | affine loop nests & analyses |
| High | `omp` | OpenMP parallelism |
| High | `gpu` | GPU kernels / heterogeneous execution |
| Low | `scf` | structured control flow (`scf.for`, `scf.if`) |
| Low | `cf` | unstructured control flow (branches) |
| Low | `func` | functions & calls |
| Low | `arith` / `math` | scalar/vector arithmetic & math |
| Low | `index` | platform-sized index computation |
| Low | `memref` | memory buffers |
| Low | `llvm` | 1:1 mirror of LLVM IR (lowest level) |

---

## The toolchain

Every chapter drives the same handful of tools (installed via Homebrew
`llvm@20`; see [MLIR_SETUP.md](MLIR_SETUP.md)):

| Tool | Role |
| --- | --- |
| `mlir-opt` | Run lowering / optimization passes on `.mlir`. |
| `mlir-runner` | JIT-execute MLIR directly (great for quick checks). |
| `mlir-translate` | Convert the `llvm` dialect into textual LLVM IR (`.ll`). |
| `llc` | Compile LLVM IR to a native object (`.o`) or assembly (`.s`). |
| `clang` | Link objects into an executable or shared library (`.so`/`.dylib`). |

The recurring pipeline:

```
 .mlir ──mlir-opt──► (llvm dialect) ──mlir-translate──► .ll ──llc──► .o ──clang──► .so / executable
                            │
                            └──────── mlir-runner (JIT, no codegen) ───────► result
```

---

## Tutorial structure

| Chapter | Topic | Status |
| --- | --- | --- |
| [`1_intro/`](1_intro/) | What is MLIR? LLVM modules, the `func`/`scf`/`arith`/`index` dialects, and the full lower-to-native pipeline. | ✅ |
| [`2_memory/`](2_memory/) | Memory in MLIR: `tensor` vs `memref`, bufferization, array add, C-compatible wrappers. | ✅ |
| *Part 3* | Affine dialect & OpenMP | reference PDF |
| *Part 4* | Linear algebra & `linalg` | reference PDF |
| *Part 5* | Neural networks & tensors | reference PDF |
| *Part 6* | e-graphs & term rewriting | reference PDF |
| *Part 7* | Transformers | reference PDF |
| *Part 8* | GPU compilation with MLIR | reference PDF |

The endgame of the full series is compiling a small GPT-2-style transformer down
to efficient GPU kernels. The `reference/` directory holds the source PDFs for
every part.

---

## Getting started

```bash
# 1. Install MLIR (macOS / Homebrew)
brew install llvm@20
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"   # llvm@20 is keg-only

# 2. Verify
mlir-opt --version        # expect: Homebrew LLVM version 20.x

# 3. Run the first chapter
cd 1_intro/2_mlir
bash build.sh
```

See [MLIR_SETUP.md](MLIR_SETUP.md) for every other install method (Apt, source,
Python wheels, Conda, Docker) and troubleshooting.

---

## Prerequisites

Some familiarity with C/C++ and Python is assumed; passing familiarity with
NVIDIA CUDA helps for the later GPU chapters. No prior compiler background is
required — concepts are introduced as they appear.
