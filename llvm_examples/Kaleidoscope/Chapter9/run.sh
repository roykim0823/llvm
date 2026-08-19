#!/bin/bash
./build/toy < example/cmd.txt
#./build/toy < example/fib.ks |& clang -o ./build/a.out -x ir -
./build/toy < example/fib.ks 2>&1 | clang -o ./build/a.out -x ir -
./build/a.out
