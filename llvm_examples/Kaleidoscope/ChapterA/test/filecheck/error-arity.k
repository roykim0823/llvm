# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s

def f(a b) a + b;
f(1);
# CHECK: Codegen error (4, 1): incorrect number of arguments: expected 2, got 1

# Also check the unknown-function and unknown-unary-operator paths in a
# second RUN with separate inputs, keeping one error per invocation since
# codegen stops at the first failure.
