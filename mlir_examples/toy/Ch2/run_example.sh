mlir-tblgen -gen-dialect-decls ./include/toy/Ops.td -I /opt/homebrew/opt/llvm/include/ -o ./build/dialect-decls.inc 
mlir-tblgen -gen-op-defs ./include/toy/Ops.td -I /opt/homebrew/opt/llvm/include/ -o ./build/op-defs.inc 
