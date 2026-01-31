mkdir -p build
cd build
cmake -DCMAKE_CXX_COMPILER=clang++ ..
make
./toy < ../example/average.txt
clang++ ../example/main.cpp output.o -o output
./output
