# llvm

## LLVM Build
Which version should you choose for LLVM?
* GCC 14/15: These are the defaults on Ubuntu 25.04. They are very fast but often trip over older LLVM codebases (leading to the uint64_t and __arm_streaming errors you saw).
* GCC 12: Generally considered the "sweet spot" for older LLVM versions (like LLVM 14–17). It is modern enough for performance but doesn't have the strict header cleanups that break builds.

```
# Configure with specific compilers
cmake -S llvm -B build -G Ninja -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 -DLLVM_ENABLE_PROJECTS="clang" -DCMAKE_BUILD_TYPE=Release
```
Then,
```
cmake --build build  # -j2 (optional)
```

## LLVM exmaple compilation
For example,
```
cd llvm_example/Kaleidoscope/Chapter3
clang++ -g -O3 toy.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o toy
```
llvm-config provides the proper path for the library and header files.
```
% llvm-config --cxxflags --ldflags --system-libs --libs core
-I/usr/local/include -std=c++17   -fno-exceptions -funwind-tables -fno-rtti -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS
-L/usr/local/lib 
-lLLVMCore -lLLVMRemarks -lLLVMBitstreamReader -lLLVMBinaryFormat -lLLVMTargetParser -lLLVMSupport -lLLVMDemangle
-lrt -ldl -lm -lz
```
