"""Python driver for tensor_ori.mlir.

Unlike aot_main.py, this version uses the "callee allocates, caller frees"
calling convention because tensor_ori.mlir *returns* a memref instead of
filling a caller-provided one.

MLIR's C interface for a function that returns a memref<?x?xi32>:
    void _mlir_ciface_identity(MemRef2D *sret_result, int64_t m, int64_t n);
where `sret_result` is an out-pointer the callee writes the descriptor into,
and we (the caller) must free `result.allocated` afterwards.
"""

import ctypes
import numpy as np
from ctypes import c_void_p, c_longlong, c_int64, Structure


class MemRef2D_i32(Structure):
  _fields_ = [
    ("allocated", c_void_p),
    ("aligned",   c_void_p),
    ("offset",    c_longlong),
    ("shape",     c_longlong * 2),
    ("stride",    c_longlong * 2),
  ]


# libc.free, for releasing the buffer the MLIR runtime malloc'd for us.
# On macOS the C runtime symbols live in libSystem.
libc = ctypes.CDLL("libSystem.dylib")
libc.free.argtypes = [c_void_p]
libc.free.restype = None


def main():
  lib = ctypes.CDLL("./build/tensor_ori.so")
  identity = lib._mlir_ciface_identity
  # sret pointer, then the two index args (i64 on 64-bit platforms)
  identity.argtypes = [ctypes.POINTER(MemRef2D_i32), c_int64, c_int64]
  identity.restype = None

  M, N = 5, 5
  result = MemRef2D_i32()           # zero-init descriptor; callee fills it in
  identity(ctypes.byref(result), M, N)

  # Reconstruct a numpy view from the aligned pointer + shape + stride.
  # Important: this view *aliases* the malloc'd buffer; if we free before
  # reading, we crash. So copy out, then free.
  shape  = (result.shape[0],  result.shape[1])
  stride = (result.stride[0] * 4, result.stride[1] * 4)  # i32 = 4 bytes
  arr_view = np.ctypeslib.as_array(
    ctypes.cast(result.aligned, ctypes.POINTER(ctypes.c_int32)),
    shape=shape,
  )
  # strides come back from MLIR in *element* units; numpy wants bytes.
  arr = np.lib.stride_tricks.as_strided(arr_view, shape=shape, strides=stride).copy()

  # Hand the buffer back to the C runtime.
  libc.free(result.allocated)

  expected = np.eye(M, N, dtype=np.int32)
  np.testing.assert_array_equal(arr, expected)
  print("Identity matrix generated successfully (returned-by-value variant)!")
  print(arr)


if __name__ == "__main__":
  main()
