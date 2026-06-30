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

**MLIR** (Multi-Level Intermediate Representation) is a compiler framework, built
*within* the LLVM ecosystem, for representing and transforming code at **many
levels of abstraction at once** — from high-level tensor math down to
hardware-specific instructions. It was designed to address the challenges of
modern hardware accelerators and machine-learning frameworks, where a single
fixed IR is too rigid.

To see why that matters, it helps to recall how **LLVM** works. LLVM is a
powerful, modular compiler infrastructure that has reshaped how we build
programming languages and tools: a front-end translates a source language into
**LLVM IR** — a low-level, assembly-like representation that still preserves some
high-level information — and LLVM then optimizes that IR and generates machine
code for many target architectures (x86, ARM, RISC-V, WASM, NVPTX, …).

LLVM IR is powerful but **low-level**. Expressing a high-level concept such as a
loop, a tensor operation, or a parallel region directly in LLVM IR is tedious
and loses structure the optimizer could have exploited. Historically, each
language invented its own *mid-level IR* to bridge this gap — Rust has MIR,
Swift has SIL, TensorFlow had XLA HLO. They all solved the same problem
independently.

MLIR generalizes that idea. Instead of one fixed IR, MLIR provides a framework
for *many* coexisting IRs, called **dialects**, at different levels of
abstraction. You write code in a high-level dialect and progressively **lower**
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

This design exists because traditional compilers like LLVM and GCC, while
excellent at conventional CPU targets, struggle with the diverse specialized
architectures emerging in AI and machine learning. MLIR addresses that head-on:

- **Heterogeneous hardware.** Modern AI systems mix CPUs, GPUs, TPUs, and custom
  ASICs. MLIR's flexible architecture makes it easier to work across all of them
  from a single infrastructure.
- **The right level of abstraction.** The dialect system can represent and
  optimize operations at many levels — from high-level ML tasks down to
  hardware-specific instructions — so optimizations can be tailored to each
  domain and AI workloads run more efficiently.
- **One unified infrastructure.** Instead of a separate compiler for each
  accelerator or framework, MLIR offers a single extensible base with
  first-class support for tensors, neural networks, and transformers — constructs
  traditional compilers were never built for.
- **"Weird domains."** Its ability to spin up domain-specific compilers (and to
  be embedded in other languages as a DSL) has made MLIR the go-to tech well
  beyond ML: signal processing, quantum computing, homomorphic encryption,
  FPGAs, and custom silicon.

### Key components of the modern compiler stack

A modern compiler weaves a few reusable pieces together:

1. **LLVM** — its intermediate representation is a low-level, assembly-like
   language that still preserves important high-level information, bridging
   source languages and target architectures.
2. **MLIR** — a "multi-level" IR that is more expressive than LLVM IR and can
   represent higher-level concepts like control flow, dataflow, and parallelism
   before being lowered into LLVM IR.
3. **Optimization passes** — both MLIR and LLVM ship a vast library of passes
   that operate at the IR level, so any language built on them inherits
   sophisticated, research-grade optimizations for free.
4. **E-graphs** — *equality saturation* (a technique from automated theorem
   proving) builds optimizing compilers by applying rewrites via e-matching
   until the e-graph is saturated, then extracting an optimal term according to a
   cost function — exploring a broad space of transformations and selecting the
   best one.

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

MLIR is a programming language in its own right. It is very low-level, but it
still has the core primitives you would expect from any language — modules,
functions, types, control flow. You'll see all of these in the examples; this
section is the glossary.

### Identifier conventions

MLIR uses a small set of sigils and delimiters to disambiguate the different
kinds of names and syntactic groups that appear in the IR.

| Sigil | Meaning | Example |
| --- | --- | --- |
| `%` | SSA value | `%result` |
| `@` | Function / symbol name | `@main` |
| `^` | Basic block label | `^bb0` |
| `#` | Attribute alias | `#map_1d_identity` |
| `!` | Type alias | `!avx_m128 = vector<4xf32>` |
| `x` | Shape / element-type delimiter | `10xf32` in `tensor<10xf32>` |
| `:` / `->` | Type annotation / result type | `%r : i32`, `(i32) -> i32` |
| `( )` | Operands / arguments | `(%arg0, %arg1)` |
| `{ }` | Region | `{ ... }` |
| `< >` | Type parameters | `tensor<10xf32>` |
| `//` | Comment | `// like this` |

### Structural concepts

**1. Modules** — the top-level container that holds everything else. Every `.mlir`
file is implicitly (or explicitly) wrapped in one:

```mlir
module {
  // functions, globals, and other operations go here
}
```

**2. Functions** — a named, ordered collection of operations, declared with the
`func` dialect. Arguments and the return type are part of the signature:

```mlir
func.func @my_function(%arg0: i32, %arg1: i32) -> i32 {
  // operations
}
```

**3. Operations** — the basic unit of work, analogous to an LLVM instruction but
namespaced by its dialect and carrying explicit type annotations. The fully
generic form names the operation as a string and lists operand and result types:

```mlir
%0 = "my_dialect.my_operation"(%arg0, %arg1) : (i32, i32) -> i32
```

Here `my_dialect.my_operation` is an op defined in `my_dialect`, `%arg0` and
`%arg1` are `i32` operands, and the `i32` result is bound to `%0`. In practice
most dialects also provide a *pretty* (custom) syntax. For example, the
`arith.addf` op adds two floating-point numbers:

```mlir
%0 = arith.addf %arg0, %arg1 : f32
```

**4. Basic blocks** — a straight-line sequence of operations with a single entry
point that executes without branching, ending in a control-flow op (a branch or
return). Blocks are labelled with the `^` sigil:

```mlir
^bb1:  // label for the "then" block
  %then_result = arith.muli %result, 2 : i32
  return %then_result : i32
```

Unlike LLVM, MLIR basic blocks can take **arguments**, passed in via the
`^bb1(%result: i32)` syntax. This replaces LLVM's phi nodes: instead of merging
values with `phi`, a predecessor block simply passes them as block arguments.

**5. Regions** — an ordered group of basic blocks attached to an operation,
written inside `{ }`. Regions are how MLIR represents nested control flow such as
loop and conditional bodies, and like blocks they can take arguments:

```mlir
{
  ^bb1(%result: i32):
    %then_result = arith.muli %result, 2 : i32
    return %then_result : i32
}
```

**6. Types** — a classification that says what kind of value an SSA value holds
and which operations apply to it. Common types are `i32`, `f32`, `index`,
`tensor<10xf32>`, and `memref<4x4xf32>`:

```mlir
%result = arith.constant 1 : i32
```

For convenience you can define a **type alias** (synonym) with the `!` sigil:

```mlir
!avx_m128 = vector<4xf32>
```

**7. Dialects** — the namespaced sets of operations and types that make MLIR
multi-level. You write code in a high-level dialect and lower it toward `llvm`.
The dialects used across this series:

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

**8. Passes** — transformations that operate on the dialects, optimizing the IR
or **lowering** it into simpler constructs. They are the arguments you hand to
`mlir-opt`. The most common ones are conversions toward the `llvm` dialect:

| Pass | Effect |
| --- | --- |
| `--convert-func-to-llvm` | Convert function-like ops to the `llvm` dialect |
| `--convert-arith-to-llvm` | Convert arithmetic ops to the `llvm` dialect |
| `--convert-math-to-llvm` | Convert math ops to the `llvm` dialect |
| `--convert-index-to-llvm` | Convert index ops to the `llvm` dialect |
| `--convert-scf-to-cf` | Lower structured control flow to the `cf` dialect |
| `--convert-cf-to-llvm` | Convert control flow to the `llvm` dialect |
| `--finalize-memref-to-llvm` | Convert memref ops to the `llvm` dialect |
| `--convert-vector-to-llvm` | Convert vector ops to the `llvm` dialect |
| `--convert-linalg-to-loops` | Expand `linalg` ops into `scf` loops |
| `--reconcile-unrealized-casts` | Resolve leftover `unrealized_conversion_cast`s |

There is also a catch-all `--convert-to-llvm` that lowers anything it can; in
practice we prefer the granular passes so we control exactly what happens.

For finer control, list passes in a `--pass-pipeline` string — they run
sequentially as one group:

```bash
mlir-opt --pass-pipeline="builtin.module(convert-scf-to-cf,convert-cf-to-llvm)" in.mlir
```

> **Pass order matters.** For example, `convert-scf-to-cf` must run *before*
> `convert-cf-to-llvm`, because the second pass only knows how to lower the `cf`
> dialect the first one produces.

When a pipeline misbehaves, these debugging flags print the IR as it changes:

| Flag | What it shows |
| --- | --- |
| `--mlir-print-ir-after-all` | The IR after every pass |
| `--mlir-print-ir-after-change` | The IR only after a pass that changed it |
| `--mlir-print-ir-after-failure` | The IR after a pass that failed |
| `--mlir-print-ir-tree-dir=<dir>` | Write each IR snapshot to files instead of stdout |

---

## Standard MLIR dialects

The concepts above are the vocabulary; the dialects below are the workhorses you
hit first when lowering toward native code. Each is its own namespace of
operations, sitting at a particular level of the abstraction ladder. The ones in
this section are the low-level "standard" dialects — the common floor that almost
every higher-level dialect eventually lowers through.

### `llvm`

The `llvm` dialect is a near 1:1 mirror of LLVM IR and the **lowest** level in
the MLIR hierarchy. It is the end of the road: once your program is fully in the
`llvm` dialect, `mlir-translate` can pass it straight through to textual LLVM IR.
Everything else in this section ultimately lowers into it.

### `scf` and `cf` — control flow

These two dialects are two views of the same thing at different levels.

**`scf`** (structured control flow) gives you the high-level constructs you'd
expect from a real language — `if`/`else`, `for`, `while` — each with a single
entry and a single exit. The simplest is `scf.if`:

```mlir
scf.if %b {
  // true region
} else {
  // false region
}
```

A `scf.for` loop carries its bounds and step explicitly, and (as in the loop
example earlier) can thread loop-carried values through `iter_args`:

```mlir
%lb = index.constant 0
%ub = index.constant 10
%step = index.constant 1
scf.for %iv = %lb to %ub step %step {
  // loop region
}
```

**`cf`** (unstructured control flow) is the lower-level target those structures
compile down to: plain branches between basic blocks. The
`--convert-scf-to-cf` pass rewrites every `scf` construct into a sequence of
these:

- `cf.br` — an **unconditional** branch that always jumps to the same block:

  ```mlir
  ^bb0:
    cf.br ^bb1
  ```

- `cf.cond_br` — a **conditional** branch that picks a block based on an `i1`
  condition (`%c` below):

  ```mlir
  ^bb0:
    cf.cond_br %c, ^bb1, ^bb2
  ```

- `cf.switch` — a multi-way branch selecting a block from an integer (`%i`):

  ```mlir
  ^bb0:
    cf.switch %i, ^bb1, ^bb2, ^bb3
  ```

Put together — and using the **block arguments** that replace LLVM's phi nodes —
a conditional branch can select between two values by passing whichever one it
wants into the shared successor block:

```mlir
func.func @select(%a: i32, %b: i32, %flag: i1) -> i32 {
  cf.cond_br %flag, ^bb1(%a : i32), ^bb1(%b : i32)
^bb1(%x: i32):
  return %x : i32
}
```

### `arith` and `math` — computation

**`arith`** holds the fundamental scalar (and vector/tensor) math, split into a
few families:

| Family | Operations |
| --- | --- |
| Integer arithmetic | `addi`, `subi`, `muli`, `divsi` (signed), `divui` (unsigned) |
| Float arithmetic | `addf`, `subf`, `mulf`, `divf` |
| Comparisons | `cmpi` (integer), `cmpf` (float) |
| Conversions | `extsi`/`extui` (sign/zero extend), `trunci`, `fptosi`/`fptoui`, `sitofp`/`uitofp` |
| Bitwise | `andi`, `ori`, `xori` |

```mlir
func.func @arithmetic_example(%a: i32, %b: f32) -> i32 {
  %1 = arith.constant 42 : i32       // an integer constant
  %2 = arith.addi %a, %1 : i32       // integer addition
  %3 = arith.fptosi %b : f32 to i32  // float → signed integer
  %4 = arith.addi %2, %3 : i32
  return %4 : i32
}
```

**`math`** layers the more elaborate functions on top:

| Family | Operations |
| --- | --- |
| Trigonometric | `sin`, `cos`, `tan` |
| Exponential / log | `exp`, `exp2`, `log`, `log2`, `log10` |
| Power | `pow`, `sqrt` |
| Special | `erf`, `atan2` |

```mlir
func.func @math_example(%x: f32) -> f32 {
  %1 = math.sin %x : f32        // compute sin(x) * sqrt(x)
  %2 = math.sqrt %x : f32
  %3 = arith.mulf %1, %2 : f32
  return %3 : f32
}
```

The `--convert-arith-to-llvm` and `--convert-math-to-llvm` passes lower both
dialects to their LLVM equivalents.

### `index` — addressing and loop counters

The `index` dialect handles **index computations** — the platform-sized integers
used for addressing memory and driving loop induction variables. Because indices
are always non-negative, you can treat them as natural numbers. Its key ops
mirror `arith` but operate on the `index` type:

| Operation | Purpose |
| --- | --- |
| `index.constant` | Create an index constant |
| `index.add` / `index.sub` | Add / subtract two indices |
| `index.mul` | Multiply two indices |
| `index.divs` / `index.rems` | Signed division / remainder |
| `index.cmp` | Compare two indices |

A typical use is turning multi-dimensional coordinates into a flat memory offset
— here the row-major formula `i * stride + j`:

```mlir
func.func @compute_offset(%i: index, %j: index, %stride: index) -> index {
  %1 = index.mul %i, %stride
  %2 = index.add %1, %j
  return %2 : index
}
```

The `--convert-index-to-llvm` pass lowers it to the `llvm` dialect. The `index`
type comes into its own alongside the `memref` and `tensor` dialects (covered in
[`2_memory/`](2_memory/)), where it expresses array indices and shapes.

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
| [`3_parallel/`](3_parallel/) | Parallelism: the `affine` dialect & polyhedral model, optimization passes, convolution, and OpenMP. | ✅ |
| [`4_linalg/`](4_linalg/) | Linear algebra: the `linalg` dialect — structured ops, matmul, broadcasting, fusion, and tiling. | ✅ |
| [`5_neural_network/`](5_neural_network/) | Neural networks: from-scratch autodiff & training in Python, plus a dense layer compiled with `linalg`. | ✅ |
| [`6_egraph/`](6_egraph/) | E-graphs & term rewriting: a from-scratch equality-saturation engine that optimizes expressions, then emits & runs MLIR. | ✅ |
| [`7_transformer/`](7_transformer/) | Transformers: a NumPy GPT-2 forward pass, plus attention's softmax compiled to MLIR. | ✅ |
| [`8_gpu/`](8_gpu/) | GPU compilation: lowering a parallel loop through the `gpu` dialect to NVVM/PTX (inspect-only — needs NVIDIA CUDA to run). | ✅ |

All eight chapters are built out and verified on this toolchain (the runnable
ones execute and check against NumPy; Chapter 8's GPU lowering is inspect-only,
since running it needs NVIDIA CUDA hardware). The endgame of the full series is
compiling a small GPT-2-style transformer down toward efficient GPU kernels. The
`reference/` directory holds the source PDFs for every part.

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
