# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# Upstream handled a missing operator function with assert(F && ...), which
# aborts in debug builds and is UB in release; here it is a located sema
# error, and both uses below are reported in one run.
#
# (Only *unary* operators can reach this check from source text: an
# unregistered binary operator token never parses as a binary op at all,
# because the parser owns the precedence table.)

def f(x) !x;
# CHECK: error-unknown-op.k:10:10: error: unknown unary operator '!'

def g(x) $x;
# CHECK: error-unknown-op.k:13:10: error: unknown unary operator '$'
# CHECK: 2 errors generated.
