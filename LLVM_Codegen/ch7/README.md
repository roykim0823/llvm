# Illustrate different LLVM IR constructs #

This directory contains the examples that illustrates the different LLVM IR constructs introduced in Chapter 7.

For the horizontal add example, please run:
```bash
clang -emit-llvm hadd_vector.c -S -o - -O0 | sed -e 's#optnone##g' | <path/to/llvm/build>/bin/opt -S -passes=mem2reg,instnamer
```

Additionally the `check_vec_int_ty.cpp` file shows the different ways to check that an integer type is a vector type.

Look at the implementation of `isVectorOfIntV1` and `isVectorOfIntV2` in this file.

To run `check_vec_int_ty.cpp` follow the steps below.



## Configuring your build environment ##

```bash
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR=<path/to/llvm/install>/lib/cmake/llvm -Bbuild .
```

## To build ##

```bash
ninja -Cbuild
```

## To run ##

```bash
./build/check_vec_int_ty [input.ll]
```

You can provide an input for `check_vec_in_ty` if you want.


## To produce an input for the `check_vec_int_ty` executable ##

To produce an input:
```bash
clang -o - -S -emit-llvm test.c -O0 | sed -e 's#optnone##g' | <path/to/llvm/build>/bin/opt -S -passes=mem2reg,instnamer > input.ll
```

optnone => remove the attribute that prevents optimizations
mem2reg => get rid of stack accesses / build SSA
instnamer => get rid of the implicit variables

