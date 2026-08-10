# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# '1 = 2' was undefined behavior in the upstream tutorial (an unchecked
# static_cast). With AST kind tags and dyn_cast it is a clean, located error.

1 = 2;
# CHECK: Codegen error (5, 3): destination of '=' must be a variable
# CHECK-NOT: Assert
