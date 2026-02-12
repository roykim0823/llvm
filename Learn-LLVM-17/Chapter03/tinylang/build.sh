mkdir build
cd build
cmake .. -G Ninja
ninja
./tools/driver/tinylang ../example/Gcd.mod
