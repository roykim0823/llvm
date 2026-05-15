"""AOT driver: load the vectorized array_add .dylib and call it.

The MLIR function is declared with `attributes {llvm.emit_c_interface}`,
so the lowered library exposes `_mlir_ciface_array_add` taking three
pointers to MemRef descriptor structs (input1, input2, output).
"""
import ctypes
import numpy as np
from np_memref import MemRefDescriptor, numpy_to_memref


def main():
  lib = ctypes.CDLL("./libarray_add_vec.dylib")

  array_add = lib._mlir_ciface_array_add
  array_add.argtypes = [ctypes.POINTER(MemRefDescriptor)] * 3
  array_add.restype = None

  size = 1024
  a = np.ones(size, dtype=np.float32)
  b = np.ones(size, dtype=np.float32) * 2
  c = np.zeros(size, dtype=np.float32)

  a_desc = numpy_to_memref(a)
  b_desc = numpy_to_memref(b)
  c_desc = numpy_to_memref(c)

  array_add(ctypes.byref(a_desc),
            ctypes.byref(b_desc),
            ctypes.byref(c_desc))

  np.testing.assert_array_almost_equal(c, a + b)
  print("Vectorized array_add successful!")
  print(f"First 8 elements: {c[:8]}")  # [3.0] * 8


if __name__ == "__main__":
  main()
