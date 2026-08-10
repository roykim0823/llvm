# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# Upstream handles a missing operator function with assert(F && ...), which
# aborts in debug builds and is UB in release. v2 reports and exits.

def unknown_op_use(x) !x;
# CHECK: Codegen error (5, 23): unknown unary operator '!'
# CHECK-NOT: Assertion
