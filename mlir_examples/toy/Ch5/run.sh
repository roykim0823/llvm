./build/toyc-ch5 ../../test_Example/Toy/Ch5/affine-lowering.mlir -emit=mlir-affine
echo ""
echo "---------"
echo "with -opt"
echo "---------"
./build/toyc-ch5 ../../test_Example/Toy/Ch5/affine-lowering.mlir -emit=mlir-affine -opt
