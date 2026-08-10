# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 4 runs the FPM (InstCombine, Reassociate, GVN, SimplifyCFG) on every
# function, so these tests assert on the *optimized* IR the REPL prints.

# GVN's commutativity-aware value numbering deduplicates x*y + x*y into one fmul.
def testCse(x y) x*y + x*y;
# CHECK-LABEL: define double @testCse(double %x, double %y)
# CHECK: [[MUL:%.*]] = fmul double %x, %y
# CHECK-NEXT: [[RES:%.*]] = fadd double [[MUL]], [[MUL]]
# CHECK-NEXT: ret double [[RES]]

# (x+5) + (5+x): GVN recognizes the two commuted adds as equal.
def testReassoc(x) (x+5) + (5+x);
# CHECK-LABEL: define double @testReassoc(double %x)
# CHECK: [[ADD:%.*]] = fadd double %x, 5.000000e+00
# CHECK-NEXT: [[RES2:%.*]] = fadd double [[ADD]], [[ADD]]
# CHECK-NEXT: ret double [[RES2]]

# (1+2+x) + (x+(1+2)): constants fold to 3.0, then the commuted adds dedup.
def testCombined(x) (1+2+x) + (x+(1+2));
# CHECK-LABEL: define double @testCombined(double %x)
# CHECK: [[ADD3:%.*]] = fadd double {{.*}}3.000000e+00
# CHECK-NEXT: [[RES3:%.*]] = fadd double [[ADD3]], [[ADD3]]
# CHECK-NEXT: ret double [[RES3]]

# All-constant expressions must fold to a single ret.
4+5;
# CHECK-LABEL: define double @__anon_expr()
# CHECK-NOT: fadd
# CHECK: ret double 9.000000e+00
