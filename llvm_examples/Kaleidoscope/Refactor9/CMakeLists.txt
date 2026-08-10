cmake_minimum_required(VERSION 3.20)

# Remove the hardcoded deployment target for macOS, and set it conditionally based on the platform.
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "macOS Deployment Target" FORCE)
endif()

project(Kaleidoscope)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find the LLVM package
find_package(LLVM REQUIRED CONFIG)

# 2. Add the LLVM headers and definitions
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# 3. Map the components to libraries
# The 'core' in your command corresponds to the LLVMCore component
llvm_map_components_to_libnames(llvm_libs core orcjit native)

# Define source files with their new paths
set(SOURCES
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/codegen.cpp
    src/log.cpp
    src/extern_d.cpp
    src/debug.cpp
)

add_executable(toy ${SOURCES})

# This is the magic line: it adds 'include/' to the header search path
target_include_directories(toy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(toy PRIVATE ${llvm_libs})
# lit + FileCheck tests (end-to-end: feed a .k file to toy, check the printed
# IR and debug-info metadata). Chapter 9 has no gtest suites: its observable
# behavior (DWARF metadata in the module dump) is text, which FileCheck matches directly.
enable_testing()
find_program(LIT_TOOL NAMES lit llvm-lit)
if(LIT_TOOL)
  add_test(NAME filecheck
           COMMAND ${LIT_TOOL} -v ${CMAKE_CURRENT_SOURCE_DIR}/test/filecheck)
  set_tests_properties(filecheck PROPERTIES
    ENVIRONMENT "TOY_BIN=$<TARGET_FILE:toy>;LIT_EXEC_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lit")
endif()
