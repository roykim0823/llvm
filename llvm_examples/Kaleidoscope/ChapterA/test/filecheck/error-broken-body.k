# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# A failing body must not erase a function that earlier code already calls:
# g's call below references foo's declaration, so when foo's definition fails
# the broken body is dropped but the declaration survives (no dangling uses).

extern foo(x);
def g(x) foo(x);
def foo(x) undefined_var;
# CHECK: Codegen error (8, 12): unknown variable 'undefined_var'
