# 5. C-compatible wrappers

When we want to call a function from C, we need to emit a C-compatible wrapper. This is done using the
`llvm.emit_c_interface` attribute. Normally when we bufferize a function we unroll all the memref fields
into a single arguments. This is not desirable when we want to call the function from C where we would typically
pass the memrefs as void pointers to structs


```mlir
module {
  func.func @add_vector_to_matrix(%A: memref<3xf32>)
    -> memref<3xf32> attributes {llvm.emit_c_interface} {
    return %A : memref<3xf32>
  }
}
```

This will emit a C-compatible wrapper for the function which is expanded into two functions.

Instead now we call the `@_mlir_ciface_add_vector_to_matrix` function which is the C-compatible wrapper for the `@add_vector_to_matrix` function. Notice how it's given two arguments: `%arg0` is the **output** (the caller-allocated descriptor that the wrapper writes the returned memref into) and `%arg1` is the **input** descriptor pointer. The wrapper loads the input struct, calls the unrolled inner function with its five scalar fields, and stores the returned struct back through `%arg0`. From the caller's side we just hand it two pointers to `MemRefDescriptor` structs and let the wrapper do the marshalling.

```mlir
module {
  llvm.func @add_vector_to_matrix(%arg0: !llvm.ptr, %arg1: !llvm.ptr, %arg2: i64, %arg3: i64, %arg4: i64) -> !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)> attributes {llvm.emit_c_interface} {
    %0 = llvm.mlir.undef : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %1 = llvm.insertvalue %arg0, %0[0] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %2 = llvm.insertvalue %arg1, %1[1] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %3 = llvm.insertvalue %arg2, %2[2] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %4 = llvm.insertvalue %arg3, %3[3, 0] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %5 = llvm.insertvalue %arg4, %4[4, 0] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    llvm.return %5 : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
  }
  llvm.func @_mlir_ciface_add_vector_to_matrix(%arg0: !llvm.ptr, %arg1: !llvm.ptr) attributes {llvm.emit_c_interface} {
    %0 = llvm.load %arg1 : !llvm.ptr -> !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %1 = llvm.extractvalue %0[0] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %2 = llvm.extractvalue %0[1] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %3 = llvm.extractvalue %0[2] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %4 = llvm.extractvalue %0[3, 0] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %5 = llvm.extractvalue %0[4, 0] : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    %6 = llvm.call @add_vector_to_matrix(%1, %2, %3, %4, %5) : (!llvm.ptr, !llvm.ptr, i64, i64, i64) -> !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>
    llvm.store %6, %arg0 : !llvm.struct<(ptr, ptr, i64, array<1 x i64>, array<1 x i64>)>, !llvm.ptr
    llvm.return
  }
}
```

## The MemRefDescriptor on the Python side

The struct that the wrapper expects has to be mirrored on the caller's
side. `np_memref.py` defines it with `ctypes` so that it matches the
LLVM struct produced by `-finalize-memref-to-llvm` field-for-field. For
a 1-D `memref<Nxf32>` that's:

```python
class MemRefDescriptor(Structure):
  _fields_ = [
    ("allocated", c_void_p),    # base pointer (the one you'd free())
    ("aligned",   c_void_p),    # aligned data pointer (used for access)
    ("offset",    c_longlong),  # offset into data, in elements (not bytes)
    ("shape",     c_longlong * 1),
    ("stride",    c_longlong * 1),
  ]
```

A few details worth knowing:

- **`allocated` vs `aligned`.** MLIR distinguishes the pointer the
  runtime would free from the pointer used for accesses, because
  `aligned_alloc` may return a different address than the one used for
  bookkeeping. NumPy hands us a single buffer pointer with no separate
  free-handle, so we set both fields to the same value. That's safe
  here because nothing in this flow ever calls `free()` on the
  descriptor.
- **`offset` and `stride` are in elements, not bytes.** A contiguous
  1-D float array has `stride[0] == 1`, not `4`.
- **The struct's shape depends on the memref rank.** A 2-D memref has
  `shape[2]` and `stride[2]`. If the MLIR signature changes rank, the
  ctypes struct has to change with it — otherwise the wrapper will
  load garbage past the end of our struct.

## Reading the result back

Because the kernel returns a memref, the wrapper writes the returned
descriptor into the result pointer we hand it. The output we care
about isn't whatever buffer we might have prepared — it's the
descriptor the kernel returns, which may point at *some other buffer
entirely*. To read the data we go through `result_desc.aligned`:

```python
result_desc = MemRefDescriptor()           # uninitialized; filled by the call
fn(ctypes.byref(result_desc), ctypes.byref(a_desc))

size    = result_desc.shape[0]
out_ptr = ctypes.cast(result_desc.aligned, ctypes.POINTER(ctypes.c_float))
out     = np.ctypeslib.as_array(out_ptr, shape=(size,)).copy()
```

For the current identity kernel, `result_desc.aligned` ends up equal
to `a_desc.aligned` — the kernel literally returned its input.

A kernel that writes into a caller-provided buffer would have a
different shape: no return value, and the output memref passed as an
ordinary argument. Then we'd read the data from the NumPy array we
allocated ourselves, not from the returned descriptor.

## The build pipeline (`build.sh`)

```bash
mlir-opt add_vector_to_matrix.mlir \
  --convert-vector-to-llvm --convert-scf-to-cf --convert-cf-to-llvm \
  --convert-arith-to-llvm --convert-func-to-llvm --convert-index-to-llvm \
  --finalize-memref-to-llvm --reconcile-unrealized-casts \
  -o build/add_vector_to_matrix_opt.mlir

mlir-translate build/add_vector_to_matrix_opt.mlir -mlir-to-llvmir \
  -o build/add_vector_to_matrix_opt.ll

llc -filetype=obj --relocation-model=pic \
  build/add_vector_to_matrix_opt.ll -o build/add_vector_to_matrix_opt.o

clang -shared -fPIC build/add_vector_to_matrix_opt.o \
  -o build/add_vector_to_matrix_opt.so
```

Two things in this pipeline that aren't obvious:

- `--finalize-memref-to-llvm` is what unrolls the memref descriptor
  into its five scalar fields. It has to run before
  `--reconcile-unrealized-casts`, because the unrolling produces
  `unrealized_conversion_cast` ops that the latter pass then erases.
- `--relocation-model=pic` and `-fPIC` are required because the
  resulting `.so` is loaded by `dlopen` (via `ctypes.CDLL`); without
  them the linker will reject the object as non-PIC.

On macOS, `nm -gU build/add_vector_to_matrix_opt.so | grep mlir` will
show the wrapper symbol with an extra leading underscore
(`__mlir_ciface_add_vector_to_matrix`) — that's just the Mach-O ABI's
convention and doesn't affect how `ctypes` looks the symbol up.

## Files in this directory

```
5_C_compatiable_wrappers/
├── README.md                  # this file
├── add_vector_to_matrix.mlir  # MLIR kernel (currently an identity fn on memref<3xf32>)
├── build.sh                   # mlir-opt → mlir-translate → llc → clang → python
├── np_memref.py               # ctypes MemRefDescriptor + numpy adapter
└── aot_main.py                # Python driver: dlopen + ctypes call
```

Build and run with `bash build.sh`. Expected output:

```
add_vector_to_matrix call successful!
input : [1. 2. 3.]
output: [1. 2. 3.]
```
