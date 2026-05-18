mkdir -p build

mlir-opt example.mlir \
--convert-func-to-llvm \
--convert-math-to-llvm \
--convert-index-to-llvm \
--convert-scf-to-cf \
--convert-cf-to-llvm \
--convert-arith-to-llvm \
--reconcile-unrealized-casts \
-o ./build/example_opt.mlir

mlir-runner -e main -entry-point-result=i32 ./build/example_opt.mlir

# To use the runner utils (e.g. for debug printing)
mlir-runner -e main -entry-point-result=i32 -shared-libs=/opt/homebrew/opt/llvm@20/lib/libmlir_runner_utils.dylib ./build/example_opt.mlir

# Compiling to a Shared Object
mlir-translate ./build/example_opt.mlir -mlir-to-llvmir -o ./build/example.ll
llc -filetype=obj --relocation-model=pic ./build/example.ll -o ./build/example.o
clang -shared -fPIC ./build/example.o -o ./build/libexample.so

# Emit native assembly text (.s) — symbolic form, no addresses
llc -filetype=asm --relocation-model=pic ./build/example.ll -o ./build/example.s

# Disassemble the object — shows hex addresses like your sample
objdump -d --no-show-raw-insn ./build/example.o > ./build/example.dis
# Or disassemble the dylib (Apple Silicon symbols are prefixed with `_`)
objdump -d --no-show-raw-insn ./build/libexample.so > ./build/libexample.dis