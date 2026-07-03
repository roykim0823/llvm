#!/usr/bin/env bash
# The Apple Silicon backend — this one RUNS on this Mac.
#
# Chapter 8's opening claim, made executable: a GPU kernel is one iteration of
# a parallel loop, so the SAME square.mlir that build.sh lowers to a CUDA
# kernel can instead be handed to the OpenMP lowering (Ch 3) and executed as
# CPU threads, natively on Apple Silicon. One high-level program, two backends:
#
#   scf.parallel ──convert-parallel-loops-to-gpu──►  gpu.launch_func → PTX   (build.sh, NVIDIA)
#   scf.parallel ──convert-scf-to-openmp──────────►  omp.parallel → threads  (this script, any CPU)
#
# Needs Homebrew libomp (brew install libomp), same as Chapter 3's 5_openmp.
set -euo pipefail
mkdir -p build

OMP_PREFIX="$(brew --prefix libomp)"

echo "== stage 1: same square.mlir + a host main, scf.parallel -> omp.parallel =="
cat square.mlir main_macos.mlir > build/square_cpu.mlir
mlir-opt build/square_cpu.mlir --convert-scf-to-openmp -o build/square_omp.mlir
echo "   look for: omp.parallel / omp.wsloop where scf.parallel was"

echo "== stage 2: lower everything to the llvm dialect =="
mlir-opt build/square_omp.mlir \
  --convert-scf-to-cf \
  --convert-cf-to-llvm \
  --convert-openmp-to-llvm \
  --convert-arith-to-llvm \
  --convert-index-to-llvm \
  --finalize-memref-to-llvm \
  --convert-func-to-llvm \
  --reconcile-unrealized-casts \
  -o build/square_cpu_llvm.mlir

echo "== stage 3: native codegen + link against the OpenMP runtime =="
mlir-translate build/square_cpu_llvm.mlir --mlir-to-llvmir -o build/square_cpu.ll
llc -filetype=obj --relocation-model=pic build/square_cpu.ll -o build/square_cpu.o
clang build/square_cpu.o -o build/square_cpu \
  -L"${OMP_PREFIX}/lib" -lomp -Wl,-rpath,"${OMP_PREFIX}/lib"

echo "== running on Apple Silicon (expect 0 1 4 9 16 25 36 49) =="
./build/square_cpu
