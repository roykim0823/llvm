#!/usr/bin/env bash
# Chapter 9's pipeline, two ways:
#
#   1. CLI twin — the exact pass list from compile.py, run through mlir-opt in
#      stages so every intermediate IR lands in build/ for reading (this is
#      Chapter 8's "save each stage" style, extended to the full compiler).
#      Runs on this Mac with Homebrew llvm@20 alone — no Python bindings needed —
#      and produces real PTX. It is the ground truth compile.py is checked against.
#   2. Bindings — if `import mlir` works, also run compile.py itself (the same
#      passes as ONE in-process PassManager, no intermediate files).
#
# PTX *generation* is pure compilation and works anywhere; *executing* the PTX
# needs an NVIDIA GPU (see run.py).
set -euo pipefail
rm -rf build
mkdir -p build

CHIP="${1:-sm_80}"

echo "== stage 1: linalg on tensors  ->  scf.parallel on memrefs =="
mlir-opt square.mlir --pass-pipeline="builtin.module(
  canonicalize,
  one-shot-bufferize{bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map},
  canonicalize,
  convert-linalg-to-parallel-loops
)" -o build/square_loops.mlir
echo "   look for: memref args (tensors are gone), scf.parallel (%arg2, %arg3)"

echo "== stage 2: scf.parallel  ->  gpu.launch region (mapped to the grid) =="
mlir-opt build/square_loops.mlir --pass-pipeline="builtin.module(
  func.func(gpu-map-parallel-loops),
  convert-parallel-loops-to-gpu
)" -o build/square_gpu_launch.mlir
echo "   look for: gpu.launch blocks(...) threads(...), body still inline"

echo "== stage 3: outline the kernel  ->  gpu.module + gpu.launch_func =="
mlir-opt build/square_gpu_launch.mlir --gpu-kernel-outlining \
  -o build/square_outlined.mlir
echo "   look for: gpu.module @square_kernel, gpu.func ... kernel, gpu.block_id"

echo "== stage 4: device -> nvvm, pin ${CHIP}, host -> llvm dialect =="
mlir-opt build/square_outlined.mlir --pass-pipeline="builtin.module(
  lower-affine,
  expand-strided-metadata,
  normalize-memrefs,
  gpu.module(convert-gpu-to-nvvm{index-bitwidth=0 use-bare-ptr-memref-call-conv}),
  nvvm-attach-target{chip=${CHIP} features=+ptx80 O=3},
  convert-nvvm-to-llvm,
  reconcile-unrealized-casts,
  gpu-to-llvm{use-bare-pointers-for-host use-bare-pointers-for-kernels}
)" -o build/square_lowered.mlir
echo "   look for: nvvm.read.ptx.sreg.ctaid.x/y, #nvvm.target<chip = \"${CHIP}\">"

echo "== stage 5: extract the gpu.module body as a standalone module =="
{
  echo "module {"
  awk '/^  gpu.module/,/^  }$/' build/square_lowered.mlir | tail -n +2 | sed '$d'
  echo "}"
} > build/square_kernel.mlir
echo "   (compile.py does this by walking module.body.operations instead of awk)"

echo "== stage 6: kernel -> LLVM IR -> PTX (mlir-translate + llc, no GPU needed) =="
mlir-translate --mlir-to-llvmir build/square_kernel.mlir -o build/kernel.ll
llc -march=nvptx64 -mcpu="${CHIP}" build/kernel.ll -o build/kernel.ptx
echo "   look for: .visible .entry square_kernel, %ctaid.x/y  in build/kernel.ptx"

echo "== stage 7: same thing in-process, via the MLIR Python bindings =="
if python3 -c "import mlir" 2>/dev/null; then
  python3 compile.py square.mlir --chip "${CHIP}" > /dev/null
  echo "   compile.py OK — compare its build/kernel.ptx with stage 6's"
  if python3 -c "import numpy" 2>/dev/null; then
    echo "== stage 8: CPU target — JIT and EXECUTE on this machine =="
    python3 run_macos.py
  fi
else
  echo "   (skipped: MLIR Python bindings not on this python3 — run instead:"
  echo "    uv run --no-project --with mlir-python-bindings --with mlir --with numpy \\"
  echo "      --find-links https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest \\"
  echo "      python3 compile.py square.mlir --chip ${CHIP}   # and: python3 run_macos.py)"
fi

echo
echo "Wrote build/square_{loops,gpu_launch,outlined,lowered,kernel}.mlir, kernel.ll, kernel.ptx."
echo "To execute the PTX you need an NVIDIA GPU: see run.py."
