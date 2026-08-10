# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 6: user-defined operators. The key property is that defining an
# operator changes how *subsequent* input parses, which only an end-to-end
# test can observe.

def unary!(v) if v then 0 else 1;
# CHECK-LABEL: define double @"unary!"(double %v)

def binary| 5 (a b) if a then 1 else (if b then 1 else 0);
# CHECK-LABEL: define double @"binary|"(double %a, double %b)

# '|' was defined with precedence 5, looser than '+' (20):
# a | b + c must parse as a | (b + c), so the fadd feeds the operator call.
def testPrec(a b c) a | b + c;
# CHECK-LABEL: define double @testPrec(double %a, double %b, double %c)
# CHECK: [[SUM:%.*]] = fadd double %b, %c
# CHECK: call double @"binary|"(double %a, double [[SUM]])

# Unary operators compile to a call of the "unary<op>" function.
def testUnary(x) !x;
# CHECK-LABEL: define double @testUnary(double %x)
# CHECK: call double @"unary!"(double %x)

# A high-precedence operator binds tighter than '+':
# a @ b + c must parse as (a @ b) + c.
def binary@ 50 (a b) a * b;
# CHECK-LABEL: define double @"binary@"(double %a, double %b)
def testPrecTight(a b c) a @ b + c;
# CHECK-LABEL: define double @testPrecTight(double %a, double %b, double %c)
# CHECK: [[APPLY:%.*]] = call double @"binary@"(double %a, double %b)
# The optimizer may commute the operands, so only require that the call
# result feeds the fadd (i.e. '@' bound tighter than '+').
# CHECK: fadd double {{.*}}[[APPLY]]
