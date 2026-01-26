mkdir build
cd build
cmake -DCMAKE_CXX_COMPILER=clang++ ..
make
./tools/driver/tinylang ../example/Gcd.mod
