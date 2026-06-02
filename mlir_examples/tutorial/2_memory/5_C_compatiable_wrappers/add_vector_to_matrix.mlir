
module {
  func.func @add_vector_to_matrix(%A: memref<3xf32>)
    -> memref<3xf32> attributes {llvm.emit_c_interface} {
    return %A : memref<3xf32>
  }
}
