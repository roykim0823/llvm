# RUN: %toy %s -emit=ir -opt | FileCheck %s
# Mutable variables live in allocas; with -opt, mem2reg must promote every
# one of them back into SSA registers.

# A sequencing operator: evaluates both sides, yields the right one.
def binary: 1 (x y) y;
# CHECK-LABEL: define double @"binary:"(double %x, double %y)

def testVar(x) var a = x in (a = a + 1) : a;
# CHECK-LABEL: define double @testVar(double %x)
# CHECK-NOT: alloca
# CHECK: fadd double %x, 1.000000e+00
# CHECK-NOT: alloca
# CHECK: ret double

# Assignment to a function parameter (parameters are allocas too).
def testParamMutation(x) (x = x * 2) : x;
# CHECK-LABEL: define double @testParamMutation(double %x)
# CHECK-NOT: alloca
# CHECK: fmul double %x, 2.000000e+00
# CHECK-NOT: alloca
# CHECK: ret double
