clang -fno-discard-value-names -O1 -S -emit-llvm gcd.c -o named_gcd.ll
clang -O1 -S -emit-llvm gcd.c -o unnamed_gcd.ll
clang -fno-discard-value-names -S -emit-llvm gcd.c -o named_O0_gcd.ll
clang -S -emit-llvm gcd.c -o unnamed_O0_gcd.ll
