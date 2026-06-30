# 3 — Parallelism: Affine Dialect & OpenMP

### A loop says more than it means

Write a `for` loop and you have, without meaning to, made a promise: *iteration 0
happens, then 1, then 2, …*, strictly in order. But look at a matrix multiply, or
blurring an image, or scaling a vector — the iterations don't actually *depend* on
each other. Computing `C[0][0]` doesn't need `C[0][1]` to have happened first. The
sequential order is an **accident of syntax**, not a requirement of the math. The
loop says more than it means.

That gap is where all parallel performance lives. A modern CPU has many cores, a
GPU has thousands; the entire game is to notice which work is *independent* and let
it run at the same time. Chapter 1 walked computation *down* the staircase
(loops → branches → registers); Chapter 2 asked *where data lives* (value vs.
address). **This chapter is about a third question: _when_ does work run, and what
is allowed to run at once?**

MLIR gives you two routes to answer it, and this chapter walks both:

- **Let the compiler find the parallelism** — the **`affine` dialect**. By
  restricting loops and array accesses to *affine* (quasi-linear) expressions of
  the loop indices, affine makes loops "first-class" objects the compiler can
  reason about geometrically. It can then *prove* iterations are independent and
  rewrite the nest automatically — parallelize it, tile it for cache, fuse
  adjacent loops, reorder for locality.
- **State the parallelism yourself** — the **`omp` (OpenMP) dialect**. Sometimes
  you'd rather just say "run these iterations on a team of threads" — the
  directive-based model familiar from `#pragma omp parallel for` in C. MLIR has it
  as first-class ops.

> Based on Stephen Diehl's *"MLIR Part 3 — Affine Dialect and OpenMP"*
> ([`../reference/`](../reference/)). Every example here is **built and run /
> inspected on this toolchain** (Homebrew LLVM 20.1.8, Apple Silicon); the
> OpenMP step additionally links Homebrew's `libomp`.

---

## The polyhedral model — loops as geometry

Why can a compiler reason about an `affine.for` nest but not a general
`scf.for`/C loop? Because affine constraints let it adopt the **polyhedral
model**: the idea of representing a loop nest as a *geometric object*.

Each execution of a statement inside a 2-D loop nest is a point `(i, j)`; the set
of all such points is a polygon (in higher dimensions, a polyhedron) carved out by
the loop bounds. Once your program is a shape, loop optimization becomes
*geometry*: parallelizing is slicing the shape into independent pieces, tiling is
gridding it into blocks, interchange is reflecting it, skewing is shearing it. The
model has three pieces:

- **Iteration domains** — the set of points a loop nest visits, bounded by affine
  inequalities (e.g. `0 ≤ i < M`).
- **Access relations** — which memory each point touches (e.g. point `(i, j)`
  reads `A[i, k]`).
- **Scheduling functions** — the order points execute in; a *valid* reordering is
  one that preserves every dependence.

MLIR builds three concepts on top of this:

| Concept | What it is | Example |
| --- | --- | --- |
| **Affine map** | A quasi-linear function from dims/symbols to results | `(d0, d1)[s0] -> (d0 + d1, s0 * d1)` |
| **Integer set** | Affine constraints carving out a domain | `(i)[N] : (i >= 0, N - i - 1 >= 0)` = `0 ≤ i < N` |
| **Affine ops** | `affine.for`, `affine.if`, `affine.parallel`, `affine.apply` | loop nests & index math |

The catch — and the price of all this power — is that affine expressions may use
**only** `+`, `-`, `*` by a constant, and `floordiv`/`ceildiv`/`mod` by a
constant. No data-dependent indices (`A[B[i]]`), no multiplying two loop
variables. That restriction is exactly what keeps the geometry tractable.

### `affine` vs `scf`

| | `scf` (Chapter 1) | `affine` (this chapter) |
| --- | --- | --- |
| Loop op | `scf.for` / `scf.parallel` | `affine.for` / `affine.parallel` |
| Index/access form | anything | affine expressions only |
| Memory ops | `memref.load`/`store` | `affine.load`/`store` (analyzable) |
| Compiler can auto-tile / fuse / parallelize? | no | **yes** |
| Lower it with | (already low) | `-lower-affine` → `scf` |

You write high-level `affine`, let the optimizer transform it, then `-lower-affine`
drops it to `scf`, and from there it's the same pipeline as every earlier chapter.

---

## Chapter layout

| Step | Directory | Topic | Runnable? |
| --- | --- | --- | --- |
| 1 | [`1_affine_matmul/`](1_affine_matmul/) | `affine.for` matmul + `--affine-parallelize` | ✅ Python |
| 2 | [`2_affine_maps/`](2_affine_maps/) | `affine.apply`, affine maps, tiled indexing | 🧩 inspect |
| 3 | [`3_affine_opts/`](3_affine_opts/) | the affine optimization passes (LICM, tile, unroll, fuse) | 🧩 inspect |
| 4 | [`4_convolution/`](4_convolution/) | 2-D convolution with `iter_args` reductions | ✅ Python |
| 5 | [`5_openmp/`](5_openmp/) | the `omp` dialect + `-convert-scf-to-openmp` | ✅ executable |

Shared helper: [`common/np_memref2d.py`](common/np_memref2d.py) — a **2-D** memref
descriptor + NumPy adapter (Chapter 2's `np_memref.py` was 1-D), imported by the
matmul and conv2d drivers.

**Legend:** ✅ runnable · 🧩 snippet (lowered/transformed with `mlir-opt`, not run).

---

## Step 1 — Affine matmul, and automatic parallelization (`1_affine_matmul/`) · ✅

**Goal:** meet `affine.for` / `affine.load` / `affine.store`, and watch the
compiler turn an ordinary loop nest into a parallel one.

The classic product $C_{ij} = \sum_k A_{ik} B_{kj}$ as a three-deep affine nest:

```mlir
affine.for %i = 0 to %M {
  affine.for %j = 0 to %N {
    affine.for %k = 0 to %K {
      %a = affine.load %A[%i, %k] : memref<?x?xf32>
      %b = affine.load %B[%k, %j] : memref<?x?xf32>
      %c = affine.load %C[%i, %j] : memref<?x?xf32>
      %sum = arith.addf %c, arith.mulf %a, %b : f32
      affine.store %sum, %C[%i, %j] : memref<?x?xf32>
    }
  }
}
```

The `%i` and `%j` loops are **independent** — each iteration writes a distinct
`C[i, j]`. The compiler can *see* this (affine accesses make the dependence
analysis exact), so `--affine-parallelize` rewrites those two loops into
`affine.parallel`, leaving the `k`-reduction sequential. This is the exact IR it
emits (in `build/matmul_parallel.mlir`):

```mlir
affine.parallel (%i) = (0) to (symbol(%M)) {        // outer loop: parallel
  affine.parallel (%j) = (0) to (symbol(%N)) {      // inner loop: parallel
    affine.for %k = 0 to %K {                       // the k-reduction stays sequential
      %a = affine.load %A[%i, %k] : memref<?x?xf32>
      %b = affine.load %B[%k, %j] : memref<?x?xf32>
      %c = affine.load %C[%i, %j] : memref<?x?xf32>
      %sum = arith.addf %c, arith.mulf %a, %b : f32
      affine.store %sum, %C[%i, %j] : memref<?x?xf32>
    }
  }
}
```

`affine.parallel` says "these iterations may run in any order, or all at once."
Two notes on what you're seeing:

- **Two nested `affine.parallel`s, not one 2-D band.** The op *can* express a
  hyper-rectangular band like `affine.parallel (%i, %j) = (0,0) to (%M,%N)` (one
  op, two induction variables), but `--affine-parallelize` parallelizes each loop
  in place, so you get an outer parallel loop wrapping an inner one. They mean the
  same thing — both say the `i` and `j` iterations are independent.
- **No `affine.yield` here.** This `affine.parallel` returns *no results*: each
  iteration accumulates straight into the `C` buffer with `affine.store`, so
  there's nothing to combine across iterations. `affine.yield` only appears when a
  loop itself *produces* a value — i.e. a true parallel **reduction**, where
  `affine.parallel` carries reduction results that get combined (sum, max, …) at
  the end. (Step 4's convolution uses the value-carrying form, via `iter_args` on
  `affine.for`.) The `k`-loop here reduces too, but it does so the same
  through-memory way, which is why it has no yield either.

`build.sh` emits that parallelized IR to `build/matmul_parallel.mlir` for you to
read, then — on a **separate branch** — lowers the original *sequential*
`matmul.mlir` with `-lower-affine` → `scf` → `llvm`, compiles it, and runs
`aot_main.py`, which checks the result against NumPy's `@`.

> **The compiled matmul runs single-threaded.** `--affine-parallelize` is shown
> here only as an IR *rewrite*; the parallel version is **not** the one we
> compile. And the default lowering wouldn't preserve it anyway: `-lower-affine`
> turns `affine.parallel` into `scf.parallel`, but `-convert-scf-to-cf` then
> *serializes* `scf.parallel` into ordinary branches. To get real parallel
> execution you must route `scf.parallel` to a backend that keeps it —
> `-convert-scf-to-openmp` for CPU threads (Step 5) or the GPU passes
> (Chapter 8). So `matmul_opt.mlir` has no threads, no SIMD, and no unrolling —
> it's a plain sequential triple loop (one `llvm.fmul` in the body); all its bulk
> is *lowering*, not parallelization.

**Run:** `cd 1_affine_matmul && bash build.sh`

```
Affine matmul successful! (max abs error 1.19e-07)
```

(The tiny error is ordinary float rounding — the kernel and NumPy sum in different
orders.)

### What the lowering produces (`build/matmul_opt.mlir`)

`build.sh` also lowers the kernel all the way to the `llvm` dialect and leaves the
result in `build/matmul_opt.mlir`. It's verbose, but every part is something you've
already seen — it's worth opening once to connect the dots:

**The signature explodes.** `@matmul` takes **21 scalar arguments**, not three
memrefs: `-finalize-memref-to-llvm` unrolls each `memref<?x?xf32>` into
`(allocated_ptr, aligned_ptr, offset, size0, size1, stride0, stride1)`. The first
thing the body does is `llvm.insertvalue` those scalars back into a descriptor
struct — exactly the one from Chapter 2:

```mlir
llvm.func @matmul(%arg0: !llvm.ptr, %arg1: !llvm.ptr, %arg2: i64, /* ... */ %arg20: i64) {
  %0 = llvm.mlir.undef : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
  %1 = llvm.insertvalue %arg14, %0[0] : !llvm.struct<...>   // C: allocated ptr
  %2 = llvm.insertvalue %arg15, %1[1] : !llvm.struct<...>   // C: aligned ptr
  // ... [2]=offset, [3,0..1]=sizes, [4,0..1]=strides; then again for B, then A
```

**The `?` dimensions become runtime loads.** `memref.dim` is gone; each `?` size
is pulled out of the descriptor's size array (`extractvalue ...[3]`) at runtime:

```mlir
%27 = llvm.extractvalue %23[3] : !llvm.struct<...>   // A's [size0, size1] array
// ... indexed + loaded to give %M, %N, %K as i64 loop bounds
```

**The loops became blocks and branches.** No loop ops remain — the three
`affine.for`s are `^bb1`…`^bb9` wired with `llvm.cond_br`/`llvm.br` (the
`affine → scf → cf` lowering, the same shape as Chapter 1's loop). We lower the
**sequential** kernel, so it's a plain nest, not the parallel version above:

```mlir
  llvm.br ^bb1(%41 : i64)
^bb1(%43: i64):                          // the i loop
  %44 = llvm.icmp "slt" %43, %30 : i64   // i < M ?
  llvm.cond_br %44, ^bb2, ^bb9
// ^bb3(%47) is the j loop, ^bb5(%51) the k loop, each the same shape
```

**`affine.load/store` became pointer math.** Each access is a `llvm.mul`/`add`
computing the row-major offset `i*stride0 + k` (Chapter 2), a `getelementptr`, and
a `load`/`store`; `arith.mulf`/`addf` are `llvm.fmul`/`fadd`:

```mlir
%53 = llvm.extractvalue %23[1]              // A aligned ptr
%54 = llvm.extractvalue %23[4, 0]           // A stride0
%55 = llvm.mul %43, %54 : i64               // i * stride0
%56 = llvm.add %55, %51 : i64               // + k
%57 = llvm.getelementptr %53[%56] : (!llvm.ptr, i64) -> !llvm.ptr, f32   // &A[i,k]
%58 = llvm.load %57 : !llvm.ptr -> f32      // A[i,k]
// ... likewise B[k,j], C[i,j]; then:
%71 = llvm.fmul %58, %64 : f32              // A[i,k] * B[k,j]
%72 = llvm.fadd %70, %71 : f32              // C[i,j] + ...
```

**There are two functions.** The real `@matmul` (21 unrolled args) plus a generated
`@_mlir_ciface_matmul` wrapper that takes three descriptor *pointers*, loads each
struct, unpacks its fields, and calls `@matmul` — the C-interface marshalling from
Chapter 2's Step 4, and what `aot_main.py` actually calls:

```mlir
llvm.func @_mlir_ciface_matmul(%arg0: !llvm.ptr, %arg1: !llvm.ptr, %arg2: !llvm.ptr) {
  %0 = llvm.load %arg0 : !llvm.ptr -> !llvm.struct<...>   // load A's descriptor
  %1 = llvm.extractvalue %0[0] : !llvm.struct<...>        // unpack its 7 fields
  // ... unpack B and C too, then:
  llvm.call @matmul(%1, %2, /* ...21 scalars... */) : (...) -> ()
}
```

In short: affine math → scalarized loops over raw pointers, ready for
`mlir-translate` to hand to LLVM.

---

## Step 2 — Affine maps & `affine.apply` (`2_affine_maps/`) · 🧩

**Goal:** the index arithmetic that makes the whole dialect tick.

Affine maps are the affine dialect's workhorse — multi-dimensional affine
transformations that spell out *how loop indices map to the memory each iteration
touches*. That makes them the lever for the optimizations this chapter is about:
once the compiler knows the access pattern as an affine map, it can reason about
memory layout and locality, which is exactly what high-performance code on CPUs
and GPUs lives or dies by.

Concretely, an **affine map** is a named function from loop indices (*dimensions*)
and runtime-constant parameters (*symbols*) to `index` results; `affine.apply`
evaluates one. The dims-vs-symbols split — `(d0, d1)[s0]` — matters: symbols are
values that are loop-invariant, so the analysis treats them as constants.

`affine_apply.mlir` works through tiling, symbols, multi-dim maps, and composition
— the whole file:

```mlir
// Reusable named maps (the `#name` attribute-alias sigil from Chapter 1):
// Define some resuable affine maps
#tile_map    = affine_map<(d0) -> (d0 floordiv 32)>          // which 32-wide tile?
#offset_map  = affine_map<(d0)[s0] -> (d0 + s0)>            // shift by a symbol
#complex_map = affine_map<(d0, d1)[s0] -> (d0 * 2 + d1 floordiv 4 + s0)>

func.func @affine_apply_examples() {
  // create some test indices
  %c0   = arith.constant 0   : index
  %c42  = arith.constant 42  : index
  %c128 = arith.constant 128 : index

  // Example 1. Simple Tiling Calculation: 42 floordiv 32 = 1 (element 42 lives in tile #1).
  %tile = affine.apply #tile_map(%c42)

  // Example 2. An inline affine (anonymous) map for an offset: 42 + 10 = 52.
  %off  = affine.apply affine_map<(i) -> (i + 10)>(%c42)

  // Example 3. Using a symbol parameter: 42 + 128 = 170.  Dims in (), symbols in [].
  %shift = affine.apply #offset_map(%c42)[%c128]

  // Example 4. Multiple dims and a symbol: 42*2 + 128 floordiv 4 + 0 = 116.
  %cplx = affine.apply #complex_map(%c42, %c128)[%c0]

  // Example 5. Composition of affine maps — feed one apply's result into the next.
  %t     = affine.apply affine_map<(i) -> (i * 2)>(%c42)   // 84
  %final = affine.apply affine_map<(i) -> (i + 5)>(%t)     // 89

  return
}
```

`tiled_loop.mlir` shows the real use — `affine.apply` turning a
`(tile, within-tile)` pair into a flat array index inside a tiled loop nest:

```mlir
// Example showing how affine.apply can be used in a practical loop context
func.func @tiled_loop(%arg0: memref<256xf32>) {
  affine.for %i = 0 to 256 step 32 {       // outer: one iteration per tile
    affine.for %j = 0 to 32 {              // inner: elements within the tile
      %idx = affine.apply affine_map<(d0, d1) -> (d0 + d1)>(%i, %j)
      %val = memref.load %arg0[%idx] : memref<256xf32>
      // ... process %val ...
      memref.store %val, %arg0[%idx] : memref<256xf32>
    }
  }
  return
}
```

This is `affine.apply` doing real work: a tiled traversal of an array. The outer
loop strides by the tile size and the inner loop walks one tile, and `affine.apply`
is what turns those two loop counters into the flat element index the loads/stores
use — the everyday pattern behind cache tiling.

A few rules that pin down what an affine map can be:

- **`floordiv` is integer division rounding toward −∞** (and `ceildiv` rounds up);
  `mod` is the matching remainder.
- **`(d0)[s0]` = one dimension `d0` plus one symbol `s0`** — dims in `()`, symbols
  in `[]`.
- **Only a fixed vocabulary is allowed:** `+`, `-`, multiplication *by a constant*,
  and division / `mod` / `floordiv` / `ceildiv` *by a constant*. No two variables
  multiplied, no data-dependent terms — that linearity is what makes the maps
  analyzable.
- **The result is always `index`.** Maps compute positions, never data.

`build.sh` parses both files and writes four outputs into `build/`:

| File | Pass | What it shows |
| --- | --- | --- |
| `affine_apply_verified.mlir` | (parse + print) | the input round-tripped; note the *inline* maps get hoisted into named `#map`s and deduplicated |
| `affine_apply_folded.mlir` | `-canonicalize` | the body collapses to just `return` — every `affine.apply` is **side-effect-free and its result is unused**, so it's dead code and gets deleted (the same value-semantics point as tensors in Chapter 2) |
| `tiled_loop_verified.mlir` | (parse + print) | the tiled loop round-tripped, still in the `affine` dialect |
| `tiled_loop_lowered.mlir` | `-lower-affine` | the affine ops expand: `affine.for` → `scf.for` (bounds/step spelled out as `arith.constant`s) and `affine.apply<(d0,d1)->(d0+d1)>` → a plain `arith.addi` |

So the pair shows both directions: `affine.apply` is *erasable* when its result
isn't used (the folded file), and *lowers to ordinary index arithmetic* when it is
(the lowered file).

**Run:** `cd 2_affine_maps && bash build.sh` (then read the files in `build/`).

---

## Step 3 — Affine optimization passes (`3_affine_opts/`) · 🧩

**Goal:** the payoff of first-class loops — a whole catalog of automatic
transformations. Each file is a minimal *before*; running the matching pass shows
the *after*. For example, `licm.mlir` has a constant computed pointlessly inside
the loop:

```mlir
func.func @licm(%A: memref<10xf32>, %B: memref<10xf32>) {
  affine.for %i = 0 to 10 {
    %x = arith.constant 42.0 : f32           // loop-invariant — gets hoisted
    %v = affine.load %A[%i] : memref<10xf32>
    %s = arith.addf %v, %x : f32
    affine.store %s, %B[%i] : memref<10xf32>
  }
  return
}
```

Running each pass (and the others — `tiling.mlir`, `unroll.mlir`, `fusion.mlir`,
all just as small):

```bash
mlir-opt licm.mlir   -affine-loop-invariant-code-motion   # hoist %x above the loop
mlir-opt tiling.mlir -affine-loop-tile="tile-size=8"      # block for cache
mlir-opt unroll.mlir -affine-loop-unroll="unroll-factor=4"
mlir-opt fusion.mlir -affine-loop-fusion                  # merge adjacent loops
```

`build.sh` runs all four and writes the results to `build/<name>_after.mlir`, so
you can `diff licm.mlir build/licm_after.mlir` and watch the constant hop out of
the loop, the 32×32 nest grow tile loops, the body replicate 4×, and two loops
collapse into one.

The full pass catalog (all available in `mlir-opt`):

| Pass | Transformation |
| --- | --- |
| `-affine-loop-invariant-code-motion` | Hoist code that doesn't depend on the loop index |
| `-affine-loop-tile` | Block a loop into cache-friendly tiles |
| `-affine-loop-unroll` / `-unroll-jam` | Replicate the body (and jam nested loops) |
| `-affine-loop-fusion` | Merge adjacent loops for locality |
| `-affine-loop-coalescing` | Flatten nested loops into one |
| `-affine-loop-normalize` | Normalize bounds/steps to start at 0, step 1 |
| `-affine-parallelize` | Turn independent loops into `affine.parallel` |
| `-affine-super-vectorize` | Vectorize to an n-D vector abstraction |
| `-affine-scalrep` | Forward stores to loads, kill redundant loads |
| `-affine-pipeline-data-transfer` | Pipeline DMA between memory levels |

Two transformations the PDF shows that don't have a one-flag pass —
**interchange** (swap loop nesting order for locality) and **skewing** (shear the
iteration space to expose parallelism) — are expressed by rewriting the loop
bounds / `affine.apply` index maps by hand; the source files' comments point at
where.

There's also `-convert-affine-for-to-gpu`, which turns affine loops straight into
GPU kernels — the bridge to a later chapter.

---

## Step 4 — 2-D convolution (`4_convolution/`) · ✅

**Goal:** a real kernel — and the affine version of Chapter 1's loop-carried
value.

Convolution (really *cross-correlation*) slides a small filter over an input:

$$(A * K)_{ij} = \sum_{u}\sum_{v} K_{uv}\, A_{(i+u)(j+v)}$$

The two outer loops walk the output; the two inner loops reduce over the filter
window, accumulating with **`iter_args`** — the same loop-carried-value mechanism
`scf.for` used in Chapter 1, now on `affine.for`, with each loop yielding its final
accumulator via `affine.yield`:

```mlir
%acc = affine.for %fi = 0 to %KH iter_args(%a = %zero) -> (f32) {
  %acc2 = affine.for %fj = 0 to %KW iter_args(%b = %a) -> (f32) {
    %fv = affine.load %filter[%fi, %fj] : memref<?x?xf32>
    %iv = affine.load %input[%i + %fi, %j + %fj] : memref<?x?xf32>   // affine index!
    %n  = arith.addf %b, arith.mulf %iv, %fv : f32
    affine.yield %n : f32
  }
  affine.yield %acc2 : f32
}
```

The index `%input[%i + %fi, %j + %fj]` is an affine function of the loop
variables — which is exactly what keeps even a sliding-window access analyzable.
Dimensions are dynamic, so one compiled kernel handles any sizes; the output shape
is `(in_h − k_h + 1, in_w − k_w + 1)`, which the driver allocates. `aot_main.py`
checks the result against a NumPy reference.

**Run:** `cd 4_convolution && bash build.sh`

```
Affine conv2d successful! (output 8x8, max abs error 3.81e-06)
```

---

## Step 5 — OpenMP (`5_openmp/`) · ✅

**Goal:** the other end of the spectrum — *explicit* parallelism, lowered to a
real threading runtime.

Where affine has the compiler *discover* parallelism, OpenMP has you *declare* it,
exactly like `#pragma omp parallel for` in C. The `omp` dialect exposes the same
primitives as first-class ops:

| Op | Meaning |
| --- | --- |
| `omp.parallel` | Fork a team of threads; its region runs on each |
| `omp.wsloop` + `omp.loop_nest` | A worksharing loop — split iterations across the team |
| `omp.barrier` | Make all threads wait until everyone arrives |
| `omp.terminator` / `omp.yield` | Required region / loop-body terminators |

`omp_double.mlir` doubles a 10-element array across a thread team, then `main`
prints it — a standalone **executable**:

```mlir
omp.parallel {
  omp.wsloop {
    omp.loop_nest (%i) : i32 = (%lb) to (%ub) inclusive step (%step) {
      %v = memref.load %input[...] : memref<10xf32>
      %r = arith.mulf %v, %two : f32
      memref.store %r, %output[...] : memref<10xf32>
      omp.yield
    }
  }
  omp.barrier
  omp.terminator
}
```

**You rarely write this by hand.** The usual route is to produce `scf.parallel`
(by writing it, or via `--affine-parallelize` + `-lower-affine`) and let
`-convert-scf-to-openmp` rewrite it into `omp.parallel`/`omp.wsloop`. That's all
`scf_parallel.mlir` is — a plain parallel loop:

```mlir
func.func @add(%A: memref<100xf32>, %B: memref<100xf32>) {
  %c0   = arith.constant 0   : index
  %c1   = arith.constant 1   : index
  %c100 = arith.constant 100 : index
  scf.parallel (%i) = (%c0) to (%c100) step (%c1) {
    %v = memref.load %A[%i] : memref<100xf32>
    %r = arith.addf %v, %v : f32
    memref.store %r, %B[%i] : memref<100xf32>
    scf.reduce
  }
  return
}
```

`build.sh` runs `-convert-scf-to-openmp` on it and saves the `omp.parallel`/
`omp.wsloop` result to `build/scf_parallel_omp.mlir`; a real pipeline then
continues with `-convert-openmp-to-llvm`.

> **Aside — variadic `printf`.** `main` prints via C's `printf`, declared
> `llvm.func @printf(!llvm.ptr, ...) -> i32`, called with
> `llvm.call @printf(%fmt, %x) vararg(...)`. One gotcha baked into the example: C
> varargs promote `float` to `double`, so we `arith.extf` each `f32` to `f64`
> before the call (the format string is `"%f\n"`).

**Toolchain note.** OpenMP needs a runtime, and `llvm@20` doesn't bundle one, so
`build.sh` links Homebrew's `libomp` (`brew install libomp`) and bakes its path in
with `-Wl,-rpath`. (A harmless linker warning about the macOS version may appear.)

**Run:** `cd 5_openmp && bash build.sh`

```
2.000000
4.000000
... (6, 8, 10, 12, 14, 16, 18)
20.000000
```

---

## Run everything

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"   # llvm@20 is keg-only
brew install libomp                                  # for Step 5
# Python deps for the drivers:  pip install numpy

for d in 1_affine_matmul 2_affine_maps 3_affine_opts 4_convolution 5_openmp; do
  echo "=== $d ==="; ( cd "$d" && bash build.sh ) 2>&1 | tail -n 3
done
```

Each step writes its intermediates (`*_opt.mlir`, `*.ll`, `*.o`, `*.dylib`,
executables) into a local `build/` directory.

## Key takeaways

- **A loop over-specifies order.** Parallelization is recovering the independence
  the sequential syntax hid — and the affine dialect is what lets the compiler
  *see* that independence.
- **`affine` = analyzable loops.** Restricting indices to affine expressions buys
  the polyhedral model, and with it automatic parallelize / tile / fuse /
  interchange. Write `affine`, transform it, then `-lower-affine` to `scf` and
  reuse the Chapter 1–2 pipeline.
- **`affine.parallel` / `iter_args`** express independent iterations and
  loop-carried reductions; `affine.apply` does the affine index math.
- **OpenMP is the explicit alternative.** `omp.parallel` + `omp.wsloop` declare
  parallelism directly; `-convert-scf-to-openmp` gets you there from `scf.parallel`
  without hand-writing the dialect.
- **Two philosophies, one backend.** Automatic (affine) and manual (OpenMP) both
  lower through the same `scf`/`llvm` machinery you already know.

**Next:** Part 4 — Linear algebra and the `linalg` dialect (see
[`../reference/`](../reference/)).
