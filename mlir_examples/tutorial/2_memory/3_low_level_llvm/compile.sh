#!/usr/bin/env bash
# Build the vectorized array_add kernel into a shared library that
# Python can dlopen and call through ctypes.
set -euo pipefail

mlir-opt array_add_vec.mlir \
  --convert-vector-to-llvm \
  --convert-scf-to-cf \
  --convert-cf-to-llvm \
  --convert-arith-to-llvm \
  --convert-func-to-llvm \
  --convert-index-to-llvm \
  --finalize-memref-to-llvm \
  --reconcile-unrealized-casts \
  -o array_add_vec_opt.mlir

mlir-translate array_add_vec_opt.mlir \
  -mlir-to-llvmir \
  -o array_add_vec.ll

llc -filetype=obj --relocation-model=pic array_add_vec.ll \
  -o array_add_vec.o
clang -shared -fPIC array_add_vec.o -o libarray_add_vec.dylib

python3 aot_main.py
