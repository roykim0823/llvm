# Write your first instruction description #

In this exercise, we print all the non-generic instructions defined for the backend identified by `MyTarget`.

The goal of this exercise is to familiarize yourself with adding instruction in TableGen using the instr-info TableGen backend.

To do that, read the direction in `yourTurn-instrinfo.td`, implement the missing instructions and run the commands to test your solution by following the steps in the next two sections.

## Configure your build directory ##

```bash
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR=<path/to/llvm/install>/lib/cmake/llvm/ -Bbuild .
```

This will initialize your build directory in `build` (the `-B` option) with Ninja (`-G` option).

You must have a version of LLVM installed at `<path/to/llvm/install>` for this to succeed.
Either build and install your own llvm (with the `install` target from your LLVM build) or install an [official LLVM release](https://releases.llvm.org/) package.

## Build ##

```bash
ninja -C build
```

This builds the default target in the build directory.

Then run:
```bash
./build/print_inst
```

This will print all the non generic instructions.

You should see your added instructions and if you followed the direction, these instructions should have the same profile as the ones printed with the `SOLUTION_` prefix.

## Solution ##

Look at the content of `solution-instrinfo.td` for a possible solution.
