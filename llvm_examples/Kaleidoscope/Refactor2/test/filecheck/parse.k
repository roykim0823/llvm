# RUN: %toy < %s 2>&1 | FileCheck %s

def foo(x y) x + foo(y, 4.0);
# CHECK: Parsed a function definition.

extern sin(a);
# CHECK: Parsed an extern

y;
# CHECK: Parsed a top-level expr
