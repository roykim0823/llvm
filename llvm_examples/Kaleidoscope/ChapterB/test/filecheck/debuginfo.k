# RUN: %toy %s -emit=ir -g | FileCheck %s
# With -g the module carries a DWARF compile unit, a DISubprogram per
# function anchored at the prototype's real source line, parameter
# declarations, and !dbg attachments driven by the AST's Location fields.

def fib(x)
  if x < 3 then 1
  else fib(x-1) + fib(x-2);

fib(6);

# CHECK: define double @fib(double %x)
# CHECK: {{(#dbg_declare|llvm.dbg.declare)}}
# CHECK: !dbg

# CHECK: !llvm.dbg.cu
# CHECK-DAG: DICompileUnit(language: DW_LANG_C
# CHECK-DAG: DISubprogram(name: "fib"{{.*}}line: 6
# CHECK-DAG: DILocation(line:
