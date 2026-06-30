// Kernel fusion (INSPECT-ONLY).
//
// Here `addmul` computes (a + b) * c as TWO separate Linalg ops. Each op is its
// own loop: the add writes a whole intermediate tensor to memory, then the mul
// reads it back. That's two passes over the data and an extra allocation.
//
// Fusion combines them into a SINGLE loop that reads each input once and writes
// the result directly — eliminating the intermediate. In Python terms:
//
//   unfused:  t=[a[i]+b[i] for i]; r=[t[i]*c[i] for i]   # two loops, temp array
//   fused:    r=[(a[i]+b[i])*c[i] for i]                 # one loop, no temp
//
// build.sh runs the fusion pipeline and writes build/fused_ops.mlir; you'll see
// the two named ops collapse into one linalg.generic whose body does both the
// addf and the mulf before yielding.
module {
  func.func @addmul(%a: tensor<10xf32>, %b: tensor<10xf32>, %c: tensor<10xf32>) -> tensor<10xf32> {
    %0 = tensor.empty() : tensor<10xf32>
    %1 = linalg.add ins(%a, %b : tensor<10xf32>, tensor<10xf32>)
                    outs(%0 : tensor<10xf32>) -> tensor<10xf32>
    %2 = tensor.empty() : tensor<10xf32>
    %3 = linalg.mul ins(%1, %c : tensor<10xf32>, tensor<10xf32>)
                    outs(%2 : tensor<10xf32>) -> tensor<10xf32>
    return %3 : tensor<10xf32>
  }
}
