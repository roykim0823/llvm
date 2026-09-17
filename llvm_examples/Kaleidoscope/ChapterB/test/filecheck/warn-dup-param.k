# RUN: %toy %s -emit=ir 2>&1 | FileCheck %s
# A duplicate parameter is legal (the second binding wins, as in IRGen) but
# almost certainly a mistake: sema warns, the compile still succeeds, and
# IR is still emitted -- note there is no `not` on the RUN line.

def f(x x) x;
# CHECK: warn-dup-param.k:6:5: warning: duplicate parameter name 'x' in 'f'
# CHECK: define double @f(double %x, double %x{{.*}})
# CHECK: ret double
