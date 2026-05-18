// Tensor-level identity-matrix generator (callable as `_mlir_ciface_identity`).
//
// Caller passes an output memref<?x?xi32>; we build the matrix in tensor
// world with `tensor.generate`, then write it directly into the caller's
// buffer via `bufferization.materialize_in_destination`.
//
// Syntax cheat-sheet (for reference; the ops below are all used or shown):
//   tensor.empty(%dyn_sizes...)        // allocate dynamic tensor
//   tensor.from_elements %a, %b, %c    // tensor literal from SSA values
//   tensor.extract %t[%i, %j]          // read one element
//   tensor.insert %x into %t[%i, %j]   // produce new tensor with one element changed
//   tensor.generate %m, %n { ... }     // build a tensor with a per-index lambda
module {
  func.func @identity(%out: memref<?x?xi32>) attributes {llvm.emit_c_interface} {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %m = memref.dim %out, %c0 : memref<?x?xi32>
    %n = memref.dim %out, %c1 : memref<?x?xi32>

    %t = tensor.generate %m, %n {
      ^bb0(%i: index, %j: index):
        %ni = arith.index_cast %i : index to i32
        %nj = arith.index_cast %j : index to i32
        %eq = arith.cmpi eq, %ni, %nj : i32
        %v  = arith.extui %eq : i1 to i32
        tensor.yield %v : i32
    } : tensor<?x?xi32>

    bufferization.materialize_in_destination %t in writable %out
      : (tensor<?x?xi32>, memref<?x?xi32>) -> ()
    return
  }
}
