# Translate Gcd.mod to Gcd.o
./build/tools/driver/tinylang -o ./build/Gcd.o --filetype=obj ./examples/Gcd.mod
# Compile the while app.
clang ./examples/callgcd.c ./build/Gcd.o -o ./build/gcd
./build/gcd
