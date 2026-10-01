# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# The headline of the diagnostic engine: one run reports every error in
# the module, in source order, with a clang-style summary count.

def f(a b) a + b;
def bad1(x) y;
def bad2(x) f(x);
1 = 2;
# CHECK: error-multiple.k:6:13: error: unknown variable 'y'
# CHECK: error-multiple.k:7:13: error: incorrect number of arguments to 'f': expected 2, got 1
# CHECK: error-multiple.k:8:3: error: destination of '=' must be a variable
# CHECK: 3 errors generated.
