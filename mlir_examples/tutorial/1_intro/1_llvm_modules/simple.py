import ctypes

module = ctypes.CDLL("./build/libsimple.so")

module.main.argtypes = []
module.main.restype = ctypes.c_int

print(module.main())
