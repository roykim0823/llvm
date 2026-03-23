./build/toyc-ch2 codegen.toy -emit=mlir -mlir-print-debuginfo

# Do the RoundTrip:
./build/toyc-ch2 codegen.toy -emit=mlir -mlir-print-debuginfo 2> ./build/codegen.mlir
./build/toyc-ch2 ./build/codegen.mlir -emit=mlir
