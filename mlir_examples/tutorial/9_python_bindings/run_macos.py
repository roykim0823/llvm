#!/usr/bin/env python3
"""The Apple Silicon backend — this one RUNS on this Mac, end to end.

Same square.mlir, same bindings, different target: instead of lowering the
linalg op toward NVVM/PTX (compile.py), lower it to the *CPU* and execute it
right here with MLIR's in-process JIT (mlir.execution_engine) — no CLI tools,
no GPU, no files. NumPy arrays go in, squared NumPy arrays come out.

    python3 run_macos.py        (needs the MLIR Python bindings, see README.md)
"""

import ctypes
import sys

import numpy as np

try:
    from mlir.ir import Context, Module, UnitAttr
    from mlir.passmanager import PassManager
    from mlir.execution_engine import ExecutionEngine
    from mlir.runtime import (
        get_ranked_memref_descriptor,
        make_nd_memref_descriptor,
        ranked_memref_to_numpy,
    )
except ImportError:
    sys.exit(
        "error: MLIR Python bindings not found (see README.md, 'Installing the bindings')"
    )

# The CPU pipeline: identical front half to compile.py (bufferize, then to
# loops — sequential this time), then straight down the Chapter 1/2 CPU
# lowering instead of the GPU branch.
CPU_PIPELINE = [
    "canonicalize",
    "one-shot-bufferize{bufferize-function-boundaries"
    " function-boundary-type-conversion=identity-layout-map}",
    "canonicalize",
    "convert-linalg-to-loops",
    "convert-scf-to-cf",
    "convert-cf-to-llvm",
    "convert-arith-to-llvm",
    "finalize-memref-to-llvm",
    "convert-func-to-llvm",
    "reconcile-unrealized-casts",
]

SIZE = 10


def main():
    with Context():
        module = Module.parse(open("square.mlir").read())

        # Ask for a C-callable wrapper for @square — same trick as writing
        # `attributes {llvm.emit_c_interface}` in the source, but done by
        # editing the IR from Python.
        square_func = module.body.operations[0]
        square_func.attributes["llvm.emit_c_interface"] = UnitAttr.get()

        pm = PassManager()
        for p in CPU_PIPELINE:
            pm.add(p)
        pm.run(module.operation)

        engine = ExecutionEngine(module, opt_level=2)

        # NumPy arrays cross the boundary as ranked memref descriptors. The
        # function *returns* a memref, and in the C interface the result slot
        # is passed as the first pointer argument.
        a = np.arange(SIZE * SIZE, dtype=np.float32).reshape(SIZE, SIZE)
        out = np.zeros_like(a)
        result = make_nd_memref_descriptor(2, ctypes.c_float)()

        engine.invoke(
            "square",
            ctypes.pointer(ctypes.pointer(result)),
            ctypes.pointer(ctypes.pointer(get_ranked_memref_descriptor(a))),
            ctypes.pointer(ctypes.pointer(get_ranked_memref_descriptor(out))),
        )

        returned = ranked_memref_to_numpy(ctypes.pointer(result))
        np.testing.assert_allclose(returned, a * a, rtol=1e-6)
        np.testing.assert_allclose(out, a * a, rtol=1e-6)  # written in place too
        print("Success! JIT-compiled MLIR ran on this machine's CPU:")
        print(f"  input[3,3]={a[3,3]:.0f}  ->  output[3,3]={returned[3,3]:.0f}")


if __name__ == "__main__":
    main()
