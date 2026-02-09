# Connect your first optimization pass #

In this exercise you will learn to connect the optimization that we provided in Chapter 4 in both the legacy and the new pass manager.

Go to `your_turn/passWithLegacyPM.cpp` and `your_turn/passWithNewPM.cpp` to implement the connection with the legacy pass manager and the new pass manager respectively.

Then, update `your_turn/passWithNewPM.h` and set the macro `YOUR_TURN_IS_READY` to `1` to enable the processing of your optimizations.

We put `TODO` comments in the places you have to update to materialize the connection.

For the body of your optimization just call the provided `solutionConstantPropagation` function, which is the implementation of the simple constant propagation that we saw in Chapter 4.

## Configuring your build environment ##

```bash
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR=<path/to/llvm/install>/lib/cmake/llvm -Bbuild .
```

This will initialize your build directory in `build` (the `-B` option) with Ninja (`-G` option).

You must have a version of LLVM installed at `<path/to/llvm/install>` for this to succeed.
Either build and install your own llvm (with the `install` target from your LLVM build) or install an [official LLVM release](https://releases.llvm.org/) package.

## Build ##

```bash
ninja -C build
```

This builds the default target in the build directory.

This should produce in the `build` directory a binary named `your_first_pass`.

## Run ##

```bash
./build/your_first_pass [input.ll|.bc]
```

This will run both the reference implementation and your implementation on `input.ll` if specified or the default input if not.

The run will apply both implementions to the input and will check whether the resulting IR is still correct.

To see how this is articulated, take a look at `main.cpp`.

## Solution ##

Look into the `solution` directory to see how to implement the connection of your optimization with both pass manager.

## Producing an input ##

Using the following command line, you can produce from a C file an input to give to your program:
```bash
clang -o - -S -emit-llvm test.c -O0 | sed -e 's#optnone##g' | <path/to/llvm/build>/bin/opt -S -passes=mem2reg,instnamer > input.ll
```

optnone => remove the attribute that prevents optimizations
mem2reg => get rid of stack accesses / build SSA
instnamer => get rid of the implicit variables
