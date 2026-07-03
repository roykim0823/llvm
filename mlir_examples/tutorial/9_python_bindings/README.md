# 9 — Driving MLIR from Python: a mini JIT compiler

### From shell scripts to a compiler-as-a-library

Chapter 8 lowered a parallel loop to GPU code by chaining `mlir-opt`
invocations in a shell script. That's perfect for *learning* — every stage is
a file you can read — but it isn't how a real system works. The goal stated at
the start of the GPU story was to specify tensor operations **dynamically, at
runtime**, and compile them on the spot. A PyTorch-style framework can't shell
out to `mlir-opt` sixteen times per graph; it needs the compiler *as a
library*.

That library exists: the **MLIR Python bindings** — the official upstream
Python API (`mlir.ir`, `mlir.passmanager`) to the same C++ infrastructure that
`mlir-opt` wraps. Same passes, same registry, same IR; a different driver.
This chapter rebuilds Chapter 8's pipeline as ~40 lines of Python that go
**linalg-on-tensors → PTX assembly**, in-process.

> **What runs on this machine:** everything except the final *GPU* execution.
> PTX is *text* — emitting it is ordinary compilation, and Homebrew `llvm@20`'s
> `llc` ships the NVPTX backend. So unlike Chapter 8, this chapter ends with a
> real `.ptx` file in `build/`, generated on the Mac. And the program itself
> *does* execute here: [`run_macos.py`](run_macos.py) JIT-compiles the same
> `square.mlir` for the **CPU** with `mlir.execution_engine` and runs it
> in-process against NumPy. Only `run.py` (which feeds the PTX to the CUDA
> driver) needs NVIDIA hardware.
>
> Based on the *"MLIR Python Bindings"* and *"MLIR Pipeline"* sections of
> Stephen Diehl's *"MLIR Part 8"* ([`../reference/`](../reference/)).

---

## Installing the bindings

The bindings ship inside the LLVM monorepo and Homebrew doesn't build them, so
the practical route is the community wheels (see also the
[top-level README](../README.md#installing-mlir-python-bindings)). Two caveats,
both verified on this machine, shape how to install them here:

- the wheels publish **LLVM trunk only** (one rolling release — currently
  22.x; there is no 20.x to pin), and
- `mlir-python-bindings` requires `numpy<=2.1.2`, which conflicts with this
  repo's `numpy>=2.4.4` — so don't `uv add` it into the project.

The clean way is an ephemeral environment, installing the bindings *plus* the
full `mlir` wheel (~1.1 GiB — it bundles version-matched `mlir-translate`
etc.; see the [appendix](#appendix-version-skew-notes) for why that matters)
and NumPy (used by `run_macos.py`):

```bash
uv run --no-project \
  --with mlir-python-bindings --with mlir --with numpy \
  --find-links https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest \
  python3 compile.py square.mlir --chip sm_80
```

(or make a throwaway venv and `pip install mlir-python-bindings mlir numpy -f <same URL>`.)

> Don't worry about mixing trunk bindings with Homebrew's LLVM 20 CLI tools —
> `compile.py` automatically picks version-matched binaries where it matters
> (the gory details live in the [appendix](#appendix-version-skew-notes)). And
> [`build.sh`](build.sh) reproduces the identical pipeline with `mlir-opt`
> alone, so the chapter is fully runnable even without the bindings.

## The API in one breath

Four objects cover almost everything this chapter needs:

| Python | What it is | CLI equivalent |
| --- | --- | --- |
| `mlir.ir.Context` | owns dialects, types, attributes; everything happens inside one | (implicit) |
| `mlir.ir.Module.parse(str)` | text → in-memory IR | reading the input file |
| `mlir.passmanager.PassManager` | the pass driver; `pm.add("pass-name")` uses **the same names as `mlir-opt` flags** | `mlir-opt --pass-name` |
| `module.body.operations` | the operation tree (module → region → block → ops), walkable and printable | reading the output file |

```python
from mlir.ir import Context, Module
from mlir.passmanager import PassManager

with Context():
    module = Module.parse(open("square.mlir").read())
    pm = PassManager()
    pm.enable_ir_printing(print_after_change=True)   # = --mlir-print-ir-after-change
    pm.add("canonicalize")
    pm.add("convert-linalg-to-parallel-loops")
    # ... the rest of the pipeline ...
    pm.run(module.operation)
    print(module)
```

That's the whole mental model: what was a shell pipeline of processes becomes
a list of `pm.add(...)` calls over one in-memory module — no serialization
between stages, and callable from inside a running program.

---

## The input: back to pure math

[`square.mlir`](square.mlir) is the same elementwise square as Chapter 8 —
but expressed one level *higher*, as a named `linalg` op on `tensor`s. No
loops, no memrefs, no index math; this is the "close to the domain" form the
pipeline starts from:

```mlir
func.func @square(%input: tensor<10x10xf32>, %output: tensor<10x10xf32>) -> tensor<10x10xf32> {
  %0 = linalg.square ins(%input : tensor<10x10xf32>)
                     outs(%output : tensor<10x10xf32>) -> tensor<10x10xf32>
  return %0 : tensor<10x10xf32>
}
```

## The pipeline (`compile.py`)

[`compile.py`](compile.py) is the whole compiler. Its pass list *is* the
four-stage roadmap from Chapter 8's "Connecting to MLIR", made concrete —
each entry below is one `pm.add(...)` call (equivalently: one `mlir-opt` flag):

| Stage | Pass | Effect |
| --- | --- | --- |
| bufferize | `one-shot-bufferize{bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map}` | tensor ops → buffer (memref) ops in a single pass, converting function boundaries with an identity layout map (Ch 2) |
| to loops | `convert-linalg-to-parallel-loops` | `linalg.square` → an `scf.parallel` nest that explicitly iterates over the elements (Ch 3/4) |
| GPU mapping | `func.func(gpu-map-parallel-loops)` | annotate which loop dims map to blocks/threads |
| | `convert-parallel-loops-to-gpu` | `scf.parallel` → a `gpu.launch` region, distributing iterations across the grid |
| | `gpu-kernel-outlining` | extract the launch region into a separate `gpu.func` that can be launched from host code |
| cleanup | `lower-affine` | affine ops → standard control flow and arithmetic |
| | `expand-strided-metadata` | strided memory-access metadata → explicit calculations |
| | `normalize-memrefs` | memrefs → the form the GPU backends expect |
| to NVVM | `gpu.module(convert-gpu-to-nvvm{index-bitwidth=0 use-bare-ptr-memref-call-conv})` | device kernel → NVVM dialect, passing memrefs as bare pointers |
| target | `nvvm-attach-target{chip=sm_80 features=+ptx80 O=3}` | attach the target: GPU architecture, PTX 8.0 features, optimization level 3 |
| host side | `convert-nvvm-to-llvm`, `reconcile-unrealized-casts` | NVVM → LLVM dialect; resolve leftover cross-dialect type casts |
| | `gpu-to-llvm{use-bare-pointers-for-host use-bare-pointers-for-kernels}` | remaining GPU ops → LLVM dialect, bare pointers on both sides |

Two details worth noticing:

- **Nesting syntax.** `func.func(gpu-map-parallel-loops)` runs a pass *on each
  function* rather than on the whole module — the string form of pass-manager
  nesting. Same for `gpu.module(convert-gpu-to-nvvm{...})`, which runs only on
  device code. This is exactly the `--pass-pipeline` nesting syntax from the
  [top-level README](../README.md#key-mlir-concepts).
- **A divergence from the reference.** The PDF routes through
  `convert-affine-for-to-gpu`. On LLVM 20 that path trips the verifier mid-pipeline
  (`affine.load` indexed by thread IDs inside `gpu.launch`, which isn't an
  affine scope in this version), so we take the `scf.parallel` route instead —
  the same one Chapter 8 used. A nice reminder that **pass pipelines are
  version-pinned artifacts**; MLIR guarantees no stability between majors.

## Watching the IR transform

`build.sh` runs the same pipeline in stages so every intermediate lands in
`build/` (or pass `-v` to `compile.py` to watch the PassManager print each
change). This is the four-stage roadmap happening for real.

**After bufferization + `convert-linalg-to-parallel-loops`** — the tensors are
gone (memref arguments now), and `linalg.square`'s implicit iteration has
become an explicit doubly-parallel loop. Note this is *exactly* the kind of op
you handed to OpenMP in Chapter 3:

*build/square_loops.mlir*
```mlir
func.func @square(%arg0: memref<10x10xf32>, %arg1: memref<10x10xf32>) -> memref<10x10xf32> {
  %c0 = arith.constant 0 : index
  %c10 = arith.constant 10 : index
  %c1 = arith.constant 1 : index
  scf.parallel (%arg2, %arg3) = (%c0, %c0) to (%c10, %c10) step (%c1, %c1) {
    %0 = memref.load %arg0[%arg2, %arg3] : memref<10x10xf32>
    %1 = arith.mulf %0, %0 : f32
    memref.store %1, %arg1[%arg2, %arg3] : memref<10x10xf32>
    scf.reduce
  }
  return %arg1 : memref<10x10xf32>
}
```

**After `gpu-map-parallel-loops` + `convert-parallel-loops-to-gpu`** — the loop
is gone; its body now lives *inline* inside a `gpu.launch` region (the first of
Chapter 8's "two ways to launch"), with the iteration space mapped onto a
10×10 grid of blocks and the loop indices rebuilt from block IDs via
`affine.apply`:

*build/square_gpu_launch.mlir* (trimmed)
```mlir
#map1 = affine_map<(d0)[s0, s1] -> (d0 * s0 + s1)>
...
gpu.launch blocks(%arg2, %arg3, %arg4) in (%arg8 = %0, %arg9 = %1, %arg10 = %c1_0)
           threads(%arg5, %arg6, %arg7) in (%arg11 = %c1_0, %arg12 = %c1_0, %arg13 = %c1_0) {
  %2 = affine.apply #map1(%arg2)[%c1, %c0]     // row  <- blockIdx.x
  %3 = affine.apply #map1(%arg3)[%c1, %c0]     // col  <- blockIdx.y
  %4 = memref.load %arg0[%2, %3] : memref<10x10xf32>
  %5 = arith.mulf %4, %4 : f32
  memref.store %5, %arg1[%2, %3] : memref<10x10xf32>
  gpu.terminator
}
```

**After `gpu-kernel-outlining`** — the inline region becomes the *second*
launch form: a `gpu.func ... kernel` inside a `gpu.module`, referencing the
grid through `gpu.block_id`/`gpu.thread_id` coordinate ops, launched from the
host by symbol. This is a clean host/device split — the module even gets the
`gpu.container_module` attribute:

*build/square_outlined.mlir* (trimmed)
```mlir
module attributes {gpu.container_module} {
  func.func @square(%arg0: memref<10x10xf32>, %arg1: memref<10x10xf32>) -> memref<10x10xf32> {
    ...
    gpu.launch_func @square_kernel::@square_kernel
        blocks in (%0, %1, %c1_0) threads in (%c1_0, %c1_0, %c1_0)
        args(%c1 : index, %c0 : index, %arg0 : memref<10x10xf32>, %arg1 : memref<10x10xf32>)
    return %arg1 : memref<10x10xf32>
  }
  gpu.module @square_kernel {
    gpu.func @square_kernel(%arg0: index, %arg1: index, %arg2: memref<10x10xf32>,
                            %arg3: memref<10x10xf32>) kernel {
      %block_id_x = gpu.block_id  x
      %block_id_y = gpu.block_id  y
      ...
      %0 = affine.apply #map1(%block_id_x)[%arg0, %arg1]
      %1 = affine.apply #map1(%block_id_y)[%arg0, %arg1]
      %2 = memref.load %arg2[%0, %1] : memref<10x10xf32>
      %3 = arith.mulf %2, %2 : f32
      memref.store %3, %arg3[%0, %1] : memref<10x10xf32>
      gpu.return
    }
  }
}
```

**After the remaining passes** (`pm.run` complete) — the module is *both
halves* of a CUDA program in their final MLIR form: a host-side
`llvm.func @square` that computes grid dimensions and calls `gpu.launch_func`,
plus the device kernel lowered to NVVM — Chapter 8's endpoint, arrived at
without ever leaving Python:

*build/square_lowered.mlir* (the kernel, trimmed)
```mlir
gpu.module @square_kernel [#nvvm.target<O = 3, chip = "sm_80", features = "+ptx80">] {
  llvm.func @square_kernel(%arg0: i64, %arg1: i64, %arg2: !llvm.ptr, %arg3: !llvm.ptr)
      attributes {gpu.kernel, nvvm.kernel, ...} {
    %1 = nvvm.read.ptx.sreg.ctaid.x : i32      // blockIdx.x
    %3 = nvvm.read.ptx.sreg.ctaid.y : i32      // blockIdx.y
    ...
    %12 = llvm.load %11 : !llvm.ptr -> f32
    %13 = llvm.fmul %12, %12 : f32             // the actual square
    llvm.store %13, %16 : f32, !llvm.ptr
    llvm.return
  }
}
```

## Extracting the kernel: walking the operation tree

The host half stays in MLIR (a real system would JIT it with
`ExecutionEngine`); only the *device* half continues to PTX. This is where the
bindings earn their keep — the IR is a data structure, not text, so finding
the `gpu.module` is a loop, not `awk`:

```python
def extract_gpu_module(module):
    for op in module.body.operations:            # module → region → block → ops
        if op.operation.name == "gpu.module":
            block = op.operation.regions[0].blocks[0]
            body = "\n".join(str(k) for k in block.operations)
            return Module.parse(f"module {{\n{body}\n}}")
    raise RuntimeError("no gpu.module found")
```

(Every operation exposes `regions → blocks → operations` all the way down —
the same nesting you've been reading in textual IR since Chapter 1.)

## Emitting PTX — on this Mac

The last step shells out twice, exactly like Chapter 1's pipeline but with the
NVPTX backend instead of the native one:

```
gpu.module (llvm dialect) ──mlir-translate --mlir-to-llvmir──► LLVM IR ──llc -march=nvptx64──► PTX
```

First `mlir-translate` turns the LLVM-dialect kernel into genuine LLVM IR —
notice the `ptx_kernel` calling convention and that the NVVM ops are now
`@llvm.nvvm.*` intrinsic calls (the middle column of Chapter 8's
CUDA↔NVVM↔gpu table):

*build/kernel.ll* (trimmed)
```llvm
define ptx_kernel void @square_kernel(i64 %0, i64 %1, ptr %2, ptr %3) {
  %5 = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()   ; blockIdx.x
  %7 = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.y()   ; blockIdx.y
  ...
  %15 = getelementptr float, ptr %2, i64 %14
  %16 = load float, ptr %15, align 4
  %17 = fmul float %16, %16
  store float %17, ptr %20, align 4
  ret void
}
```

Then `llc` runs the NVPTX back-end. No GPU is involved — PTX is
architecture-neutral *text* (Chapter 8's table), and it comes out recognizably
the same program as `square.cu`'s hand-written kernel:

*build/kernel.ptx* (trimmed)
```ptx
.visible .entry square_kernel(
    .param .u64 square_kernel_param_0,
    .param .u64 square_kernel_param_1,
    .param .u64 .ptr .align 1 square_kernel_param_2,
    .param .u64 .ptr .align 1 square_kernel_param_3
)
{
    mov.u32     %r1, %ctaid.x;          // blockIdx.x
    mov.u32     %r2, %ctaid.y;          // blockIdx.y
    mad.lo.s64  %rd11, %rd9, 10, %rd10; // row * 10 + col
    ld.global.f32  %f1, [%rd13];
    mul.rn.f32     %f2, %f1, %f1;       // x * x
    st.global.f32  [%rd14], %f2;
    ret;
}
```

**Run:** `cd 9_python_bindings && bash build.sh` — it runs the pipeline in
stages, saving every intermediate above
(`build/square_{loops,gpu_launch,outlined,lowered,kernel}.mlir`, `kernel.ll`,
`kernel.ptx`), and *also* runs `compile.py` if the bindings are installed
(`python3 compile.py square.mlir --chip sm_80` does the whole thing in-process).

> **Aside — embedding the binary instead.** The alternative to extracting PTX
> yourself is to keep everything inside MLIR: the `gpu-module-to-binary` pass
> (Chapter 8's `run_on_gpu.sh` uses it) serializes each `gpu.module` according
> to its attached `#nvvm.target` and replaces it with a `gpu.binary` op — a
> compiled blob (PTX, CUBIN, or a fatbin) embedded right in the IR, one object
> per target architecture:
>
> ```mlir
> gpu.binary @square_kernel [#gpu.object<#nvvm.target<chip = "sm_80">, offload = "BC\C0\DE5\14...">]
> ```
>
> The host-side `gpu.launch_func` then loads and launches it at runtime. That
> path needs an MLIR built with the NVPTX serializer (and ultimately a CUDA
> toolchain), which is why this chapter extracts the kernel and emits PTX with
> stock tools instead.

## Running it — two targets

Like Chapter 8, one high-level program forks to two execution stories — except
this time both halves are Python, and one of them **runs on this Mac**:

```
                  ┌─ compile.py ──► kernel.ptx ──► run.py (CUDA driver API) ──► NVIDIA GPU (Linux)
square.mlir ──────┤
                  └─ run_macos.py ──► mlir.execution_engine (CPU JIT) ──► result, right here
```

### Target 1 — Apple Silicon CPU, in-process JIT (runs on this Mac)

[`run_macos.py`](run_macos.py) reuses everything this chapter built, swapping
only the back half of the pass list: the same bufferization front end, then
`convert-linalg-to-loops` and the Chapter 1/2 CPU lowering instead of the GPU
branch. Then — the part no CLI tool offers — it executes the module *in the
same Python process* with `mlir.execution_engine.ExecutionEngine`, MLIR's
JIT (the library behind `mlir-runner`):

```python
square_func.attributes["llvm.emit_c_interface"] = UnitAttr.get()  # C-callable wrapper
pm.run(module.operation)                                          # CPU pipeline
engine = ExecutionEngine(module, opt_level=2)
engine.invoke("square", result_ptr, input_ptr, output_ptr)        # NumPy in, NumPy out
```

NumPy arrays cross the boundary as ranked memref descriptors
(`mlir.runtime.get_ranked_memref_descriptor`), and since `@square` *returns* a
memref, the C interface passes the result slot as the first argument.

```
$ python3 run_macos.py
Success! JIT-compiled MLIR ran on this machine's CPU:
  input[3,3]=33  ->  output[3,3]=1089
```

That's the complete "compiler as a library" loop closed on local hardware:
parse → transform → JIT → call it like a Python function — the same mechanism
a real ML framework uses to run a compiled graph.

### Target 2 — NVIDIA GPU via PTX (`run.py`, reference-only)

[`run.py`](run.py) is the runtime half on a CUDA box: load the PTX with the
CUDA **driver API** (`cuModuleLoadData`), grab the kernel
(`cuModuleGetFunction`), launch it over a 10×10 grid (`cuLaunchKernel`), and
verify against NumPy. Note what it *isn't*: there is no MLIR in that file.
Once the PTX exists, MLIR's job is done — the runtime side is plain CUDA
plumbing (the `cuda-python` package), which is why this series treats it as an
appendix rather than a chapter of its own.

---

## Appendix: version-skew notes

Everything below was hit for real while building this chapter; it's collected
here so the install section stays short.

The wheels track LLVM *trunk* while the CLI tools are Homebrew `llvm@20`, and
MLIR guarantees no IR stability across majors. The concrete failure: the trunk
bindings print `llvm.getelementptr inbounds|nuw ...`, syntax that v20's
`mlir-translate` rejects (`expected SSA operand`). That's why `compile.py`'s
tool lookup prefers the binaries bundled inside the `mlir` wheel — same commit
as the bindings, so they always parse what the bindings print. The wheel's
`llc` is built *without* the NVPTX backend, though, so for that one tool the
lookup probes `--version` for `nvptx` and falls back to Homebrew's. Explicit
`$MLIR_TRANSLATE` / `$LLC` environment overrides beat everything.

With that arrangement the in-process PTX matches `build.sh`'s CLI-generated
PTX except for one lost `.maxntid` hint (trunk emits it in a form v20's `llc`
doesn't read) — a pleasingly concrete illustration of the same lesson as the
pipeline divergence above: **pin your MLIR version, and treat pass lists and
tool pairings as part of the lockfile.**

---

## Key takeaways

- **The Python bindings are MLIR, not a wrapper around the CLI** — `mlir.ir` and
  `mlir.passmanager` expose the same contexts, modules, and pass registry that
  `mlir-opt` uses; `pm.add("pass-name")` takes exactly the `mlir-opt` flag names.
- **This is how "compile at runtime" actually works.** A framework parses or
  builds IR in-process, runs one `PassManager`, and hands the result to a
  backend — no shell, no temp files. This chapter is that pattern at minimum
  viable size: `linalg` in, PTX out.
- **IR as a data structure.** `module → regions → blocks → operations` is
  walkable from Python, which turns "extract the GPU module" from text-mangling
  into a three-line loop.
- **Pipelines are version-pinned.** The reference's affine route fails LLVM 20's
  verifier; the `scf.parallel` route works on both. Treat a pass list like a
  lockfile.
- **PTX generation needs no GPU** — only *executing PTX* does. And the program
  itself still runs here: point the same module down the CPU pipeline and
  `mlir.execution_engine` JITs and executes it in-process (`run_macos.py`) —
  NumPy in, NumPy out, no CLI tools at all.

**The actual end.** Chapter 1 lowered `return 42` by hand; this chapter is a
program that *performs* every one of those steps on demand, from tensor math
to GPU assembly. That program — grown a frontend and a runtime — is an ML
compiler. See the [top-level README](../README.md) for the whole map.
