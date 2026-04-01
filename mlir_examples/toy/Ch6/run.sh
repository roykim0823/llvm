echo 'def main() { print([[1, 2], [3, 4]]); }' | ./build/toyc-ch6 -emit=mlir
echo 'def main() { print([[1, 2], [3, 4]]); }' | ./build/toyc-ch6 -emit=llvm
echo 'def main() { print([[1, 2], [3, 4]]); }' | ./build/toyc-ch6 -emit=mlir-affine
echo 'def main() { print([[1, 2], [3, 4]]); }' | ./build/toyc-ch6 -emit=mlir-llvm
echo 'def main() { print([[1, 2], [3, 4]]); }' | ./build/toyc-ch6 -emit=jit
