# RUN: %toy %s -emit=obj -o %t.o | FileCheck %s
# RUN: test -f %t.o

def average(x y) (x + y) * 0.5;
# CHECK: Wrote {{.*}}.o
