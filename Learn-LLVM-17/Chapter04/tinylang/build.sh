mkdir build
cd build
cmake -DCMAKE_CXX_COMPILER=clang++ ..
make
# Translate Gcd.mod to Gcd.o
./tools/driver/tinylang -o Gcd.o --filetype=obj ../examples/Gcd.mod
# Compile the while app.
clang ../examples/callgcd.c Gcd.o -o gcd
./gcd
