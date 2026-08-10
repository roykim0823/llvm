# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 9: debug information. The driver prints the full module at exit;
# it must carry a DWARF compile unit, a DISubprogram per function, !dbg
# location attachments on instructions, and DILocation nodes.

def fib(x)
  if x < 3 then 1 else fib(x-1) + fib(x-2);

fib(6);

# CHECK: define double @fib(double %x)
# An instruction inside fib must carry a !dbg attachment.
# CHECK: !dbg

# The final module dump must contain the debug-info metadata graph.
# CHECK: !llvm.dbg.cu
# CHECK-DAG: DICompileUnit(language: DW_LANG_C
# CHECK-DAG: DISubprogram(name: "fib"
# CHECK-DAG: DILocation(line:
