# 4 — Linear Algebra: the `linalg` Dialect

### Stop writing loops. Name the operation.

Chapter 3 made loops *first-class* so the compiler could optimize them — but you
still **wrote the loops**. You spelled out the three nested `affine.for`s of a
matrix multiply, and only then did the optimizer get to work. `linalg` takes the
next step and asks a sharper question: why write loops at all? A matrix multiply
*is* a matrix multiply. Say **that**, and let the compiler generate the loops,
choose their order, tile them, and fuse them with their neighbours.

That is the whole idea of the `linalg` dialect: a small set of **structured
operations** that describe linear-algebra computations *declaratively* — the
**what**, not the **how**. `linalg.matmul`, `linalg.add`, `linalg.broadcast`,
convolutions, reductions — each is a single op that still carries enough structure
for the compiler to reason about. This is the level real ML compilers actually
target, because chains of these ops (a neural network is little else) can be fused
and tiled *as a group* in ways hand-written loops never could.

Think of it as one more rung above Chapter 3. There, an `affine.for` nest was a
*shape* the polyhedral model could slice and tile — but the shape was still
implied by loops you typed out. Here the operation names itself, and the loop
nest is something the compiler *derives on demand*. You have climbed from "loops
the compiler can analyze" to "loops the compiler writes for you."

> Based on Stephen Diehl's *"MLIR Part 4 — Linear Algebra in MLIR"*
> ([`../reference/`](../reference/)). Every runnable example is built and checked
> against NumPy on this toolchain (Homebrew LLVM 20.1.8, Apple Silicon).

---

## What a structured op actually is

A `linalg` op is **payload + structure**. The *payload* is the scalar math (an
`addf`, a `mulf`) written in a region; the *structure* is the iteration space and
how each tensor is indexed within it. Nothing in between — no loop keywords, no
index arithmetic. The op *is* the pair:

```text
   ┌─────────────────────── linalg.generic ───────────────────────┐
   │  STRUCTURE                          PAYLOAD                    │
   │  indexing_maps  = [...]   ─┐        ^bb0(%a, %b, %out):        │
   │  iterator_types = [...]    ├──────►   %s = arith.addf %a, %b   │
   │  ins(...) outs(...)       ─┘          linalg.yield %s          │
   │  "which element, in what order"     "what to compute per elt"  │
   └───────────────────────────────────────────────────────────────┘
      the compiler expands this into a loop nest on demand
```

The most general op, `linalg.generic`, spells the structure out explicitly with
two attributes:

- **`indexing_maps`** — one affine map per operand (inputs *and* outputs), saying
  how that tensor is indexed by the loop variables. For elementwise add, all three
  maps are `(d0) -> (d0)`: element `i` of each tensor maps to loop index `i`. For
  matmul they are `(i,j,k)->(i,k)`, `(i,j,k)->(k,j)`, `(i,j,k)->(i,j)` — exactly
  $A_{ik}$, $B_{kj}$, $C_{ij}$. (These are the same affine maps from Chapter 3,
  now used to *describe* an op rather than index a hand-written loop.)
- **`iterator_types`** — one per loop dimension, classifying it:

  | Iterator | Meaning | Example |
  | --- | --- | --- |
  | `parallel` | iterations are independent | elementwise; the `i, j` of matmul |
  | `reduction` | iterations accumulate into one value | the `k` sum of matmul |
  | `window` | iterations slide a window | convolution |

Everything else in the dialect is `linalg.generic` with the boilerplate hidden:

| Level | Op | What it is |
| --- | --- | --- |
| Most general | `linalg.generic` | the raw payload+structure form |
| Elementwise | `linalg.map { op }`, `linalg.add`/`sub`/`mul`/`div` | apply a scalar op per element |
| Reduction | `linalg.reduce { op }` | collapse a dimension (sum, product, …) |
| Contraction | `linalg.matmul` (and friends) | the named matrix multiply |
| Shape | `linalg.broadcast` | stretch a smaller tensor to a larger shape |

Because the structure is explicit, lowering is just *generating the loop nest the
maps imply* — `-convert-linalg-to-loops` (→ `scf`) or
`-convert-linalg-to-affine-loops` (→ `affine`, Chapter 3). From there it's the
same pipeline as every earlier chapter.

### Tensors vs. memrefs at this level

Two of these steps operate on **tensors** (SSA values, Chapter 2) and two on
**memrefs** (buffers). The rule is the one from Chapter 2: a named op writing
into a caller-provided memref `outs` buffer works *in place* and needs no
allocation, so it lowers straight to loops. A `linalg` op that *returns* a
`tensor` must first be **bufferized** (tensors → memrefs) before it can run —
which is exactly why Steps 1–3 (memref, runnable) skip bufferization while the
inspect-only tiling step (Step 5, tensors) runs a `--one-shot-bufferize`
pipeline. Same ops, different data model.

---

## Chapter layout

| Step | Directory | Topic | Runnable? |
| --- | --- | --- | --- |
| 1 | [`1_elementwise/`](1_elementwise/) | `linalg.generic`, `linalg.map`, named ops | ✅ Python |
| 2 | [`2_matmul/`](2_matmul/) | `linalg.matmul` (and its generic form) | ✅ Python |
| 3 | [`3_broadcast/`](3_broadcast/) | `linalg.broadcast` + elementwise add | ✅ Python |
| 4 | [`4_fusion/`](4_fusion/) | fusing elementwise ops into one loop | 🧩 inspect |
| 5 | [`5_tiling/`](5_tiling/) | tiling a matmul for cache locality | 🧩 inspect |

Shared helper: [`common/np_memref.py`](common/np_memref.py) — 1-D **and** 2-D
memref descriptors + NumPy adapters (the same zero-copy ctypes bridge as
Chapters 2–3).

**Legend:** ✅ runnable · 🧩 snippet (transformed/inspected with `mlir-opt`, not run).

---

## Step 1 — Elementwise: `generic`, `map`, named ops (`1_elementwise/`) · ✅

**Goal:** meet the fundamental `linalg.generic`, then watch it collapse to a
one-liner.

`generic_add.mlir` writes an elementwise tensor add **the long way** — three
identity `indexing_maps`, one `parallel` iterator, and a region that does the
`arith.addf`. It's verbose on purpose: every other Linalg op is this op with the
boilerplate removed.

*1_elementwise/generic_add.mlir*
```mlir
#map_1d = affine_map<(d0) -> (d0)>

func.func @add_tensors(%arg0: tensor<10xf32>, %arg1: tensor<10xf32>) -> tensor<10xf32> {
  %result = tensor.empty() : tensor<10xf32>
  %0 = linalg.generic {
      indexing_maps  = [#map_1d, #map_1d, #map_1d],   // arg0, arg1, result
      iterator_types = ["parallel"]
    }
    ins(%arg0, %arg1 : tensor<10xf32>, tensor<10xf32>)
    outs(%result : tensor<10xf32>) {
  ^bb0(%a: f32, %b: f32, %out: f32):
    %1 = arith.addf %a, %b : f32
    linalg.yield %1 : f32
  } -> tensor<10xf32>
  return %0 : tensor<10xf32>
}
```

"Structured" means the loops are *implied*, not written. Ask the compiler to make
them concrete and the maps become a plain loop nest:

```bash
$ mlir-opt 1_elementwise/generic_add.mlir -convert-linalg-to-loops
```

The `linalg.generic` here stays a `linalg.generic` when only round-tripped, but
the same op lowered to loops is exactly the elementwise `scf.for` you'd have
written by hand — the identity maps become `A[i]`, `B[i]`, `result[i]`. `build.sh`
saves the round-tripped form to `build/generic_add_loops.mlir`:

*build/generic_add_loops.mlir*
```mlir
#map = affine_map<(d0) -> (d0)>
module {
  func.func @add_tensors(%arg0: tensor<10xf32>, %arg1: tensor<10xf32>) -> tensor<10xf32> {
    %0 = tensor.empty() : tensor<10xf32>
    %1 = linalg.generic {indexing_maps = [#map, #map, #map], iterator_types = ["parallel"]} ins(%arg0, %arg1 : tensor<10xf32>, tensor<10xf32>) outs(%0 : tensor<10xf32>) {
    ^bb0(%in: f32, %in_0: f32, %out: f32):
      %2 = arith.addf %in, %in_0 : f32
      linalg.yield %2 : f32
    } -> tensor<10xf32>
    return %1 : tensor<10xf32>
  }
}
```

The **named op** is that whole thing as a single line. Because it writes into a
memref `outs` buffer in place, no bufferization is needed:

*1_elementwise/add.mlir*
```mlir
func.func @addv(%a: memref<10xf32>, %b: memref<10xf32>, %c: memref<10xf32>)
    attributes {llvm.emit_c_interface} {
  linalg.add ins(%a, %b : memref<10xf32>, memref<10xf32>)
             outs(%c : memref<10xf32>)
  return
}
```

`build.sh` lowers this named version to a runnable library (and saves the
loop-lowered generic form above for comparison). The driver checks `c == a + b`.

**Run:** `cd 1_elementwise && bash build.sh`

```
linalg.add successful!
[10. 11. 12. 13. 14. 15. 16. 17. 18. 19.]
```

---

## Step 2 — `linalg.matmul` (`2_matmul/`) · ✅

**Goal:** a whole matrix multiply as one op — contrast it with Chapter 3's
hand-written nest.

*2_matmul/matmul.mlir*
```mlir
func.func @matmul(%A: memref<8x10xf32>, %B: memref<10x16xf32>, %C: memref<8x16xf32>)
    attributes {llvm.emit_c_interface} {
  linalg.matmul ins(%A, %B : memref<8x10xf32>, memref<10x16xf32>)
                outs(%C : memref<8x16xf32>)
  return
}
```

That single line is sugar for the `linalg.generic` whose maps and iterators encode
$C_{ij} = \sum_k A_{ik}B_{kj}$ — the contraction where two indices are free and one
is summed away:

```text
        k                j                    j
      ┌────┐          ┌──────┐             ┌──────┐
   i  │ A  │    ×   k │  B   │     =     i  │  C   │
      └────┘          └──────┘             └──────┘
     (i,k)            (k,j)                (i,j)

   iterator_types: i = parallel   j = parallel   k = reduction
   C[i,j] = Σ_k A[i,k]·B[k,j]   — sweep k, accumulate; i,j independent
```

Written out, that structure is:

```mlir
linalg.generic {
  indexing_maps  = [affine_map<(i,j,k)->(i,k)>,   // A
                    affine_map<(i,j,k)->(k,j)>,   // B
                    affine_map<(i,j,k)->(i,j)>],  // C
  iterator_types = ["parallel", "parallel", "reduction"]   // i, j free; k summed
} ins(%A, %B : ...) outs(%C : ...) {
^bb0(%a: f32, %b: f32, %acc: f32):
  %p = arith.mulf %a, %b : f32
  %s = arith.addf %acc, %p : f32
  linalg.yield %s : f32
}
```

`-convert-linalg-to-loops` turns either form into an `scf` nest. To see that the
named op really *is* Chapter 3's matmul, `build.sh` also emits the
`-convert-linalg-to-affine-loops` version — an `affine.for` nest identical in
shape to the one you wrote by hand there:

*build/matmul_affine.mlir*
```mlir
module {
  func.func @matmul(%arg0: memref<8x10xf32>, %arg1: memref<10x16xf32>, %arg2: memref<8x16xf32>) attributes {llvm.emit_c_interface} {
    affine.for %arg3 = 0 to 8 {
      affine.for %arg4 = 0 to 16 {
        affine.for %arg5 = 0 to 10 {
          %0 = affine.load %arg0[%arg3, %arg5] : memref<8x10xf32>
          %1 = affine.load %arg1[%arg5, %arg4] : memref<10x16xf32>
          %2 = affine.load %arg2[%arg3, %arg4] : memref<8x16xf32>
          %3 = arith.mulf %0, %1 : f32
          %4 = arith.addf %2, %3 : f32
          affine.store %4, %arg2[%arg3, %arg4] : memref<8x16xf32>
        }
      }
    }
    return
  }
}
```

That's the whole point of the level: *one line* upstream, but by the time it's
loops it's exactly what you'd have hand-written — and the compiler was free to
choose the order. The driver checks the result against NumPy's `@` (the kernel
accumulates into `C`, so it's passed zeroed).

**Run:** `cd 2_matmul && bash build.sh`

```
linalg.matmul successful! (8x10 @ 10x16, max abs error 2.38e-07)
```

(Inputs are random each run, so the exact error varies; it's always a tiny
float-rounding value — the kernel and NumPy sum the `k` dimension in different
orders.)

---

## Step 3 — Broadcasting (`3_broadcast/`) · ✅

**Goal:** NumPy-style broadcasting — combining tensors of different shapes.

Adding a length-4 vector to every row of a 3×4 matrix is `matrix + vector` in
NumPy. The shapes don't match, so something has to *stretch* the vector to fill
the missing dimension:

```text
   vector (4,)        broadcast dim 0        matrix (3,4)      result (3,4)
   [1 2 3 4]   ──────────────────────►   [1 1 1 1]   +    [2 3 4 5]
                 replicate across            [1 1 1 1]   =    [2 3 4 5]
                 the 3 rows                  [1 1 1 1]        [2 3 4 5]
                                          (each row now [1,2,3,4])
```

In Linalg it's `linalg.broadcast` (stretch the vector across the rows) followed by
an elementwise add, both writing the caller's `%out` buffer in place:

*3_broadcast/add_vec_to_mat.mlir*
```mlir
func.func @add_vec_to_mat(%matrix: memref<3x4xf32>, %vector: memref<4xf32>, %out: memref<3x4xf32>)
    attributes {llvm.emit_c_interface} {
  // 1. broadcast the vector across the 3 rows of %out
  linalg.broadcast ins(%vector : memref<4xf32>)
                   outs(%out : memref<3x4xf32>)
                   dimensions = [0]
  // 2. add the matrix elementwise into %out (in place)
  linalg.add ins(%matrix, %out : memref<3x4xf32>, memref<3x4xf32>)
             outs(%out : memref<3x4xf32>)
  return
}
```

`dimensions = [0]` names the axis the input *lacks* — dimension 0, the rows — so
the vector is replicated along it. The driver checks against NumPy's own
broadcasting (`matrix + vector`).

**Run:** `cd 3_broadcast && bash build.sh`

```
linalg.broadcast + add successful!
[[2. 3. 4. 5.]
 [2. 3. 4. 5.]
 [2. 3. 4. 5.]]
```

---

## Step 4 — Kernel fusion (`4_fusion/`) · 🧩

**Goal:** the optimization that makes structured ops pay off.

`separate_ops.mlir` computes `(a + b) * c` as **two** named ops on tensors. Run
as written, that's two loops: the add materializes a whole intermediate tensor to
memory, then the mul reads it back.

*4_fusion/separate_ops.mlir*
```mlir
module {
  func.func @addmul(%a: tensor<10xf32>, %b: tensor<10xf32>, %c: tensor<10xf32>) -> tensor<10xf32> {
    %0 = tensor.empty() : tensor<10xf32>
    %1 = linalg.add ins(%a, %b : tensor<10xf32>, tensor<10xf32>)
                    outs(%0 : tensor<10xf32>) -> tensor<10xf32>
    %2 = tensor.empty() : tensor<10xf32>
    %3 = linalg.mul ins(%1, %c : tensor<10xf32>, tensor<10xf32>)
                    outs(%2 : tensor<10xf32>) -> tensor<10xf32>
    return %3 : tensor<10xf32>
  }
}
```

**Fusion** merges the two into a single loop that reads each input once and writes
the result directly — no intermediate tensor, no second pass over memory:

```text
   separate                              fused
   ┌────────────────┐                    ┌──────────────────────┐
   │ for i: t[i]=a+b│  writes t          │ for i:               │
   └────────────────┘  ───► memory ───►  │   r[i] = (a[i]+b[i])  │
   ┌────────────────┐  reads t           │          * c[i]       │
   │ for i: r[i]=t*c│                    │  (t never exists)     │
   └────────────────┘                    └──────────────────────┘
   two loops + temp array                one loop, no temp
```

Run the fusion pipeline:

```bash
$ mlir-opt 4_fusion/separate_ops.mlir \
    --canonicalize \
    --linalg-fuse-elementwise-ops \
    --cse \
    --linalg-generalize-named-ops \
    --linalg-fuse-elementwise-ops
```

The two named ops have become **one `linalg.generic`** whose body does both the
`addf` and the `mulf` before a single `linalg.yield` — note it now has four
`indexing_maps` (`a`, `b`, `c`, and the output all indexed `(d0)->(d0)`) and no
intermediate `tensor.empty` for `t`. `build.sh` writes this to
`build/fused_ops.mlir`:

*build/fused_ops.mlir*
```mlir
#map = affine_map<(d0) -> (d0)>
module {
  func.func @addmul(%arg0: tensor<10xf32>, %arg1: tensor<10xf32>, %arg2: tensor<10xf32>) -> tensor<10xf32> {
    %0 = tensor.empty() : tensor<10xf32>
    %1 = linalg.generic {indexing_maps = [#map, #map, #map, #map], iterator_types = ["parallel"]} ins(%arg0, %arg1, %arg2 : tensor<10xf32>, tensor<10xf32>, tensor<10xf32>) outs(%0 : tensor<10xf32>) {
    ^bb0(%in: f32, %in_0: f32, %in_1: f32, %out: f32):
      %2 = arith.addf %in, %in_0 : f32
      %3 = arith.mulf %2, %in_1 : f32
      linalg.yield %3 : f32
    } -> tensor<10xf32>
    return %1 : tensor<10xf32>
  }
}
```

This is the bread-and-butter optimization for deep-learning graphs, which are
mostly long chains of elementwise ops — it's the `linalg`-op cousin of Chapter 3's
`-affine-loop-fusion` (which fused *loops*; this fuses *ops* before any loop
exists).

**Run:** `cd 4_fusion && bash build.sh` (then `diff separate_ops.mlir build/fused_ops.mlir`).

---

## Step 5 — Tiling (`5_tiling/`) · 🧩

**Goal:** restructure a matmul for cache locality.

On modern hardware, memory access — not arithmetic — is usually the bottleneck.
**Tiling** breaks a big computation into cache-sized blocks so data stays hot
while it's reused. We start from a one-line `linalg.matmul` on **tensors**:

*5_tiling/matmul_tile.mlir*
```mlir
module {
  func.func @matmul(%a: tensor<10x10xf32>, %b: tensor<10x10xf32>,
                    %c: tensor<10x10xf32>) -> tensor<10x10xf32> {
    %0 = linalg.matmul ins(%a, %b : tensor<10x10xf32>, tensor<10x10xf32>)
                       outs(%c : tensor<10x10xf32>) -> tensor<10x10xf32>
    return %0 : tensor<10x10xf32>
  }
}
```

Because these are tensors that get *returned*, the pipeline first bufferizes them
(tensors → memrefs, Chapter 2), then lowers to affine loops, then tiles:

```bash
$ mlir-opt 5_tiling/matmul_tile.mlir \
    --convert-tensor-to-linalg \
    --linalg-generalize-named-ops \
    --one-shot-bufferize="bufferize-function-boundaries" \
    --buffer-deallocation-pipeline \
    --convert-bufferization-to-memref \
    --convert-linalg-to-affine-loops \
    --affine-loop-tile="tile-size=5" \
    --canonicalize --cse
```

The 3-loop matmul becomes a **6-deep nest**: three outer *tile* loops stepping by
5 over blocks, three inner *point* loops (bounds `#map = (d0)->(d0)` and
`#map1 = (d0)->(d0+5)`) walking the 5 elements inside each tile — the same
tile/point split you saw in Chapter 3's `-affine-loop-tile`, now applied to a
matmul the compiler generated from one `linalg.matmul`. `build.sh` writes it to
`build/matmul_tiled.mlir`:

```text
   before: sweep the whole            after: 5×5×5 blocks, one at a time
   10×10×10 space
   for i in 0..10                     for i0 in 0..10 step 5      ┐
     for j in 0..10                     for j0 in 0..10 step 5    │ tile loops
       for k in 0..10                     for k0 in 0..10 step 5  ┘ (step 5)
         C[i,j] += A[i,k]*B[k,j]            for i in i0..i0+5     ┐
                                              for j in j0..j0+5   │ point loops
                                                for k in k0..k0+5 ┘ (walk the tile)
                                                  C[i,j] += A[i,k]*B[k,j]
```

*build/matmul_tiled.mlir*
```mlir
#map = affine_map<(d0) -> (d0)>
#map1 = affine_map<(d0) -> (d0 + 5)>
module {
  func.func @matmul(%arg0: memref<10x10xf32, strided<[?, ?], offset: ?>>, %arg1: memref<10x10xf32, strided<[?, ?], offset: ?>>, %arg2: memref<10x10xf32, strided<[?, ?], offset: ?>>) -> memref<10x10xf32, strided<[?, ?], offset: ?>> {
    affine.for %arg3 = 0 to 10 step 5 {
      affine.for %arg4 = 0 to 10 step 5 {
        affine.for %arg5 = 0 to 10 step 5 {
          affine.for %arg6 = #map(%arg3) to #map1(%arg3) {
            affine.for %arg7 = #map(%arg4) to #map1(%arg4) {
              affine.for %arg8 = #map(%arg5) to #map1(%arg5) {
                %0 = affine.load %arg0[%arg6, %arg8] : memref<10x10xf32, strided<[?, ?], offset: ?>>
                %1 = affine.load %arg1[%arg8, %arg7] : memref<10x10xf32, strided<[?, ?], offset: ?>>
                %2 = affine.load %arg2[%arg6, %arg7] : memref<10x10xf32, strided<[?, ?], offset: ?>>
                %3 = arith.mulf %0, %1 : f32
                %4 = arith.addf %2, %3 : f32
                affine.store %4, %arg2[%arg6, %arg7] : memref<10x10xf32, strided<[?, ?], offset: ?>>
              }
            }
          }
        }
      }
    }
    %alloc = memref.alloc() : memref<10x10xf32>
    %cast = memref.cast %alloc : memref<10x10xf32> to memref<10x10xf32, strided<[?, ?], offset: ?>>
    memref.copy %arg2, %alloc : memref<10x10xf32, strided<[?, ?], offset: ?>> to memref<10x10xf32>
    return %cast : memref<10x10xf32, strided<[?, ?], offset: ?>>
  }
}
```

(The `strided<[?, ?], offset: ?>` types and the trailing `alloc`/`copy` are
bufferization's doing — it can't assume the returned buffer's layout, so it works
through a generic strided view and copies out a fresh result. The `?`s are the
runtime strides from Chapter 2's descriptor.)

Because Linalg keeps the op abstract until lowering, *the compiler* picks the tile
structure — and the same mechanism is what targets GPU shared memory in Chapter 8.

**Run:** `cd 5_tiling && bash build.sh` (then read `build/matmul_tiled.mlir`).

---

## Run everything

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"   # llvm@20 is keg-only
# Python deps for the drivers:  pip install numpy

for d in 1_elementwise 2_matmul 3_broadcast 4_fusion 5_tiling; do
  echo "=== $d ==="; ( cd "$d" && bash build.sh ) 2>&1 | tail -n 3
done
```

Each step writes its intermediates (`*_opt.mlir`, `*.ll`, `*.o`, `*.dylib`, and
the inspected `build/*.mlir`) into a local `build/` directory.

## Key takeaways

- **Describe the operation, not the loops.** A `linalg` op is *payload* (the
  scalar math) + *structure* (`indexing_maps` + `iterator_types`); the compiler
  generates and optimizes the loops from that structure.
- **Named ops are sugar for `linalg.generic`.** `linalg.matmul`, `linalg.add`,
  `linalg.broadcast` all lower through the same generic machinery — and to loops
  via `-convert-linalg-to-loops` / `-convert-linalg-to-affine-loops`, reproducing
  Chapter 3's affine nests exactly.
- **The three iterators** — `parallel`, `reduction`, `window` — are how an op
  declares which loops are independent, which accumulate, and which slide.
- **Tensors need bufferizing; memrefs don't.** In-place named ops on memrefs run
  straight away (Steps 1–3); ops that *return* tensors go through
  `--one-shot-bufferize` first (Step 5).
- **Fusion and tiling are why this level exists.** Keeping ops abstract lets the
  compiler fuse elementwise chains into one loop and tile contractions for cache —
  exactly the transforms that matter for ML and GPU code.

**Next:** Part 5 — Neural networks and tensors (see [`../reference/`](../reference/)).
```

