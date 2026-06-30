#!/usr/bin/env bash
# Inspect-only: run each affine optimization pass and save the "after" IR into
# build/, so you can diff it against the original source file.
set -euo pipefail
rm -rf build
mkdir -p build

echo "== loop-invariant code motion =="
mlir-opt licm.mlir -affine-loop-invariant-code-motion -o ./build/licm_after.mlir

echo "== loop tiling (8x8) =="
mlir-opt tiling.mlir -affine-loop-tile="tile-size=8" -o ./build/tiling_after.mlir

echo "== loop unrolling (factor 4) =="
mlir-opt unroll.mlir -affine-loop-unroll="unroll-factor=4" -o ./build/unroll_after.mlir

echo "== loop fusion =="
mlir-opt fusion.mlir -affine-loop-fusion -o ./build/fusion_after.mlir

echo
echo "Compare each <name>.mlir with build/<name>_after.mlir, e.g.:"
echo "    diff licm.mlir build/licm_after.mlir"
