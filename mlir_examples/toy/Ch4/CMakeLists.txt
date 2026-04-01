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
include(AddLLVM)
include(AddMLIR)
include(TableGen)  # Add TableGen module to use mlir_tablegen command

# 4. NOW you can enter the subdirectory because the commands are defined
include_directories(${MLIR_INCLUDE_DIRS}
                    ${LLVM_INCLUDE_DIRS})

# --------------------------------------------------------
# The original CMakeLists.txt had the following structure,
# Unnecessary statements have been commented out.
# --------------------------------------------------------

include_directories(include)
add_subdirectory(include)

set(LLVM_LINK_COMPONENTS
  Support
  )

set(LLVM_TARGET_DEFINITIONS mlir/ToyCombine.td)
mlir_tablegen(ToyCombine.inc -gen-rewriters)
add_public_tablegen_target(ToyCh4CombineIncGen)

add_executable(toyc-ch4
  toyc.cpp
  parser/AST.cpp
  mlir/MLIRGen.cpp
  mlir/Dialect.cpp
  mlir/ShapeInferencePass.cpp
  mlir/ToyCombine.cpp

  # DEPENDS
  # ToyCh4OpsIncGen
  # ToyCh4ShapeInferenceInterfaceIncGen
  # ToyCh4CombineIncGen
  )

add_dependencies(toyc-ch4 ToyCh4OpsIncGen)
add_dependencies(toyc-ch4 ToyCh4ShapeInferenceInterfaceIncGen)
add_dependencies(toyc-ch4 ToyCh4CombineIncGen)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include/)
target_link_libraries(toyc-ch4
  PRIVATE
    MLIRAnalysis
    MLIRCastInterfaces
    MLIRCallInterfaces
    MLIRFunctionInterfaces
    MLIRIR
    MLIRParser
    MLIRPass
    MLIRSideEffectInterfaces
    MLIRTransforms)
