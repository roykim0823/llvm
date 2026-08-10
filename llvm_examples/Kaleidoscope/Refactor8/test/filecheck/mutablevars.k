# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 7: mutable variables are stored in stack allocas, and the FPM's
# PromotePass (mem2reg) is expected to promote every alloca back into SSA
# registers. The REPL prints post-FPM IR, so no alloca may survive.

# A sequencing operator, as in the tutorial: evaluates both sides, yields the right one.
def binary: 1 (x y) y;
# CHECK-LABEL: define double @"binary:"(double %x, double %y)

def testVar(x) var a = x in (a = a + 1) : a;
# CHECK-LABEL: define double @testVar(double %x)
# CHECK-NOT: alloca
# CHECK: [[INC:%.*]] = fadd double %x, 1.000000e+00
# CHECK-NOT: alloca
# CHECK: ret double

# Assignment to a function parameter (parameters are allocas in Ch7 too).
def testParamMutation(x) (x = x * 2) : x;
# CHECK-LABEL: define double @testParamMutation(double %x)
# CHECK-NOT: alloca
# CHECK: fmul double %x, 2.000000e+00
# CHECK-NOT: alloca
# CHECK: ret double
