mkdir -p build

mlir-opt tensor.mlir \
-one-shot-bufferize="bufferize-function-boundaries=true" \
-convert-linalg-to-loops \
-convert-bufferization-to-memref \
-finalize-memref-to-llvm \
-convert-scf-to-cf \
-convert-cf-to-llvm \
-convert-arith-to-llvm \
-convert-func-to-llvm \
-reconcile-unrealized-casts \
-o ./build/tensor_opt.mlir

mlir-translate ./build/tensor_opt.mlir \
-mlir-to-llvmir \
-o ./build/tensor_opt.ll

llc -filetype=obj --relocation-model=pic ./build/tensor_opt.ll -o ./build/tensor_opt.o
clang -shared -fPIC ./build/tensor_opt.o -o ./build/tensor.so

python aot_main.py

mlir-opt tensor_ori.mlir \
-one-shot-bufferize="bufferize-function-boundaries=true" \
-convert-linalg-to-loops \
-convert-bufferization-to-memref \
-finalize-memref-to-llvm \
-convert-scf-to-cf \
-convert-cf-to-llvm \
-convert-arith-to-llvm \
-convert-func-to-llvm \
-reconcile-unrealized-casts \
-o ./build/tensor_ori_opt.mlir

mlir-translate ./build/tensor_ori_opt.mlir \
-mlir-to-llvmir \
-o ./build/tensor_ori_opt.ll

llc -filetype=obj --relocation-model=pic ./build/tensor_ori_opt.ll -o ./build/tensor_ori_opt.o
clang -shared -fPIC ./build/tensor_ori_opt.o -o ./build/tensor_ori.so

python aot_main_ori.py










python aot_main_ori.py
