# For a better template to copy, see examples/standalone
cmake_minimum_required(VERSION 3.15)
# MUST come before project() to help find the right system paths
# Set the macOS SDK path before project() initializes the toolchain
if(APPLE)
    # Automatically get the path to the current macOS SDK
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE MACOS_SDK_PATH
                    OUTPUT_STRIP_TRAILING_WHITESPACE)

    # Force CMake to use this SDK path for system headers
    set(CMAKE_OSX_SYSROOT "${MACOS_SDK_PATH}" CACHE PATH "macOS SDK" FORCE)
endif()

project(toy-ch2)

# 1. Find the packages FIRST
find_package(MLIR REQUIRED CONFIG)
find_package(LLVM REQUIRED CONFIG)

# 2. Add the MLIR/LLVM CMake paths so include() can find the files
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

# 3. Include the macros that define 'mlir_tablegen' and 'add_toy_chapter'
include(AddLLVM)
include(AddMLIR)
include(TableGen)

# 4. NOW you can enter the subdirectory because the commands are defined

include_directories(${MLIR_INCLUDE_DIRS}
                    ${LLVM_INCLUDE_DIRS}
                    include/)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)
add_definitions(${LLVM_DEFINITIONS})

add_subdirectory(include)

set(LLVM_LINK_COMPONENTS
  Support
  )

add_executable(toyc-ch2
  toyc.cpp
  parser/AST.cpp
  mlir/MLIRGen.cpp
  mlir/Dialect.cpp

  # DEPENDS
  # ToyCh2OpsIncGen

  )
add_dependencies(toyc-ch2 ToyCh2OpsIncGen)
# Crucial: Apply LLVM's flags (like -fno-rtti) to your target
llvm_update_compile_flags(toyc-ch2)

#include_directories(include/)
#include_directories(${CMAKE_CURRENT_BINARY_DIR}/include/)
target_link_libraries(toyc-ch2
  PRIVATE
    MLIRAnalysis
    MLIRFunctionInterfaces
    MLIRIR
    MLIRParser
    MLIRSideEffectInterfaces
    MLIRTransforms)