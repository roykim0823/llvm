mkdir build
cd build
cmake .. -G Ninja
ninja
# Translate Gcd.mod to Gcd.o
./tools/driver/tinylang -o Gcd.o --filetype=obj ../examples/Gcd.mod
# Compile the while app.
clang ../examples/callgcd.c Gcd.o -o gcd
./gcd
