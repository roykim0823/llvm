rm -rf build
mkdir -p build
cmake -B build -G Ninja -S .
cmake --build build
cd build
./toy < ../example/average.txt
clang++ ../example/main.cpp output.o -o output
./output
