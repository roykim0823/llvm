# RUN: %toy %s -emit=ir -opt | FileCheck %s
# Control flow structure after optimization: diamonds with phis survive when
# the arms have side effects; the loop variable's alloca is promoted.

extern foo();
# CHECK: declare double @foo()
extern bar();
# CHECK: declare double @bar()

def testIfCfg(x) if x then foo() else bar();
# CHECK-LABEL: define double @testIfCfg(double %x)
# CHECK: fcmp {{.*}} double %x, 0.000000e+00
# CHECK: then:
# CHECK: call double @foo()
# CHECK: else:
# CHECK: call double @bar()
# CHECK: ifcont:
# CHECK-NEXT: {{.*}} = phi double
# CHECK: ret double

# Constant arms get if-converted to a select.
def testIfSelect(x) if x then 2 else 3;
# CHECK-LABEL: define double @testIfSelect(double %x)
# CHECK-NOT: br i1
# CHECK: select
# CHECK: ret double

extern putchard(c);
# CHECK: declare double @putchard(double)

# The loop variable lives in an alloca; mem2reg must rebuild the phi.
def testFor(n) for i = 1, i < n in putchard(i);
# CHECK-LABEL: define double @testFor(double %n)
# CHECK-NOT: alloca
# CHECK: loop:
# CHECK: {{%.*}} = phi double
# CHECK: call double @putchard(double
# CHECK: afterloop:
# CHECK: ret double 0.000000e+00
