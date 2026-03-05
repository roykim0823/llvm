mlir-tblgen -gen-dialect-decls ./include/toy/Ops.td -I /opt/homebrew/opt/llvm@20/include/ -o ./build/dialect-decls.inc 
mlir-tblgen -gen-op-defs ./include/toy/Ops.td -I /opt/homebrew/opt/llvm@20/include/ -o ./build/op-defs.inc
./build/toyc-ch2 codegen.toy -emit=mlir -mlir-print-debuginfo

# Do the RoundTrip:
./build/toyc-ch2 codegen.toy -emit=mlir -mlir-print-debuginfo 2> ./build/codegen.mlir
./build/toyc-ch2 ./build/codegen.mlir -emit=mlir
