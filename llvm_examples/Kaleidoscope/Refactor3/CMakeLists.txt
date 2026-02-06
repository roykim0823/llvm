cmake_minimum_required(VERSION 3.20)

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


project(Kaleidoscope)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find the LLVM package
# If your LLVM is in a custom path, run cmake with:
# -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
find_package(LLVM REQUIRED CONFIG)

# 2. Add the LLVM headers and definitions
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})


# 4. Map the components to libraries
# The 'core' in your command corresponds to the LLVMCore component
llvm_map_components_to_libnames(llvm_libs core orcjit native)

# Define source files with their new paths
set(SOURCES
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/log.cpp
)

add_executable(toy ${SOURCES})

# This is the magic line: it adds 'include/' to the header search path
target_include_directories(toy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(toy PRIVATE ${llvm_libs})