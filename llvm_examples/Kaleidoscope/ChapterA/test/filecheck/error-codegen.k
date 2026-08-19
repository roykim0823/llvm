# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# Codegen errors carry the source location of the offending node.

def bad(x) y;
# CHECK: Codegen error (4, 12): unknown variable 'y'
