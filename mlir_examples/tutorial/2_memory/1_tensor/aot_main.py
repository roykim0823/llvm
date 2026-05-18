import ctypes
import numpy as np
from ctypes import c_void_p, c_longlong, Structure


class MemRef2D_i32(Structure):
  """MLIR MemRef descriptor for a 2-D i32 memref<?x?xi32>."""
  _fields_ = [
    ("allocated", c_void_p),
    ("aligned",   c_void_p),
    ("offset",    c_longlong),
    ("shape",     c_longlong * 2),
    ("stride",    c_longlong * 2),
  ]


def numpy_to_memref_2d(arr: np.ndarray) -> MemRef2D_i32:
  if not arr.flags["C_CONTIGUOUS"]:
    arr = np.ascontiguousarray(arr)
  m, n = arr.shape
  desc = MemRef2D_i32()
  desc.allocated = arr.ctypes.data_as(c_void_p)
  desc.aligned   = desc.allocated
  desc.offset    = 0
  desc.shape[0], desc.shape[1] = m, n
  desc.stride[0], desc.stride[1] = n, 1  # row-major
  return desc


def main():
  lib = ctypes.CDLL("./build/tensor.so")
  identity = lib._mlir_ciface_identity
  identity.argtypes = [ctypes.POINTER(MemRef2D_i32)]
  identity.restype = None

  M, N = 5, 5
  out = np.zeros((M, N), dtype=np.int32)
  desc = numpy_to_memref_2d(out)
  identity(ctypes.byref(desc))

  expected = np.eye(M, N, dtype=np.int32)
  np.testing.assert_array_equal(out, expected)
  print("Identity matrix generated successfully!")
  print(out)


if __name__ == "__main__":
  main()
