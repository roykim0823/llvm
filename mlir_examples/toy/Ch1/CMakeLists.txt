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

# 4. NOW you can enter the subdirectory because the commands are defined
include_directories(${MLIR_INCLUDE_DIRS}
                    ${LLVM_INCLUDE_DIRS})

# --------------------------------------------------------
# The original CMakeLists.txt had the following structure,
# Unnecessary statements have been commented out.
# --------------------------------------------------------
add_executable(toyc-ch1
  toyc.cpp
  parser/AST.cpp
)

include_directories(include/)
target_link_libraries(toyc-ch1
  PRIVATE
    MLIRSupport
)