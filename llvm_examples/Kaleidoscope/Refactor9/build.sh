rm -rf build
mkdir -p build
cmake -B build -G Ninja -S .
cmake --build build
./build/toy < cmd.txt
