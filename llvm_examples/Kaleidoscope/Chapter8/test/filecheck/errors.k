# RUN: %toy < %s 2>&1 | FileCheck %s
# With the JIT gone nothing evaluates, but the error-recovery contract from
# the earlier chapters still holds: codegen errors are reported, the driver
# keeps accepting input, and the accumulated module is still emitted at EOF.

def bad(x) y;
# CHECK: Error: Unknown variable name

unknownfn(1.0);
# CHECK: Error: Unknown function referenced

# Still alive after both errors — and the failed definitions must not block
# object emission.
def good(x) x;
# CHECK: define double @good(double %x)
# CHECK: Wrote output.o
