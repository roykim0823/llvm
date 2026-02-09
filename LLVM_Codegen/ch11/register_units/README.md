# Write your first register description #

In this exercise, we print all the register of a specific backend.

The goal is to teach you how to write the register description for registers, register units, and register classes and use them in the register-info TableGen backend.

Follow the direction in `yourTurn-reginfo.td` and implement the described register hierarchy.

Then, use the steps below to test your changes.

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
./build/print_regunit
```

This will print the register info for both the solution and what you implemented.

You should aim to have your register info (printed under the `RegisterInfo for YourTurn` section) look exactly like what is under the `RegisterInfo for Solution` section.

## Solution ##

Look at the content of `solution-reginfo.td` for a possible solution.
