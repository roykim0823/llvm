# For a better template to copy, see examples/standalone
cmake_minimum_required(VERSION 3.20)
# Remove the hardcoded deployment target for macOS, and set it conditionally based on the platform.
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "macOS Deployment Target" FORCE)
endif()
project(MyMLIRProject)

# 1. Find the packages FIRST
find_package(MLIR REQUIRED CONFIG)
find_package(LLVM REQUIRED CONFIG)

# 2. Add the MLIR/LLVM CMake paths so include() can find the files
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

# 3. Include the macros that define 'mlir_tablegen' and 'add_toy_chapter'
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

# 4. NOW you can enter the subdirectory because the commands are defined
include_directories(${MLIR_INCLUDE_DIRS}
                    ${LLVM_INCLUDE_DIRS})

# --------------------------------------------------------
# The original CMakeLists.txt had the following structure,
# Unnecessary statements have been commented out.
# --------------------------------------------------------

# This chapter depends on JIT support enabled.
if(NOT MLIR_ENABLE_EXECUTION_ENGINE)
  return()
endif()

# For a better template to copy, see examples/standalone
include_directories(include)
add_subdirectory(include)

set(LLVM_LINK_COMPONENTS
  Core
  Support
  nativecodegen
  OrcJIT
  )

set(LLVM_TARGET_DEFINITIONS mlir/ToyCombine.td)
mlir_tablegen(ToyCombine.inc -gen-rewriters)
add_public_tablegen_target(ToyCh7CombineIncGen)

add_executable(toyc-ch7
  toyc.cpp
  parser/AST.cpp
  mlir/MLIRGen.cpp
  mlir/Dialect.cpp
  mlir/LowerToAffineLoops.cpp
  mlir/LowerToLLVM.cpp
  mlir/ShapeInferencePass.cpp
  mlir/ToyCombine.cpp

  # DEPENDS
  # ToyCh7ShapeInferenceInterfaceIncGen
  # ToyCh7OpsIncGen
  # ToyCh7CombineIncGen
  )

add_dependencies(toyc-ch7 ToyCh7ShapeInferenceInterfaceIncGen)
add_dependencies(toyc-ch7 ToyCh7OpsIncGen)
add_dependencies(toyc-ch7 ToyCh7CombineIncGen)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include/)
get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
get_property(extension_libs GLOBAL PROPERTY MLIR_EXTENSION_LIBS)
target_link_libraries(toyc-ch7
  PRIVATE
    ${dialect_libs}
    ${conversion_libs}
    ${extension_libs}
    MLIRAnalysis
    MLIRBuiltinToLLVMIRTranslation
    MLIRCallInterfaces
    MLIRCastInterfaces
    MLIRExecutionEngine
    MLIRFunctionInterfaces
    MLIRIR
    MLIRLLVMCommonConversion
    MLIRLLVMToLLVMIRTranslation
    MLIRMemRefDialect
    MLIRParser
    MLIRPass
    MLIRSideEffectInterfaces
    MLIRTargetLLVMIRExport
    MLIRTransforms
    )
