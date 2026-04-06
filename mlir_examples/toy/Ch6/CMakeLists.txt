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

set(LLVM_TARGET_DEFINITIONS mlir/ToyCombine.td)
mlir_tablegen(ToyCombine.inc -gen-rewriters)
add_public_tablegen_target(ToyCh6CombineIncGen)

add_executable(toyc-ch6
  toyc.cpp
  parser/AST.cpp
  mlir/MLIRGen.cpp
  mlir/Dialect.cpp
  mlir/LowerToAffineLoops.cpp
  mlir/LowerToLLVM.cpp
  mlir/ShapeInferencePass.cpp
  mlir/ToyCombine.cpp
  )

add_dependencies(toyc-ch6 ToyCh6ShapeInferenceInterfaceIncGen)
add_dependencies(toyc-ch6 ToyCh6OpsIncGen)
add_dependencies(toyc-ch6 ToyCh6CombineIncGen)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include/)

target_link_libraries(toyc-ch6
  PRIVATE
    MLIR                         # libMLIR.dylib (all dialects, passes, conversions)
    MLIRExecutionEngineShared    # libMLIRExecutionEngineShared.dylib (JIT support)
    )
