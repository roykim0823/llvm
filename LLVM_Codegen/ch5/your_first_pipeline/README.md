# Build your first pass pipeline #

In this exercise you will practice how to create your first pass pipeline with both the legacy and the new pass manager.

Go to `yourTurn/passPipelineWithLegacyPM.cpp` and `your_turn/passPipelineWithNewPM.cpp` to implement the pass pipeline for the legacy pass manager and the new pass manager respectively.
In other words, populate the `runYourTurnPassPipelineXXX` function from the related file to implement a pass pipeline with the legacy and new pass manager.

For this exercise, you will need to implement a pass pipeline that consists of the three following optimizations, in this order:
- The mem2reg pass (called PromoteMemoryToRegister)
- The instcombine pass
- The always inliner pass

Then, run this pipeline on the given `Module` argument of the `runYourTurnPassPipelineXXX` function.

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

This should produce in the `build` directory a binary named `your_first_pipeline`.

## Run ##

```bash
./build/your_first_pipeline [input.ll|.bc]
```

This will run both the reference implementation and your implementation on `input.ll` if specified or the default input if not.

Check that the output look similar for both your and the reference pipelines.

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

## Going Further ##

In the `solution` directory, the implementation of the pass pipeline using the new pass manager offers two different implementations:
- One using an adapter for the function passes
- One using a dedicated function pass manager for the function passes

The default implementation uses the function adapter and to use the dedicated function pass manager, you must set the `USE_MODULE_MGR` preprocessor macro in your build command.
You can achieve this by using for instance the following CMake command to configure your build directory.

```bash
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS_DEBUG='-DUSE_MODULE_MGR'-DLLVM_DIR=<path/to/llvm/install>/lib/cmake/llvm -Bbuild .
```

Next, run your pass pipeline like you did previously and observe how the output of the pass pipeline changes.
Try to understand what happens!
