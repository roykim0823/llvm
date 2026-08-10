# RUN: %toy < %s 2>&1 | FileCheck %s
# Codegen errors must be reported and must not abort the driver.

def bad(x) y;
# CHECK: Error: Unknown variable name

unknown_fn(1.0);
# CHECK: Error: Unknown function referenced

def good(x) x;
# CHECK: define double @good(double %x)
