rm -rf ./build
mkdir build
cmake -B build -G Ninja -S .
cmake --build build
./build/toy < cmd.txt
