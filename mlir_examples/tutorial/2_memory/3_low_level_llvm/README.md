# 3 — Low-Level LLVM Structures in MLIR

This tutorial drops below the comfortable `memref` / `tensor` abstractions
and looks at the constructs MLIR exposes when you want C-level control:
LLVM-dialect **structs** and **arrays**, the **vector** dialect for SIMD,
**bufferization** (tensor → memref conversion), and the
`llvm.emit_c_interface` attribute that makes a kernel callable from C
or Python.

The directory is split into two halves:

| Kind                     | Files                                                                                                |
| ------------------------ | ---------------------------------------------------------------------------------------------------- |
| Concept-only MLIR snippets | `structs_arrays.mlir`, `vectors.mlir`, `bufferization.mlir`, `c_interface.mlir`                      |
| Runnable end-to-end demo | `array_add_vec.mlir` + `compile.sh` + `aot_main.py` + `np_memref.py`                                 |

The concept files are designed to be opened, read, and lowered with
`mlir-opt` so you can see how the IR changes. Only `array_add_vec.mlir`
is compiled all the way to a `.dylib` and called from Python.

---

## 1. LLVM structs and arrays — `structs_arrays.mlir`

C's `struct Pair { int a; float b; }` becomes
`!llvm.struct<(i32, f32)>`. The LLVM dialect is SSA, so you don't mutate
a struct in place; you build a new value with `llvm.insertvalue` and
read fields with `llvm.extractvalue`.

```mlir
%z  = llvm.mlir.zero : !llvm.struct<(i32, f32)>
%a  = llvm.mlir.constant(42 : i32) : i32
%s0 = llvm.insertvalue %a, %z[0] : !llvm.struct<(i32, f32)>
```

`!llvm.array<N x T>` is the fixed-size, no-metadata array — the rigid
sibling of `memref`. Nested index paths address fields inside structs
inside arrays (`llvm.insertvalue %v, %arr[0, 0]`).

Inspect the lowered IR:

```bash
mlir-opt structs_arrays.mlir -reconcile-unrealized-casts
```

## 2. Vectors — `vectors.mlir`

`vector<4xf32>` is MLIR's SIMD type. Elementwise math uses the regular
`arith.*` ops with a vector operand type; there are also lane-level ops
in the `vector` dialect:

| Op                     | What it does                              |
| ---------------------- | ----------------------------------------- |
| `arith.addf`/`mulf`    | Elementwise add / multiply                |
| `vector.extract`       | Pull one lane out as a scalar             |
| `vector.shuffle`       | Permute lanes from two vectors            |
| `vector.reduction`     | Horizontal reduce a vector to a scalar    |
| `vector.broadcast`     | Splat a scalar across all lanes           |

The same `vector<8xf32>` op may lower to one AVX instruction on x86,
two NEON instructions on ARM, or a scalar loop on hardware without
SIMD — LLVM handles that translation.

Lower to LLVM IR:

```bash
mlir-opt vectors.mlir \
  -convert-vector-to-llvm \
  -convert-arith-to-llvm \
  -convert-func-to-llvm \
  -reconcile-unrealized-casts
```

## 3. Bufferization — `bufferization.mlir`

Bufferization rewrites value-semantic `tensor` ops into pointer-semantic
`memref` ops. The file shows three flavors:

1. **Manual** primitives — `bufferization.to_memref` and
   `bufferization.to_tensor` (with `restrict` so the analyzer accepts
   it).
2. **Automatic** via the `-one-shot-bufferize` pass. With
   `bufferize-function-boundaries`, function arguments and returns are
   also converted, so `tensor<8xf32>` in the signature becomes
   `memref<8xf32>`.
3. **In-place destination** with
   `bufferization.materialize_in_destination`, the typical final step
   of a kernel that writes into a buffer the caller already owns.

Try both forms:

```bash
mlir-opt bufferization.mlir -canonicalize
mlir-opt bufferization.mlir -one-shot-bufferize="bufferize-function-boundaries"
```

In the one-shot output, watch `tensor.insert` become `memref.store`
acting on the original argument — no copy, no allocation.

## 4. C interface — `c_interface.mlir`

Without `llvm.emit_c_interface`, lowering a function that takes
`memref<3xf32>` unrolls the descriptor into five scalar arguments
(`allocated_ptr, aligned_ptr, offset, sizes[], strides[]`). That's
unpleasant to call from C.

Adding the attribute keeps the unrolled function *and* emits an extra
wrapper named `_mlir_ciface_<name>` that takes pointers to descriptor
structs instead. That wrapper is what `ctypes` calls in `aot_main.py`.

Lower and look at the generated wrapper:

```bash
mlir-opt c_interface.mlir \
  -finalize-memref-to-llvm \
  -convert-func-to-llvm \
  -reconcile-unrealized-casts
```

You should see both `@identity` and `@_mlir_ciface_identity` in the
output.

---

## 5. Runnable example — vectorized `array_add`

`array_add_vec.mlir` is the same kernel as in `../2_array_add_numpy`,
but the loop processes 8 floats at a time using `vector.load` /
`vector.store` and a `vector<8xf32>` add:

```mlir
scf.for %i = %c0 to %c1024 step %c8 {
  %va = vector.load  %arg0[%i] : memref<1024xf32>, vector<8xf32>
  %vb = vector.load  %arg1[%i] : memref<1024xf32>, vector<8xf32>
  %vc = arith.addf   %va, %vb  : vector<8xf32>
  vector.store %vc, %arg2[%i]  : memref<1024xf32>, vector<8xf32>
}
```

`attributes {llvm.emit_c_interface}` ensures the `.dylib` exports
`_mlir_ciface_array_add`, which `aot_main.py` looks up via `ctypes`.

### Build and run

```bash
bash compile.sh
```

The script chains four tools:

1. `mlir-opt` — lowers SCF → CF, vector / arith / memref / func / index
   → LLVM dialect.
2. `mlir-translate --mlir-to-llvmir` — MLIR LLVM dialect → textual
   LLVM IR.
3. `llc` — LLVM IR → relocatable object (`.o`).
4. `clang -shared` — `.o` → `libarray_add_vec.dylib`.
5. `python3 aot_main.py` — loads the dylib, builds three
   `MemRefDescriptor` structs around NumPy arrays, calls
   `_mlir_ciface_array_add`, verifies the result.

### Expected output

```
Vectorized array_add successful!
First 8 elements: [3. 3. 3. 3. 3. 3. 3. 3.]
```

(`a = ones(1024)`, `b = 2 * ones(1024)`, so `c = a + b = 3` everywhere.)

### Cleanup

```bash
bash clean.sh
```

Removes the generated `*_opt.mlir`, `*.ll`, `*.o`, `*.dylib`, and the
Python `__pycache__`.

---

## File map

```
3_low_level_llvm/
├── README.md             # this file
├── structs_arrays.mlir   # !llvm.struct, !llvm.array, insert/extractvalue
├── vectors.mlir          # vector<...> ops (add, shuffle, reduce, …)
├── bufferization.mlir    # tensor → memref via manual + one-shot
├── c_interface.mlir      # llvm.emit_c_interface wrapper demo
├── array_add_vec.mlir    # runnable SIMD array_add kernel
├── compile.sh            # build + run the runnable example
├── clean.sh              # remove build artifacts
├── np_memref.py          # ctypes MemRefDescriptor + numpy adapter
└── aot_main.py           # Python driver: dlopen + ctypes call
```
