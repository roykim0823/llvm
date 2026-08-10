rm -rf ./build
cmake -B build -G Ninja -S .
cmake --build build
cd build && ctest
