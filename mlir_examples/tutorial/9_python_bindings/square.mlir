// The input to our mini compiler: elementwise square over a 10x10 tensor,
// written with a *named* linalg op — pure mathematical intent, no loops, no
// buffers, no threads. compile.py lowers this all the way to NVIDIA PTX.
func.func @square(%input: tensor<10x10xf32>, %output: tensor<10x10xf32>) -> tensor<10x10xf32> {
  %0 = linalg.square ins(%input : tensor<10x10xf32>)
                     outs(%output : tensor<10x10xf32>) -> tensor<10x10xf32>
  return %0 : tensor<10x10xf32>
}
