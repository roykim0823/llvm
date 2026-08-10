# RUN: %toy %s -emit=jit -opt | FileCheck %s
# End-to-end execution: each top-level expression is compiled into its own
# module, run, and printed, in source order.

4 + 5;
# CHECK: Evaluated to 9.000000

def fib(x) if x < 3 then 1 else fib(x-1) + fib(x-2);
fib(10);
# CHECK: Evaluated to 55.000000

# Mutable variables and assignment.
var a = 1.0 in (a = a + 2.0) + a;
# CHECK: Evaluated to 6.000000

# The for-loop end condition uses the pre-increment value, so the body runs
# for j = 1, 2 and 3: i accumulates 6; the loop itself evaluates to 0.
var i = 0.0 in ((for j = 1.0, j < 3.0 in (i = i + j)) + i);
# CHECK: Evaluated to 6.000000

# User-defined operators execute like any function.
def binary@ 5 (x y) x + y * 2;
1 @ 3;
# CHECK: Evaluated to 7.000000

def unary-(v) 0 - v;
-(2 @ 4);
# CHECK: Evaluated to -10.000000
