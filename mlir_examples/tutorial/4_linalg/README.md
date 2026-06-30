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

> Based on Stephen Diehl's *"MLIR Part 4 — Linear Algebra in MLIR"*
> ([`../reference/`](../reference/)). Every runnable example is built and checked
> against NumPy on this toolchain (Homebrew LLVM 20.1.8, Apple Silicon).

---

## What a structured op actually is

A `linalg` op is **payload + structure**. The *payload* is the scalar math (an
`addf`, a `mulf`); the *structure* is the iteration space and how each tensor is
indexed within it. The most general op, `linalg.generic`, spells the structure out
explicitly with two attributes:

- **`indexing_maps`** — one affine map per operand (inputs *and* outputs), saying
  how that tensor is indexed by the loop variables. For elementwise add, all three
  maps are `(d0) -> (d0)`: element `i` of each tensor maps to loop index `i`. For
  matmul they are `(i,j,k)->(i,k)`, `(i,j,k)->(k,j)`, `(i,j,k)->(i,j)` — exactly
  $A_{ik}$, $B_{kj}$, $C_{ij}$.
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
memref descriptors + NumPy adapters.

**Legend:** ✅ runnable · 🧩 snippet (transformed/inspected with `mlir-opt`).

---

## Step 1 — Elementwise: `generic`, `map`, named ops (`1_elementwise/`) · ✅

**Goal:** see the fundamental `linalg.generic`, then watch it collapse to a
one-liner.

`generic_add.mlir` (inspect) writes an elementwise tensor add the long way — three
identity `indexing_maps`, one `parallel` iterator, and a region that does the
`arith.addf`. It's verbose on purpose: every other Linalg op is this op with the
boilerplate removed. The same computation as a **named op** is a single line, and
because it writes into a memref `outs` buffer in place, no bufferization is needed:

```mlir
linalg.add ins(%a, %b : memref<10xf32>, memref<10xf32>) outs(%c : memref<10xf32>)
```

`build.sh` lowers the named version to a runnable library (and saves the
loop-lowered generic form to `build/` for comparison).

**Run:** `cd 1_elementwise && bash build.sh` →

```
linalg.add successful!
[10. 11. 12. 13. 14. 15. 16. 17. 18. 19.]
```

---

## Step 2 — `linalg.matmul` (`2_matmul/`) · ✅

**Goal:** a whole matrix multiply as one op — contrast with Chapter 3's
hand-written nest.

```mlir
linalg.matmul ins(%A, %B : memref<8x10xf32>, memref<10x16xf32>) outs(%C : memref<8x16xf32>)
```

That single line is sugar for the `linalg.generic` whose maps and iterators encode
$C_{ij} = \sum_k A_{ik}B_{kj}$:

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

`-convert-linalg-to-loops` turns either form into the same scf nest;
`build.sh` also emits the `-convert-linalg-to-affine-loops` version to `build/`, so
you can see it reproduce Chapter 3's affine matmul exactly. The driver checks the
result against NumPy's `@`.

**Run:** `cd 2_matmul && bash build.sh` →

```
linalg.matmul successful! (8x10 @ 10x16, max abs error 4.77e-07)
```

---

## Step 3 — Broadcasting (`3_broadcast/`) · ✅

**Goal:** NumPy-style broadcasting — combine tensors of different shapes.

Adding a length-4 vector to every row of a 3×4 matrix is `matrix + vector` in
NumPy. In Linalg it's `linalg.broadcast` (stretch the vector across the rows)
followed by an elementwise add:

```mlir
linalg.broadcast ins(%vector : memref<4xf32>) outs(%out : memref<3x4xf32>) dimensions = [0]
linalg.add ins(%matrix, %out : memref<3x4xf32>, memref<3x4xf32>) outs(%out : memref<3x4xf32>)
```

`dimensions = [0]` says the input lacks the leading dimension, so it's replicated
along it. The driver checks against NumPy's own broadcasting.

**Run:** `cd 3_broadcast && bash build.sh` →

```
[[2. 3. 4. 5.]
 [2. 3. 4. 5.]
 [2. 3. 4. 5.]]
```

---

## Step 4 — Kernel fusion (`4_fusion/`) · 🧩

**Goal:** the optimization that makes structured ops pay off.

`separate_ops.mlir` computes `(a + b) * c` as two named ops. Run separately,
that's two loops: the add writes a whole intermediate tensor to memory, the mul
reads it back. **Fusion** merges them into a single loop that reads each input
once and writes the result directly — no intermediate, no second pass:

```python
unfused:  t=[a[i]+b[i] for i]; r=[t[i]*c[i] for i]   # two loops + temp array
fused:    r=[(a[i]+b[i])*c[i] for i]                 # one loop, no temp
```

`build.sh` runs the fusion pipeline (`--linalg-fuse-elementwise-ops`, with
`--linalg-generalize-named-ops` and `--cse`) and writes `build/fused_ops.mlir`,
where the two ops have become **one `linalg.generic`** whose body does both the
`addf` and the `mulf` before yielding. This is the bread-and-butter optimization
for deep-learning graphs, which are mostly long chains of elementwise ops.

**Run:** `cd 4_fusion && bash build.sh` (then `diff separate_ops.mlir build/fused_ops.mlir`).

---

## Step 5 — Tiling (`5_tiling/`) · 🧩

**Goal:** restructure a matmul for cache locality.

On modern hardware, memory access — not arithmetic — is usually the bottleneck.
**Tiling** breaks a big computation into cache-sized blocks so data stays hot
while it's reused. Starting from a one-line `linalg.matmul` on tensors,
`build.sh` runs the full pipeline (generalize → bufferize → affine loops →
`--affine-loop-tile="tile-size=5"`) and writes `build/matmul_tiled.mlir`, where the
3-loop matmul has become a **6-deep nest**: outer loops stepping by 5 over tiles,
inner loops walking the 5 elements of each tile.

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

## Key takeaways

- **Describe the operation, not the loops.** A `linalg` op is *payload* (the
  scalar math) + *structure* (`indexing_maps` + `iterator_types`); the compiler
  generates and optimizes the loops from that structure.
- **Named ops are sugar for `linalg.generic`.** `linalg.matmul`, `linalg.add`,
  `linalg.broadcast` all lower through the same generic machinery — and to loops
  via `-convert-linalg-to-loops` / `-convert-linalg-to-affine-loops`.
- **The three iterators** — `parallel`, `reduction`, `window` — are how an op
  declares which loops are independent, which accumulate, and which slide.
- **Fusion and tiling are why this level exists.** Keeping ops abstract lets the
  compiler fuse elementwise chains into one loop and tile contractions for cache —
  exactly the transforms that matter for ML and GPU code.

**Next:** Part 5 — Neural networks and tensors (see [`../reference/`](../reference/)).
