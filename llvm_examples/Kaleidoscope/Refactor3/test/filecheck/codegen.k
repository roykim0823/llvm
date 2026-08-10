# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 3 emits raw (unoptimized) IR, so instruction structure is stable.

def foo(a b) a*a + 2*a*b + b*b;
# CHECK-LABEL: define double @foo(double %a, double %b)
# CHECK: entry:
# CHECK: fmul double %a, %a
# CHECK: fmul double 2.000000e+00, %a
# CHECK-COUNT-2: fadd double
# CHECK: ret double

def cmp(x y) x < y;
# CHECK-LABEL: define double @cmp(double %x, double %y)
# CHECK: [[CMP:%.*]] = fcmp ult double %x, %y
# CHECK-NEXT: [[BOOL:%.*]] = uitofp i1 [[CMP]] to double
# CHECK-NEXT: ret double [[BOOL]]

extern cos(x);
# CHECK: declare double @cos(double)

cos(1.234);
# CHECK-LABEL: define double @__anon_expr()
# CHECK: [[RES:%.*]] = call double @cos(double 1.234000e+00)
# CHECK-NEXT: ret double [[RES]]
