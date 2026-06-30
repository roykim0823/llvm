#!/usr/bin/env bash
# Build the named-op elementwise add and run it; also save the lowered form of the
# linalg.generic version so you can see what the named op expands to.
set -euo pipefail
rm -rf build
mkdir -p build

# (inspect) what linalg.generic lowers to — a plain scf loop nest.
mlir-opt generic_add.mlir -convert-linalg-to-loops -o ./build/generic_add_loops.mlir

# (runnable) lower the named linalg.add to the llvm dialect and compile.
mlir-opt add.mlir \
  -convert-linalg-to-loops \
  -convert-scf-to-cf \
  -convert-cf-to-llvm \
  -convert-arith-to-llvm \
  -finalize-memref-to-llvm \
  -convert-func-to-llvm \
  -reconcile-unrealized-casts \
  -o ./build/add_opt.mlir

mlir-translate ./build/add_opt.mlir -mlir-to-llvmir -o ./build/add.ll
llc -filetype=obj --relocation-model=pic ./build/add.ll -o ./build/add.o
clang -shared -fPIC ./build/add.o -o ./build/libadd.dylib

python3 aot_main.py
