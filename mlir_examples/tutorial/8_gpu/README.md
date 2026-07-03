# 8 — GPU Compilation with MLIR

### The endgame: from a parallel loop to GPU threads

Everything has led here. Chapter 7 left us with a transformer whose hot ops are
matmul and softmax; Chapter 4 showed how to tile them; Chapter 3 showed how to
mark independent iterations parallel. This chapter takes that last step —
**mapping parallel work onto a GPU's thousands of cores** — which is what makes
modern deep learning practical.

The beautiful part is how little new you need. A GPU kernel is, conceptually,
*one iteration of a parallel loop, run on one thread*. You already wrote
`scf.parallel` in Chapter 3 and handed it to OpenMP for CPU threads; here you hand
the *same* loop to a different set of lowering passes and it becomes a GPU kernel.
One high-level program, two backends.

> **⚠️ The GPU half of this chapter is inspect-only on this machine.** GPU
> execution needs an **NVIDIA GPU + CUDA**, and an MLIR built with the CUDA
> runner (`-DMLIR_ENABLE_CUDA_RUNNER=ON`) — *"sorry, no macOS"*, as the
> reference puts it. The lowering *passes* are pure IR→IR transforms and run
> fine on Apple Silicon, so you can watch a loop **turn into** GPU code
> ([`build.sh`](build.sh)); executing the result takes a CUDA box
> ([`run_on_gpu.sh`](run_on_gpu.sh)).
>
> **But the chapter still ends with the program *running* on this Mac:**
> [`run_on_macos.sh`](run_on_macos.sh) hands the very same `square.mlir` to the
> OpenMP lowering instead, and executes it as native Apple Silicon threads —
> see [Running it — one program, two backends](#running-it--one-program-two-backends).
>
> Based on Stephen Diehl's *"MLIR Part 8 — GPU Compilation with MLIR"*
> ([`../reference/`](../reference/)).

---

## How the GPU sees the work

A GPU runs a **grid** of **thread blocks**, each block a group of **threads**. You
launch a *kernel* across this grid (`kernel<<<blocks, threads>>>(...)` in CUDA),
and every thread runs the same code but on different data, locating its element
from its coordinates:

```text
   grid ───────────────────────────────────────────────
   ┌── block 0 ──┐ ┌── block 1 ──┐ ┌── block 2 ──┐ ...
   │ t0 t1 … t255│ │ t0 t1 … t255│ │ t0 t1 … t255│
   └─────────────┘ └─────────────┘ └─────────────┘
       each thread computes ONE element:
       tid = blockDim.x · blockIdx.x + threadIdx.x
             └ block size ┘ └ which block ┘ └ within block ┘
```

```c
int tid = blockDim.x * blockIdx.x + threadIdx.x;   // this thread's global index
if (tid < n) array[tid] = array[tid] * array[tid];
```

That index arithmetic — built from `blockIdx`, `blockDim`, `threadIdx` — is the
heart of GPU programming. [`square.cu`](square.cu) is the full CUDA-C version; its
kernel is exactly the snippet above:

*square.cu* (the CUDA kernel)
```c
__global__ void square(float *array, int n) {
  int tid = blockDim.x * blockIdx.x + threadIdx.x;   // this thread's global index
  if (tid < n)                                       // guard the array bounds
    array[tid] = array[tid] * array[tid];
}
```

### The CUDA compilation chain

NVIDIA's toolchain has its own IR ladder, parallel to LLVM's:

| Stage | What it is |
| --- | --- |
| CUDA C++ | `__global__` kernels, launched with `<<<…>>>` |
| **PTX** | architecture-neutral parallel-thread assembly (stable across GPU generations) |
| **CUBIN** | device-specific machine code for one `sm_XX` architecture |
| **SASS** | the actual GPU shader assembly CUBIN contains |
| FATBIN | a bundle of PTX + several CUBINs, so one binary supports many GPUs |

GPU architectures are named `sm_<N>`: `sm_75` (Turing), `sm_80` (Ampere, A100),
`sm_90` (Hopper, H100), etc. A CUBIN runs only on its generation; PTX is
JIT-compiled to fill the gaps.

### Connecting to MLIR

So where does MLIR enter this chain? The goal is to be able to specify
high-level tensor operations *dynamically* — i.e. at runtime — and have them
lowered to the appropriate GPU code. MLIR's progressive lowering walks from
mathematical intent down into the CUDA chain above through four well-defined
stages:

1. **High-level representation.** We start with abstract tensor computations in
   the `linalg` dialect — matmul, convolution, element-wise ops (Ch 4/5/7).
   These represent pure mathematical intent without implementation details,
   keeping the code close to the domain.
2. **Affine transformation.** Passes like `-convert-linalg-to-loops` /
   `-convert-linalg-to-affine-loops` turn them into loops whose memory accesses
   have precise mathematical structure (the `affine` dialect, Ch 3). This is
   where the crucial optimizations live — tiling, fusion, loop interchange —
   the ones that decide memory locality and computational efficiency (Ch 4).
3. **GPU mapping.** Passes like `-convert-parallel-loops-to-gpu` and
   `-convert-affine-for-to-gpu` map those loop structures onto the GPU's
   execution model: the `gpu` dialect, which explicitly represents thread
   blocks and threads while remaining *hardware-agnostic*.
4. **Hardware-specific lowering.** Finally we transition to NVIDIA-specific
   functionality — the `nvvm` dialect (with `nvgpu` as an optional middle rung,
   below) — and ultimately LLVM IR with GPU intrinsics, which the NVPTX backend
   converts to PTX assembly and then CUBIN machine code that runs directly on
   the GPU.

```text
linalg (math)  →  affine/scf (loops)  →  gpu (blocks & threads)  →  nvvm (NVIDIA IR)
   Ch 4/5/7          Ch 3/4                this chapter                  ↓
                                             CUBIN  ←  PTX  ←  LLVM IR + intrinsics
```

Every step preserves program semantics while moving closer to the target
hardware: mathematical expression → explicit loop nest → GPU execution
constructs → hardware-specific code. That's the whole trick — express the
computation at the level of *math*, and let a pipeline of IR→IR rewrites
manufacture code that still leverages the full capabilities of the hardware.
Note that the front half of this pipeline is exactly the dialects from earlier
chapters; nothing before stage 3 knows a GPU exists.

---

## The MLIR `gpu` dialect

MLIR doesn't make you write thread-index math. You express parallelism at a high
level (`scf.parallel`) and a sequence of passes manufactures the kernel. The
`gpu` dialect is the middle layer that makes this possible: it provides
CUDA/OpenCL-style abstractions for launching kernels — grids, blocks, threads,
device memory — while abstracting away the device- and driver-specific
operations needed to actually launch one. It is deliberately *target-agnostic*:
in principle it can lower to multiple GPU backends; we focus on the NVIDIA path
down to PTX.

**Structure: host/device split as nested IR.** Device code is organized inside
a `gpu.module`, with each kernel a `gpu.func ... kernel` (the enclosing
`builtin.module` gets a `gpu.container_module` attribute). This is CUDA's
`__global__`-vs-host-code split, expressed as IR nesting — which is what lets
one file carry both sides of the program through the pipeline.

A tour of the ops you'll actually meet, in three groups:

**Kernel & control ops** — launching from the host, control on the device:

| Op | What it does |
| --- | --- |
| `gpu.launch` | launch a kernel written *inline* as a region, with explicit grid/block dims |
| `gpu.launch_func` | launch a named `gpu.func` inside a `gpu.module` (the outlined form) |
| `gpu.terminator` | marks the end of a `gpu.launch` region |
| `gpu.return` | return from a `gpu.func` |
| `gpu.barrier` | synchronize all threads in a block — CUDA's `__syncthreads()` |
| `gpu.printf` | formatted printing from device code (debugging) |
| `gpu.wait` | wait for asynchronous GPU operations to complete |
| `gpu.binary` | a compiled kernel blob (PTX/CUBIN) embedded in the IR |

**Coordinate ops** — the CUDA builtins, each taking a dimension `x`/`y`/`z`:

| Op | CUDA equivalent |
| --- | --- |
| `gpu.thread_id` | `threadIdx` — this thread's ID within its block |
| `gpu.block_id` | `blockIdx` — this block's ID within the grid |
| `gpu.block_dim` | `blockDim` — threads per block |
| `gpu.grid_dim` | `gridDim` — blocks per grid |

**Memory ops** — host↔device data movement, mirroring `cudaMalloc`/`cudaMemcpy`:

| Op | What it does |
| --- | --- |
| `gpu.alloc` / `gpu.dealloc` | allocate/free device memory, typed as a memref |
| `gpu.memcpy` | host-to-device, device-to-host, or device-to-device copy |
| `gpu.host_register` / `gpu.host_unregister` | pin a host memref for direct device access |

**Two ways to launch.** `gpu.launch` embeds the kernel body inline as a region
(convenient mid-lowering, when the body is still being transformed);
`gpu.launch_func` invokes an outlined `gpu.func` by symbol — the form a real
driver launch needs. The `--gpu-kernel-outlining` pass converts the first into
the second, and you'll watch it do so below.

```mlir
// inline form — body is a region, terminated by gpu.terminator
gpu.launch blocks(%bx, %by, %bz) in (%g0 = %c1, %g1 = %c1, %g2 = %c1)
           threads(%tx, %ty, %tz) in (%b0 = %c2, %b1 = %c1, %b2 = %c1) {
  gpu.printf "Hello from %d\n" %tx : index
  gpu.terminator
}

// outlined form — kernel is a symbol inside a gpu.module
gpu.launch_func @kernels::@square_kernel
    blocks in (%nblocks, %c1, %c1) threads in (%c256, %c1, %c1)
    args(%arg0 : memref<1024xf32>)
```

### Below `gpu`: the NVIDIA-specific rungs (`nvvm` and `nvgpu`)

Lowering out of the hardware-agnostic `gpu` dialect toward an NVIDIA chip goes
through two more dialects.

**`nvvm`** is MLIR's mirror of NVVM IR — NVIDIA's LLVM-based device IR, the
same representation `nvcc`'s front-end produces internally before PTX. It is
essentially the LLVM dialect plus GPU intrinsics: the coordinate ops become
reads of PTX *special registers*, and the correspondence with CUDA is exact:

| CUDA builtin | NVVM intrinsic | `gpu` dialect op |
| --- | --- | --- |
| `threadIdx.{x,y,z}` | `llvm.nvvm.read.ptx.sreg.tid.{x,y,z}` | `gpu.thread_id {x,y,z}` |
| `blockIdx.{x,y,z}` | `llvm.nvvm.read.ptx.sreg.ctaid.{x,y,z}` | `gpu.block_id {x,y,z}` |
| `blockDim.{x,y,z}` | `llvm.nvvm.read.ptx.sreg.ntid.{x,y,z}` | `gpu.block_dim {x,y,z}` |
| `gridDim.{x,y,z}` | `llvm.nvvm.read.ptx.sreg.nctaid.{x,y,z}` | `gpu.grid_dim {x,y,z}` |
| `__syncthreads()` | `llvm.nvvm.barrier0()` | `gpu.barrier` |

**`nvgpu`** is a *bridge* dialect that sits between the target-agnostic
`gpu`/`vector` dialects and the low-level `nvvm`. Its trick is representing
PTX-specific operations while still using MLIR's high-level types (`memref`,
`vector`) — so you get access to NVIDIA hardware features without hand-wiring
NVVM intrinsics. What it exposes is the modern-GPU performance toolkit:

- **Tensor Memory Accelerator (TMA)** transfers between global and shared memory,
- **asynchronous copies** that overlap memory movement with computation (hiding latency),
- **swizzled** shared-memory layouts that avoid bank conflicts,
- **mbarriers** for fine-grained cross-thread synchronization,
- **warp-level matrix-multiply-accumulate** — the tensor-core ops behind
  Hopper/Blackwell-class matmul throughput.

When it's time to generate final code, `-convert-nvgpu-to-nvvm` lowers it to
NVVM intrinsics. Our square kernel is far too simple to need any of this — the
pipeline below goes straight from `gpu` to `nvvm` — but `nvgpu` is where a
production matmul kernel (the kind Ch 7's transformer actually wants) would
spend its time.

---

## Lowering `square.mlir` step by step

[`square.mlir`](square.mlir) is the high-level kernel — the same elementwise
square as `square.cu`, but written as one `scf.parallel` loop with **no index
math at all**. This is the identical op you handed to OpenMP in Chapter 3:

*square.mlir*
```mlir
func.func @square(%a: memref<1024xf32>) {
  %c0    = arith.constant 0    : index
  %c1    = arith.constant 1    : index
  %c1024 = arith.constant 1024 : index
  scf.parallel (%i) = (%c0) to (%c1024) step (%c1) {
    %v = memref.load %a[%i] : memref<1024xf32>
    %t = arith.mulf %v, %v : f32        // square this element
    memref.store %t, %a[%i] : memref<1024xf32>
    scf.reduce
  }
  return
}
```

### The lowering pipeline (`build.sh`)

`build.sh` lowers it in stages, saving each so you can read the transformation.

**Stage 1 — `scf.parallel` → a GPU kernel + host launch.** Three passes map the
loop onto the grid, then *outline* its body into a separate `gpu.func`:

```bash
$ mlir-opt square.mlir \
    --gpu-map-parallel-loops \
    --convert-parallel-loops-to-gpu \
    --gpu-kernel-outlining
```

The loop is gone. Its body is now a `gpu.func @square_kernel` indexed by
`gpu.block_id`, launched from the host by a `gpu.launch_func` — and the compiler
synthesized the `blockIdx·blockDim + threadIdx` index math for you (the
`affine.apply #map1` inside the kernel):

*build/square_gpu.mlir*
```mlir
#map = affine_map<(d0)[s0, s1] -> ((d0 - s0) ceildiv s1)>
#map1 = affine_map<(d0)[s0, s1] -> (d0 * s0 + s1)>
module attributes {gpu.container_module} {
  func.func @square(%arg0: memref<1024xf32>) {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c1024 = arith.constant 1024 : index
    %c1_0 = arith.constant 1 : index
    %0 = affine.apply #map(%c1024)[%c0, %c1]
    gpu.launch_func  @square_kernel::@square_kernel blocks in (%0, %c1_0, %c1_0) threads in (%c1_0, %c1_0, %c1_0)  args(%c1 : index, %c0 : index, %arg0 : memref<1024xf32>)
    return
  }
  gpu.module @square_kernel {
    gpu.func @square_kernel(%arg0: index, %arg1: index, %arg2: memref<1024xf32>) kernel attributes {known_block_size = array<i32: 1, 1, 1>} {
      %block_id_x = gpu.block_id  x
      %block_id_y = gpu.block_id  y
      %block_id_z = gpu.block_id  z
      %thread_id_x = gpu.thread_id  x
      %thread_id_y = gpu.thread_id  y
      %thread_id_z = gpu.thread_id  z
      %grid_dim_x = gpu.grid_dim  x
      %grid_dim_y = gpu.grid_dim  y
      %grid_dim_z = gpu.grid_dim  z
      %block_dim_x = gpu.block_dim  x
      %block_dim_y = gpu.block_dim  y
      %block_dim_z = gpu.block_dim  z
      %0 = affine.apply #map1(%block_id_x)[%arg0, %arg1]
      %1 = memref.load %arg2[%0] : memref<1024xf32>
      %2 = arith.mulf %1, %1 : f32
      memref.store %2, %arg2[%0] : memref<1024xf32>
      gpu.return
    }
  }
}
```

**Stage 2 — device kernel → NVVM (PTX-level).** Taking that `gpu.module` and
converting to NVVM turns the coordinate ops into PTX special registers and the
memref into raw pointer math:

```bash
$ mlir-opt build/square_gpu.mlir --convert-gpu-to-nvvm
```

`gpu.block_id x` has become `nvvm.read.ptx.sreg.ctaid.x` (literally the `blockIdx`
register), the load/store are `llvm.getelementptr` + `llvm.load`/`store`, and the
kernel is now an `llvm.func` tagged `nvvm.kernel`. (The full file opens with a
dozen `llvm.insertvalue` lines rebuilding the memref *descriptor struct* from
Chapter 2 out of the expanded arguments — elided here; read
`build/square_nvvm.mlir` for the whole thing.)

*build/square_nvvm.mlir* (the kernel, trimmed)
```mlir
  gpu.module @square_kernel {
    llvm.func @square_kernel(%arg0: i64, %arg1: i64, %arg2: !llvm.ptr, %arg3: !llvm.ptr, %arg4: i64, %arg5: i64, %arg6: i64)
        attributes {gpu.kernel, nvvm.kernel, nvvm.maxntid = array<i32: 1, 1, 1>, ...} {
      // ... memref descriptor rebuilt via llvm.insertvalue (Ch 2's struct) ...
      %8 = nvvm.read.ptx.sreg.ctaid.x : i32              // blockIdx.x
      %9 = llvm.sext %8 : i32 to i64
      %11 = affine.apply #map1(%10)[%1, %0]              // the synthesized index math
      %14 = llvm.getelementptr %13[%12] : (!llvm.ptr, i64) -> !llvm.ptr, f32
      %15 = llvm.load %14 : !llvm.ptr -> f32
      %16 = llvm.fmul %15, %15 : f32                     // the actual square
      %18 = llvm.getelementptr %17[%12] : (!llvm.ptr, i64) -> !llvm.ptr, f32
      llvm.store %16, %18 : f32, !llvm.ptr
      llvm.return
    }
  }
```

**Stage 3 — pin a target architecture.** Also starting from the stage-1
`gpu.module`, this tags it with the GPU generation to compile for (here Ampere),
which is what the later CUBIN-emission step reads:

```bash
$ mlir-opt build/square_gpu.mlir --nvvm-attach-target=chip=sm_80
```

The only change is an attribute on the module — `#nvvm.target<...>` — now attached
so a backend knows which `sm_XX` machine code to emit:

*build/square_target.mlir* (the tagged module line)
```mlir
  gpu.module @square_kernel [#nvvm.target<chip = "sm_80">] {
```

**Run (inspect):** `cd 8_gpu && bash build.sh`, then read the files in `build/`.

```
== stage 1: scf.parallel  ->  gpu.module (outlined kernel + host launch) ==
   look for: gpu.launch_func, gpu.module @square_kernel, gpu.block_id/thread_id
== stage 2: device kernel  ->  NVVM dialect (PTX-level) ==
   look for: nvvm.read.ptx.sreg.tid.x / ctaid.x  (= threadIdx / blockIdx)
== stage 3: attach a target GPU architecture (e.g. sm_80 = Ampere) ==
   look for: #nvvm.target<chip = "sm_80"> on the gpu.module
```

## Running it — one program, two backends

The chapter's opening claim — *a GPU kernel is one iteration of a parallel
loop* — means `square.mlir` doesn't care which backend it lands on. The fork
happens at `scf.parallel`, and both prongs are in this directory:

```
                    ┌─ convert-parallel-loops-to-gpu ─► gpu.launch_func ─► NVVM ─► PTX/CUBIN
scf.parallel ───────┤        (build.sh / run_on_gpu.sh — NVIDIA GPU, Linux)
  (square.mlir)     │
                    └─ convert-scf-to-openmp ─► omp.parallel ─► OpenMP runtime threads
                             (run_on_macos.sh — any CPU, runs HERE)
```

### Backend 1 — NVIDIA GPU (Linux, reference only here)

On a GPU machine the pipeline continues past NVVM: `--gpu-module-to-binary`
serializes the kernel to a CUBIN, `--gpu-to-llvm` lowers the host side, and
`mlir-runner` JIT-executes it linked against `libmlir_cuda_runtime`. That's
[`run_on_gpu.sh`](run_on_gpu.sh).

### Backend 2 — Apple Silicon CPU via OpenMP (runs on this Mac)

[`run_on_macos.sh`](run_on_macos.sh) concatenates the unchanged `square.mlir`
with a small host driver ([`main_macos.mlir`](main_macos.mlir) — fill an array
with 0,1,2,…, call `@square`, print the first 8 results), then swaps in
Chapter 3's OpenMP lowering where `build.sh` used the GPU passes:

```bash
mlir-opt build/square_cpu.mlir --convert-scf-to-openmp   # scf.parallel -> omp.parallel/omp.wsloop
# ... standard CPU lowering to the llvm dialect ...
mlir-translate + llc + clang -lomp                        # native arm64, linked against libomp
```

```
$ bash run_on_macos.sh          # needs: brew install libomp (same as Ch 3)
== running on Apple Silicon (expect 0 1 4 9 16 25 36 49) ==
0.0
1.0
4.0
9.0
16.0
25.0
36.0
49.0
```

The kernel body was not touched — the *same* `scf.parallel` iterations that
`build.sh` outlines into a `gpu.func` are here split across a team of CPU
threads by `omp.wsloop`. That's the portability argument of the whole
dialect design, demonstrated rather than asserted.

> **Aside — driving the pipeline from Python.** Everything `build.sh` does with
> `mlir-opt` can also be done in-process through the MLIR Python bindings
> (`mlir.ir.Module.parse(...)` + `mlir.passmanager.PassManager`, adding the same
> pass names with `pm.add("...")`). The reference uses this to build a tiny
> runtime compiler: parse an MLIR string, run this pass list, `mlir-translate` +
> `llc` the GPU module to PTX, and hand the PTX to the CUDA driver API
> (`cuModuleLoadData` / `cuLaunchKernel`). Same passes, different driver — we
> stay with the CLI here so every intermediate stage remains a file you can read.
> **That version of this pipeline is Chapter 9** ([`../9_python_bindings/`](../9_python_bindings/)),
> where it goes one step further than this chapter can: emitting real PTX on the Mac.

---

## Where this leaves the series

Trace one operation all the way down and you've seen the whole stack:

```
transformer softmax / matmul   (Ch 7 / Ch 4, in tensor + linalg)
   → tiled for the memory hierarchy        (Ch 4)
   → marked parallel                       (Ch 3, scf.parallel)
   → outlined into a GPU kernel            (this chapter, gpu dialect)
   → NVVM → PTX → CUBIN → running on thousands of cores
```

That is exactly what a production ML compiler (IREE, Triton, XLA) does — and it's
all the same MLIR machinery you built up from `func.func @main` returning 42 in
Chapter 1.

## Key takeaways

- **A GPU kernel is one parallel-loop iteration per thread.** The same
  `scf.parallel` that fed OpenMP (Ch 3) lowers to a GPU kernel — the backend is
  just a different choice of passes.
- **The `gpu` dialect mirrors CUDA but stays hardware-agnostic** (`gpu.module`,
  `gpu.launch_func`, `gpu.thread_id`); **NVVM** mirrors NVIDIA's device IR, where
  `gpu.block_id x` becomes `nvvm.read.ptx.sreg.ctaid.x`, lowering to PTX → CUBIN.
  In between, **`nvgpu`** bridges to NVIDIA-specific hardware features (TMA,
  async copies, tensor-core MMA) while keeping high-level memref/vector types.
- **MLIR writes the thread-index math for you** — the `affine.apply` for
  `blockDim·blockIdx + threadIdx` is synthesized by `--gpu-kernel-outlining`, never
  hand-written.
- **GPU execution needs NVIDIA hardware + CUDA-enabled MLIR** — on this Mac the
  GPU path stops at inspecting the generated device IR. But the **same program
  runs here through the OpenMP backend** (`run_on_macos.sh`), so "one high-level
  program, two backends" is demonstrated end to end, not just claimed.

**Almost the end.** You've gone from raw LLVM IR (Ch 1) to a transformer compiled
toward GPU kernels (Ch 8) — the full arc of the modern ML compiler stack. One
epilogue remains: [Chapter 9](../9_python_bindings/) rebuilds this pipeline as a
*library* — the MLIR Python bindings driving the same passes in-process, all the
way to real PTX. See the [top-level README](../README.md) for the whole map.

