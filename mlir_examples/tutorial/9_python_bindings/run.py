#!/usr/bin/env python3
"""REFERENCE ONLY — executing the PTX needs an NVIDIA GPU (no macOS).

The runtime half of the mini compiler: take the PTX that compile.py produced,
load it with the CUDA *driver API* (via the cuda-python package), launch the
kernel over a 10x10 grid, and check the result against NumPy.

Note this file contains no MLIR at all — by the time we're here, MLIR's job is
done and we're a plain CUDA host program that happens to get its kernel from a
compiler pipeline instead of nvcc.

On a CUDA box:  pip install cuda-python numpy && python3 run.py
"""

import sys

import numpy as np

try:
    import cuda.cuda as cu
except ImportError:
    sys.exit("error: cuda-python not installed (and it needs an NVIDIA GPU anyway)")

from compile import compile_mlir_to_ptx

SIZE = 10


def check(result):
    """Unwrap a driver-API (err, value...) tuple, raising on failure."""
    err = result[0]
    if isinstance(err, cu.CUresult) and err != cu.CUresult.CUDA_SUCCESS:
        raise RuntimeError(f"CUDA error: {cu.cuGetErrorName(err)[1]}")
    return result[1] if len(result) == 2 else (None if len(result) == 1 else result[1:])


def main():
    # 1. Compile: MLIR (linalg on tensors) -> PTX, entirely at runtime.
    ptx = compile_mlir_to_ptx(open("square.mlir").read())[0]

    # 2. CUDA context on device 0.
    check(cu.cuInit(0))
    device = check(cu.cuDeviceGet(0))
    context = check(cu.cuCtxCreate(0, device))

    try:
        # 3. Host data + device buffers.
        input_data = np.arange(SIZE * SIZE, dtype=np.float32).reshape(SIZE, SIZE)
        output_data = np.zeros_like(input_data)
        d_input = check(cu.cuMemAlloc(input_data.nbytes))
        d_output = check(cu.cuMemAlloc(output_data.nbytes))
        check(cu.cuMemcpyHtoD(d_input, input_data.ctypes.data, input_data.nbytes))

        # 4. JIT the PTX and grab the kernel.
        module = check(cu.cuModuleLoadData(ptx.encode()))
        kernel = check(cu.cuModuleGetFunction(module, b"square_kernel"))

        # 5. Launch. The kernel signature comes from the lowered gpu.launch_func:
        #    args(step=1 : i64, lower-bound=0 : i64, input ptr, output ptr),
        #    one block per element: blocks (10,10,1) x threads (1,1,1).
        import ctypes
        args = [1, 0, d_input, d_output]
        arg_types = [ctypes.c_longlong, ctypes.c_longlong, None, None]
        check(cu.cuLaunchKernel(kernel, SIZE, SIZE, 1, 1, 1, 1,
                                0, 0, (tuple(args), tuple(arg_types)), 0))
        check(cu.cuCtxSynchronize())

        # 6. Copy back and verify against NumPy.
        check(cu.cuMemcpyDtoH(output_data.ctypes.data, d_output, output_data.nbytes))
        np.testing.assert_allclose(output_data, input_data * input_data, rtol=1e-5)
        print("Success! GPU result matches NumPy.")

        check(cu.cuMemFree(d_input))
        check(cu.cuMemFree(d_output))
        check(cu.cuModuleUnload(module))
    finally:
        check(cu.cuCtxDestroy(context))


if __name__ == "__main__":
    main()
