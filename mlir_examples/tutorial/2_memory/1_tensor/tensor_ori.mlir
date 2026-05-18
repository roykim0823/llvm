func.func @identity(%m : index, %n : index) -> tensor<?x?xi32> attributes {llvm.emit_c_interface} {
  %out = tensor.generate %m, %n {
  ^bb0(%i : index, %j : index):
    %ni = arith.index_cast %i : index to i32
    %nj = arith.index_cast %j : index to i32
    %elem = arith.cmpi eq, %ni, %nj : i32
    %elem_i32 = arith.extui %elem : i1 to i32
    tensor.yield %elem_i32 : i32
  } : tensor<?x?xi32>
  return %out : tensor<?x?xi32>
}