// linalg.generic — the most fundamental Linalg op (INSPECT-ONLY).
//
// Every other Linalg op is a specialization of linalg.generic. It describes a
// loop nest *declaratively*: instead of writing the loops, you give three things
// and let the compiler generate the nest.
//
//   indexing_maps  — how each tensor is indexed by the loop variables. Here all
//                    three use (d0) -> (d0): element i of each tensor maps to the
//                    same loop index i (a plain elementwise access).
//   iterator_types — one per loop dimension: "parallel" (independent iterations),
//                    "reduction" (accumulate into one value), or "window".
//   the region     — the scalar computation per element; ins() values arrive as
//                    block args, the result is returned with linalg.yield.
//
// This computes the elementwise sum of two tensors — NumPy's `a + b`.
//
// Inspect / lower to loops with:
//   mlir-opt generic_add.mlir
//   mlir-opt generic_add.mlir -convert-linalg-to-loops
#map_1d = affine_map<(d0) -> (d0)>

func.func @add_tensors(%arg0: tensor<10xf32>, %arg1: tensor<10xf32>) -> tensor<10xf32> {
  %result = tensor.empty() : tensor<10xf32>
  %0 = linalg.generic {
      indexing_maps  = [#map_1d, #map_1d, #map_1d],   // arg0, arg1, result
      iterator_types = ["parallel"]
    }
    ins(%arg0, %arg1 : tensor<10xf32>, tensor<10xf32>)
    outs(%result : tensor<10xf32>) {
  ^bb0(%a: f32, %b: f32, %out: f32):
    %1 = arith.addf %a, %b : f32
    linalg.yield %1 : f32
  } -> tensor<10xf32>
  return %0 : tensor<10xf32>
}
