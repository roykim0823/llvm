import numpy as np
from ctypes import c_void_p, c_longlong, Structure


class MemRefDescriptor(Structure):
  """ctypes layout matching MLIR's 1-D memref descriptor.

  The fields here MUST match the struct produced by
  `-finalize-memref-to-llvm` for a memref<Nxf32>:
      { ptr, ptr, i64, [1 x i64], [1 x i64] }
  """
  _fields_ = [
    ("allocated", c_void_p),    # base pointer (for free())
    ("aligned",   c_void_p),    # aligned data pointer (used for access)
    ("offset",    c_longlong),  # offset into data, in elements
    ("shape",     c_longlong * 1),
    ("stride",    c_longlong * 1),
  ]


def numpy_to_memref(arr):
  """Wrap a 1-D contiguous NumPy array in a MemRefDescriptor."""
  if not arr.flags["C_CONTIGUOUS"]:
    arr = np.ascontiguousarray(arr)

  desc = MemRefDescriptor()
  desc.allocated = arr.ctypes.data_as(c_void_p)
  desc.aligned   = desc.allocated
  desc.offset    = 0
  desc.shape[0]  = arr.shape[0]
  desc.stride[0] = 1
  return desc
