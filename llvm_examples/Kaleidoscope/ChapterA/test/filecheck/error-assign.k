# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# '1 = 2' was undefined behavior in the upstream tutorial (an unchecked
# static_cast). With AST kind tags and dyn_cast it is a clean, located
# error -- reported by sema before any IR exists.

1 = 2;
# CHECK: error-assign.k:6:3: error: destination of '=' must be a variable
# CHECK-NOT: Assert
# CHECK: 1 error generated.
