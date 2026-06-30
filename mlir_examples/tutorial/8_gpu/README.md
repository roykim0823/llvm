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

> **⚠️ This chapter is inspect-only on this machine.** GPU execution needs an
> **NVIDIA GPU + CUDA**, and an MLIR built with the CUDA runner
> (`-DMLIR_ENABLE_CUDA_RUNNER=ON`) — *"sorry, no macOS"*, as the reference puts
> it. The lowering *passes* are pure IR→IR transforms and run fine on Apple
> Silicon, so you can watch a loop **turn into** GPU code; you just can't run it
> here. [`run_on_gpu.sh`](run_on_gpu.sh) has the commands for a real CUDA box.
>
> Based on Stephen Diehl's *"MLIR Part 8 — GPU Compilation with MLIR"*
> ([`../reference/`](../reference/)).

---

## How the GPU sees the work

A GPU runs a **grid** of **thread blocks**, each block a group of **threads**. You
launch a *kernel* across this grid (`kernel<<<blocks, threads>>>(...)` in CUDA),
and every thread runs the same code but on different data, locating its element
from its coordinates:

```c
int tid = blockDim.x * blockIdx.x + threadIdx.x;   // this thread's global index
if (tid < n) array[tid] = array[tid] * array[tid];
```

That index arithmetic — built from `blockIdx`, `blockDim`, `threadIdx` — is the
heart of GPU programming. [`square.cu`](square.cu) is the full CUDA-C version (run
it with `nvcc` on a GPU box).

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

---

## The MLIR `gpu` dialect

MLIR doesn't make you write thread-index math. You express parallelism at a high
level (`scf.parallel`) and a sequence of passes manufactures the kernel. The
`gpu` dialect mirrors the CUDA model — `gpu.module` (a container of device
kernels), `gpu.func`, `gpu.launch_func` (the host-side launch), and
`gpu.block_id` / `gpu.thread_id` (the coordinate ops). Lowering further produces
the **NVVM** dialect — MLIR's mirror of NVIDIA's device IR, where the loop index
becomes `nvvm.read.ptx.sreg.tid.x` / `ctaid.x` (exactly `threadIdx` / `blockIdx`).

[`square.mlir`](square.mlir) is the high-level kernel — the same elementwise
square as `square.cu`, but written as one `scf.parallel` loop with no index math.

### The lowering pipeline (`build.sh`)

`build.sh` lowers it in stages, saving each so you can read the transformation:

```
scf.parallel
   │  --gpu-map-parallel-loops --convert-parallel-loops-to-gpu --gpu-kernel-outlining
   ▼
gpu.module { gpu.func @square_kernel ... }   +   host gpu.launch_func   (build/square_gpu.mlir)
   │  --convert-gpu-to-nvvm
   ▼
NVVM: nvvm.read.ptx.sreg.ctaid.x, ...        (PTX-level device IR)        (build/square_nvvm.mlir)
   │  --nvvm-attach-target=chip=sm_80
   ▼
gpu.module tagged #nvvm.target<chip="sm_80">  (ready for cubin emission)  (build/square_target.mlir)
```

After stage 1, the body that was a loop is now a `gpu.func` indexed by
`gpu.block_id`/`gpu.thread_id`, launched by a `gpu.launch_func` on the host — the
compiler synthesized the `tid = blockDim·blockIdx + threadIdx` math for you. Stage
2 turns those into PTX special registers; stage 3 pins a target architecture.

**Run (inspect):** `cd 8_gpu && bash build.sh`, then read the files in `build/`.

```
== stage 1: scf.parallel  ->  gpu.module (outlined kernel + host launch) ==
== stage 2: device kernel  ->  NVVM dialect (PTX-level) ==
== stage 3: attach a target GPU architecture (e.g. sm_80 = Ampere) ==
```

### Actually running it (on a CUDA box)

On a GPU machine the pipeline continues past NVVM: `--gpu-module-to-binary`
serializes the kernel to a CUBIN, `--gpu-to-llvm` lowers the host side, and
`mlir-runner` JIT-executes it linked against `libmlir_cuda_runtime`. That's
[`run_on_gpu.sh`](run_on_gpu.sh) — reference only here.

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
- **The `gpu` dialect mirrors CUDA** (`gpu.module`, `gpu.launch_func`,
  `gpu.thread_id`); **NVVM** mirrors NVIDIA's device IR, lowering to PTX → CUBIN.
- **MLIR writes the thread-index math for you** — you never hand-write
  `blockDim·blockIdx + threadIdx`.
- **Execution needs NVIDIA hardware + CUDA-enabled MLIR**; on this Mac we inspect
  the generated GPU IR, which is itself the payoff — watching a plain loop become
  a GPU kernel.

**The end.** You've gone from raw LLVM IR (Ch 1) to a transformer compiled toward
GPU kernels (Ch 8) — the full arc of the modern ML compiler stack. See the
[top-level README](../README.md) for the whole map.
