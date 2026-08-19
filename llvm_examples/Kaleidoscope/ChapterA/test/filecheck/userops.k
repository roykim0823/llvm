# RUN: %toy %s -emit=ir -opt | FileCheck %s
# User-defined operators: defining one changes how subsequent input parses.
# In v2 the PARSER registers the precedence (at prototype parse time), so
# grammar never depends on codegen.

def unary!(v) if v then 0 else 1;
# CHECK-LABEL: define double @"unary!"(double %v)

def binary| 5 (a b) if a then 1 else (if b then 1 else 0);
# CHECK-LABEL: define double @"binary|"(double %a, double %b)

# '|' (prec 5) binds looser than '+' (prec 20): a | b + c == a | (b + c).
def testPrec(a b c) a | b + c;
# CHECK-LABEL: define double @testPrec(double %a, double %b, double %c)
# CHECK: [[SUM:%.*]] = fadd double %b, %c
# CHECK: call double @"binary|"(double %a, double [[SUM]])

def testUnary(x) !x;
# CHECK-LABEL: define double @testUnary(double %x)
# CHECK: call double @"unary!"(double %x)

# A high-precedence operator binds tighter than '+': a @ b + c == (a @ b) + c.
def binary@ 50 (a b) a * b;
# CHECK-LABEL: define double @"binary@"(double %a, double %b)
def testPrecTight(a b c) a @ b + c;
# CHECK-LABEL: define double @testPrecTight(double %a, double %b, double %c)
# CHECK: [[APPLY:%.*]] = call double @"binary@"(double %a, double %b)
# CHECK: fadd double {{.*}}[[APPLY]]

# An operator can use itself recursively in its own body: the parser
# registered '&' before parsing the body.
def binary& 6 (a b) if a then (if b then 1 else 0) else 0 & 0;
# CHECK-LABEL: define double @"binary&"(double %a, double %b)
