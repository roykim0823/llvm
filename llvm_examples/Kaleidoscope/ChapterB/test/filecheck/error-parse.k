# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# A parse error reports what was expected, where (file:line:col), and what
# was found instead -- and panic-mode recovery at record boundaries means
# one run reports EVERY parse error, not just the first.

def 123foo(x) x;
# CHECK: error-parse.k:6:5: error: expected 'function name' in prototype, got a number

def ok(x) x;

def bar(x) (x;
# CHECK: error-parse.k:11:14: error: expected ')' to close parenthesized expression, got ';'
# CHECK: 2 errors generated.
