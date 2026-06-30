#!/usr/bin/env bash
# Inspect-only: fuse the two elementwise ops into one and save the result.
set -euo pipefail
rm -rf build
mkdir -p build

#   --canonicalize                 general cleanup
#   --linalg-fuse-elementwise-ops  the actual fusion
#   --cse                          drop redundant ops
#   --linalg-generalize-named-ops  named ops (add/mul) -> linalg.generic
mlir-opt separate_ops.mlir \
  --canonicalize \
  --linalg-fuse-elementwise-ops \
  --cse \
  --linalg-generalize-named-ops \
  --linalg-fuse-elementwise-ops \
  -o ./build/fused_ops.mlir

echo "Wrote build/fused_ops.mlir — the two ops are now one linalg.generic."
echo "Compare:  diff separate_ops.mlir build/fused_ops.mlir"
