# RUN: %toy %s -emit=ir | FileCheck %s
# Unoptimized IR: instruction structure is exactly what codegen emits.
# Note underscores are legal in identifiers in v2.

def foo_bar(a b) a*a + 2*a*b + b*b;
# CHECK-LABEL: define double @foo_bar(double %a, double %b)
# CHECK: entry:
# Arguments are spilled to allocas (mem2reg only runs with -opt).
# CHECK: %a1 = alloca double
# CHECK: store double %a
# CHECK: fmul double
# CHECK: ret double

def cmp(x y) x < y;
# CHECK-LABEL: define double @cmp(double %x, double %y)
# CHECK: [[CMP:%.*]] = fcmp ult double
# CHECK-NEXT: [[BOOL:%.*]] = uitofp i1 [[CMP]] to double
# CHECK-NEXT: ret double [[BOOL]]

extern cos(x);
# CHECK: declare double @cos(double)

cos(1.234);
# CHECK-LABEL: define double @__anon_expr0()
# CHECK: [[RES:%.*]] = call double @cos(double 1.234000e+00)
# CHECK-NEXT: ret double [[RES]]

# Multiple top-level expressions coexist in one module with unique names.
cos(0.0);
# CHECK-LABEL: define double @__anon_expr1()
