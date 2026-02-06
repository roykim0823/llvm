mkdir build
cd build
# the 3rd argument to fix libc++ not found issue on macbook pro
#cmake -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_FLAGS="-isysroot $(xcrun --show-sdk-path)" ..
cmake ..
make
./src/calc "with a: a*3"
./src/calc "with a, b: a*3+b*2" | llc -filetype=obj -relocation-model=pic -o expr.o
clang -o expr expr.o ../rtcalc.c
./expr
