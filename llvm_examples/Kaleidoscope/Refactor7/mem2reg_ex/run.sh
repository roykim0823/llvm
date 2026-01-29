llvm-as < example.ll | opt -passes=mem2reg | llvm-dis
