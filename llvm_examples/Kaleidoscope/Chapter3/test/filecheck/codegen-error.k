# RUN: %toy < %s 2>&1 | FileCheck %s
# Codegen errors must be reported and must not abort the driver.

def bad(x) y;
# CHECK: Error: Unknown variable name

unknownfn(1.0);
# CHECK: Error: Unknown function referenced

def good(x) x;
# CHECK: define double @good(double %x)

def good(x) x+1;
# CHECK: Error: Function cannot be redefined.

def stillgoing(x) x;
# CHECK: define double @stillgoing(double %x)
