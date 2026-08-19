# RUN: %toy < %s 2>&1 | FileCheck %s
# The batch driver still recovers from codegen errors: they are reported,
# parsing continues, and the surviving definitions appear in the module dump.

def bad(x) y;
# CHECK: Error: Unknown variable name

unknownfn(1.0);
# CHECK: Error: Unknown function referenced

def good(x) x;
# CHECK: define double @good(double %x)
