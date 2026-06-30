// 2-D convolution (cross-correlation) with the affine dialect.
//
//   output[i, j] = sum_{fi, fj} filter[fi, fj] * input[i + fi, j + fj]
//
// This is the workhorse of image processing and CNNs. Dimensions are dynamic, so
// one compiled kernel handles any sizes; the output shape is
// (in_h - k_h + 1, in_w - k_w + 1), which the caller allocates.
//
// The two inner loops accumulate into a running sum using `iter_args` — the same
// loop-carried-value pattern as scf.for in Chapter 1, here on affine.for. Each
// inner loop yields its final accumulator with `affine.yield`.
//
// Note `affine.load %input[%i + %fi, %j + %fj]`: the index is an affine function
// of the loop variables, which is what keeps this analyzable (and tileable /
// parallelizable). Exported as `_mlir_ciface_conv_2d` (see aot_main.py).
func.func @conv_2d(%input: memref<?x?xf32>, %filter: memref<?x?xf32>, %output: memref<?x?xf32>)
    attributes {llvm.emit_c_interface} {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %OH = memref.dim %output, %c0 : memref<?x?xf32>   // output rows
  %OW = memref.dim %output, %c1 : memref<?x?xf32>   // output cols
  %KH = memref.dim %filter, %c0 : memref<?x?xf32>   // filter rows
  %KW = memref.dim %filter, %c1 : memref<?x?xf32>   // filter cols

  affine.for %i = 0 to %OH {
    affine.for %j = 0 to %OW {
      %zero = arith.constant 0.0 : f32
      // Reduce over the filter window, threading the partial sum through iter_args.
      %acc = affine.for %fi = 0 to %KH iter_args(%a = %zero) -> (f32) {
        %acc2 = affine.for %fj = 0 to %KW iter_args(%b = %a) -> (f32) {
          %fv = affine.load %filter[%fi, %fj] : memref<?x?xf32>
          %iv = affine.load %input[%i + %fi, %j + %fj] : memref<?x?xf32>
          %p  = arith.mulf %iv, %fv : f32
          %n  = arith.addf %b, %p : f32
          affine.yield %n : f32
        }
        affine.yield %acc2 : f32
      }
      affine.store %acc, %output[%i, %j] : memref<?x?xf32>
    }
  }
  return
}
