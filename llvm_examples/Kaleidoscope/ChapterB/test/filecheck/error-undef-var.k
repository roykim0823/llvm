# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# Sema resolves every name before any IR exists; the error carries the
# source location of the offending node.

def bad(x) y;
# CHECK: error-undef-var.k:5:12: error: unknown variable 'y'
# CHECK: 1 error generated.
