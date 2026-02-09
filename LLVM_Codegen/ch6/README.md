# Getting started with TableGen #

This directory contains examples of TableGen inputs.

The goal here is to familiarize yourself with the TableGen syntax and understand how the records are expanded.

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

This should produce in the `build` directory one `.inc` file per `.td` file in the source directory.

Look at the produced `.inc` and the related `.td` and try to see if the expansion matches your understanding after reading Chapter 6.
