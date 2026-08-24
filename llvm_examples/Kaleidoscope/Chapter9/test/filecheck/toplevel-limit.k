# RUN: %toy < %s 2>&1 | FileCheck %s
# Chapter 9 supports only ONE top-level command per program: every top-level
# expression becomes main(). There is no redefinition guard (dropped in
# Chapter4, faithful to upstream), so a second expression is not rejected --
# its body lands in the existing main() as a dead extra block. This test pins
# that quirk so any change in the behavior is noticed.

1 + 2;
3 + 4;

# CHECK: define double @main()
# CHECK: entry:
# CHECK-NEXT: ret double 3.000000e+00
# CHECK: entry1:
# CHECK-NEXT: ret double 7.000000e+00
