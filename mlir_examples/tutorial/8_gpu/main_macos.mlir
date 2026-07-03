// Host driver for the CPU (Apple Silicon) backend — see run_on_macos.sh.
//
// This file contains NO kernel logic. It allocates a 1024-element array,
// fills it with 0,1,2,..., calls the exact same @square from square.mlir
// (concatenated in front of this file by the build script), and prints the
// first 8 results — expected: 0, 1, 4, 9, 16, 25, 36, 49.
llvm.func @printf(!llvm.ptr, ...) -> i32
llvm.mlir.global private constant @fmt("%.1f\0A\00") {addr_space = 0 : i32}

func.func @main() -> i32 {
  %buf   = memref.alloc() : memref<1024xf32>
  %c0    = arith.constant 0 : index
  %c1    = arith.constant 1 : index
  %c8    = arith.constant 8 : index
  %c1024 = arith.constant 1024 : index

  // buf[i] = i
  scf.for %i = %c0 to %c1024 step %c1 {
    %ii = arith.index_cast %i : index to i32
    %f  = arith.sitofp %ii : i32 to f32
    memref.store %f, %buf[%i] : memref<1024xf32>
  }

  call @square(%buf) : (memref<1024xf32>) -> ()

  // print the first 8 squared values (%f varargs promote to f64)
  %fs = llvm.mlir.addressof @fmt : !llvm.ptr
  scf.for %i = %c0 to %c8 step %c1 {
    %v  = memref.load %buf[%i] : memref<1024xf32>
    %vd = arith.extf %v : f32 to f64
    llvm.call @printf(%fs, %vd) vararg(!llvm.func<i32 (ptr, ...)>)
      : (!llvm.ptr, f64) -> i32
  }
  %z = arith.constant 0 : i32
  return %z : i32
}
