# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 5 control flow: if/then/else diamonds with phi nodes, and for loops.
# The REPL prints post-FPM IR, so opaque calls are used where the CFG must survive.

extern foo();
# CHECK: declare double @foo()
extern bar();
# CHECK: declare double @bar()

# Calls may have side effects, so SimplifyCFG keeps the diamond CFG and the phi.
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

# Constant arms get if-converted; the branch collapses to a select.
def testIfSelect(x) if x then 2 else 3;
# CHECK-LABEL: define double @testIfSelect(double %x)
# CHECK-NOT: br i1
# CHECK: select
# CHECK: ret double

extern putchard(c);
# CHECK: declare double @putchard(double)

# The loop variable is a phi in Chapter 5 (explicit SSA, pre-mem2reg).
def testFor(n) for i = 1, i < n in putchard(i);
# CHECK-LABEL: define double @testFor(double %n)
# CHECK: loop:
# CHECK: %i = phi double
# CHECK: call double @putchard(double %i
# CHECK: afterloop:
# CHECK: ret double 0.000000e+00

# Nested control flow: an if-diamond inside the loop body must keep both structures.
def testNested(n) for i = 1, i < n in (if i < 2 then foo() else bar());
# CHECK-LABEL: define double @testNested(double %n)
# CHECK: loop:
# CHECK: then:
# CHECK: call double @foo()
# CHECK: else:
# CHECK: call double @bar()
# CHECK: ifcont:
# CHECK: afterloop:
