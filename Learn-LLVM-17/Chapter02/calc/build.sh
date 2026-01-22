mkdir build
cd build
cmake -DCMAKE_CXX_COMPILER=clang++ ..
make
./src/calc "with a: a*3"
./src/calc "with a, b: a*3+b*2" | llc -filetype=obj -relocation-model=pic -o expr.o
clang -o expr expr.o ../rtcalc.c
./expr
