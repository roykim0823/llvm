# RUN: %toy < %s 2>&1 | FileCheck %s
# End-to-end JIT behavior: top-level expressions compile to native code, run,
# and print "Evaluated to ..."; codegen errors are reported and the REPL keeps
# accepting input (same error-recovery contract as earlier chapters).

4+5;
# CHECK: Evaluated to 9.000000

# Cross-module call: by now double5 lives in an earlier, already-JIT'd module,
# so the call site below re-declares it via the functionProtos registry.
def double5(x) x*5;
double5(4);
# CHECK: Evaluated to 20.000000

# Extern resolved via dlsym into the running process (libm's sin).
extern sin(x);
sin(1.0);
# CHECK: Evaluated to 0.841471

# Chapter 5: control flow evaluates like any other expression.
if 1 then 42 else 0;
# CHECK: Evaluated to 42.000000

# A for expression always evaluates to 0.0.
for i = 1, i < 3 in 1;
# CHECK: Evaluated to 0.000000

# Codegen errors must not abort the driver.
def bad(x) y;
# CHECK: Error: Unknown variable name

unknownfn(1.0);
# CHECK: Error: Unknown function referenced

# Still alive after both errors.
2+3;
# CHECK: Evaluated to 5.000000
