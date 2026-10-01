# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 9 supports only ONE top-level command per program: every top-level
# expression becomes main(), and main() stays in the module (there is no JIT
# to run it and nothing erases it). A second expression is therefore a
# redefinition, which the guard from Chapter 8 refuses -- the first main()
# survives unchanged. (Upstream silently appends the second body to the
# existing main() as a dead block instead.)

1 + 2;
3 + 4;

# CHECK: Error: Function cannot be redefined.
# CHECK: define double @main()
# CHECK: entry:
# CHECK-NEXT: ret double 3.000000e+00
# CHECK-NOT: entry1:
# CHECK-NOT: ret double 7.000000e+00
