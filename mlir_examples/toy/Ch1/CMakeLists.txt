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

project(MyMLIRProject)

set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")

#cmake -G Ninja .. \
#  -DMLIR_DIR=~/llvm-project/build/lib/cmake/mlir \
#  -DLLVM_DIR=~/llvm-project/build/lib/cmake/llvm
# Add this BEFORE find_package

if(NOT APPLE)
  # Use locally compiler MLIR
  set(MLIR_DIR "$ENV{HOME}/compiler/llvm-project/build/lib/cmake/mlir" CACHE PATH "Path to MLIR config")
  set(LLVM_DIR "$ENV{HOME}/compiler/llvm-project/build/lib/cmake/llvm" CACHE PATH "Path to LLVM config")
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

# 1. Find the MLIR package
find_package(MLIR REQUIRED CONFIG)

# Load LLVM-specific macros
include(AddLLVM)

# 2. Add MLIR's include directories and definitions
include_directories(${MLIR_INCLUDE_DIRS}
                    ${LLVM_INCLUDE_DIRS}
                    include/)
add_definitions(${LLVM_DEFINITIONS})

add_executable(toyc-ch1
  toyc.cpp
  parser/AST.cpp
)

# Crucial: Apply LLVM's flags (like -fno-rtti) to your target
llvm_update_compile_flags(toyc-ch1)

# 3. Link against MLIRSupport
target_link_libraries(toyc-ch1 PRIVATE
    MLIRSupport
    LLVMSupport
)