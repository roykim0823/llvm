# RUN: %toy < %s 2>&1 | FileCheck %s
# RUN: test -f output.o
# Chapter 8: after the REPL drains stdin, the driver emits target object code
# for the accumulated module into output.o in the working directory.

def average(x y) (x + y) * 0.5;
# CHECK-LABEL: define double @average(double %x, double %y)
# CHECK: fadd double
# CHECK: fmul double
# CHECK: Wrote output.o
