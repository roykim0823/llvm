# Building Machine IR #

In this exercise, you will build your first function using the Machine intermediate representation (IR).

Open `your_turn/populate_function.cpp` and implement the `populateMachineIR` function according to the comment
in that file.

We recommend to start by running the program once to see the output of the solution.

This way you will get an idea of which opcodes you should use for your machine instructions.

At the end, your produced IR should match what is printed between the `# Machine code for function solution_foo` and `# End machine code for function solution_foo.` lines.

Your function is printed between the `# Machine code for function foo` and `# End machine code for function foo.` lines.

## Configuring your environment ##


```bash
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR=<path/to/llvm/install>/lib/cmake/llvm -Bbuild .
ninja -Cbuild
```

## Running the example ##

```bash
./build/build_mir
```

## Solution ##

If your output doesn't match the solution, look at `solution/populate_function.cpp` to see how to implement the desired IR.
