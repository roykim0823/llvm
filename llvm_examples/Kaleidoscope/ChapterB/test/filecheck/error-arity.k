# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# Call checks: arity and unknown callees. Sema keeps going after each
# error, so BOTH problems below are reported by one run (ChapterA's
# codegen necessarily stopped at the first).

def f(a b) a + b;
f(1);
# CHECK: error-arity.k:7:1: error: incorrect number of arguments to 'f': expected 2, got 1

nope(2);
# CHECK: error-arity.k:10:1: error: unknown function 'nope'
# CHECK: 2 errors generated.
