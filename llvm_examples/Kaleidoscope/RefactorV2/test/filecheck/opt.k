# RUN: %toy %s -emit=ir -opt | FileCheck %s
# With -opt the per-function pipeline runs (mem2reg, InstCombine,
# Reassociate, GVN, SimplifyCFG).

# GVN's commutativity-aware value numbering deduplicates x*y + x*y.
def test_cse(x y) x*y + x*y;
# CHECK-LABEL: define double @test_cse(double %x, double %y)
# CHECK: [[MUL:%.*]] = fmul double %x, %y
# CHECK-NEXT: [[RES:%.*]] = fadd double [[MUL]], [[MUL]]
# CHECK-NEXT: ret double [[RES]]

# (x+5) + (5+x): the two commuted adds are recognized as equal.
def test_reassoc(x) (x+5) + (5+x);
# CHECK-LABEL: define double @test_reassoc(double %x)
# CHECK: [[ADD:%.*]] = fadd double %x, 5.000000e+00
# CHECK-NEXT: [[RES2:%.*]] = fadd double [[ADD]], [[ADD]]
# CHECK-NEXT: ret double [[RES2]]

# All-constant expressions fold to a single ret.
4+5;
# CHECK-LABEL: define double @__anon_expr0()
# CHECK-NOT: fadd
# CHECK: ret double 9.000000e+00
