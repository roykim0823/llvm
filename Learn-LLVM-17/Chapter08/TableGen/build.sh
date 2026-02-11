rm -rf ./build
mkdir -p build
cmake -DCMAKE_CXX_COMPILER=clang++ -B build
cmake --build build
./build/tinylang-tblgen --gen-tokens -o ./build/TokenFilter.inc KeywordC.td
./build/tinylang-tblgen --gen-tokens -o ./build/TokenFilterM2.inc KeywordM2.td
echo "Check build/TokenFilter.inc and build/TokenFilterM2.inc for generated token filters."