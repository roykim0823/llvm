# Translate Point.mod to Point.s
./build/tools/driver/tinylang -o ./build/Point.s --filetype=asm ./examples/Point.mod
# Compile the while app.
#clang ./examples/callgcd.c ./build/Gcd.o -o ./build/gcd
#./build/gcd
