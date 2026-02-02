cmake_minimum_required(VERSION 3.15)
project(MyMLIRProject)

#cmake -G Ninja .. \
#  -DMLIR_DIR=~/llvm-project/build/lib/cmake/mlir \
#  -DLLVM_DIR=~/llvm-project/build/lib/cmake/llvm
# Add this BEFORE find_package
set(MLIR_DIR "$ENV{HOME}/compiler/llvm-project/build/lib/cmake/mlir" CACHE PATH "Path to MLIR config")
set(LLVM_DIR "$ENV{HOME}/compiler/llvm-project/build/lib/cmake/llvm" CACHE PATH "Path to LLVM config")

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