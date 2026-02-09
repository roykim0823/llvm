# Building LLVM IR #

In this exercise, you will build your first function using the LLVM intermediate representation (IR).

Open `your_turn/populate_function.cpp` and implement the `myBuildModule` function according to the comment
in that file.
Then, follow the steps below to configure and build the test program and check that your solution matches the output of the provided solution.

In other words, check that the printed LLVM IR after `## Processing module from Your solution implementation` line matches the solution printed after the `## Processing module from Reference implementation` line.

## Configuring your environment ##


```bash
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR=<path/to/llvm/install>/lib/cmake/llvm -Bbuild .
ninja -Cbuild
```

## Running the example ##

```bash
./build/build_ir
```

## Solution ##

If your output doesn't match the solution, look at `solution/populate_function.cpp` to see how to implement the desired IR.
