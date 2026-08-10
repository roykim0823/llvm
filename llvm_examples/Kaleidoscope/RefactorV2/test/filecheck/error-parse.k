# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# A parse error reports what was expected, where (line, col), and what was
# found instead -- and the driver exits non-zero.

def 123foo(x) x;
# CHECK: Parse error (5, 5): expected 'function name' in prototype
