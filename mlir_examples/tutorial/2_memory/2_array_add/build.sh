mkdir -p build
mlir-opt array_add.mlir \
-finalize-memref-to-llvm \
-convert-scf-to-cf \
-convert-cf-to-llvm \
-convert-arith-to-llvm \
-convert-func-to-llvm \
-reconcile-unrealized-casts \
-o ./build/array_add_opt.mlir

mlir-translate ./build/array_add_opt.mlir \
-mlir-to-llvmir \
-o ./build/array_add_opt.ll

llc -filetype=obj --relocation-model=pic ./build/array_add_opt.ll -o ./build/array_add_opt.o
clang -shared -fPIC ./build/array_add_opt.o -o ./build/array_add.so