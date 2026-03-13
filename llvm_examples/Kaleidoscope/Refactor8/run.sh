cd build  # to make output.o ./build
./toy < ../example/average.txt
nm -g output.o  # check the _average
clang++ ../example/main.cpp output.o -o output
./output
cd ..
