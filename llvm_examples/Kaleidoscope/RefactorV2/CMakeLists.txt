cmake_minimum_required(VERSION 3.20)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "macOS Deployment Target" FORCE)
endif()

project(KaleidoscopeV2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(llvm_libs core orcjit native)

# Core library: AST dumper + IR generation. Lexer and Parser are header-only.
add_library(toy_core STATIC
    src/AST.cpp
    src/CodeGen.cpp
)
target_include_directories(toy_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # for include/KaleidoscopeJIT.h
)
target_link_libraries(toy_core PUBLIC ${llvm_libs})

# The driver. extern_d.cpp (putchard/printd) is listed on the executable so
# the linker cannot drop it: the JIT resolves those symbols from the process.
add_executable(toyc src/toyc.cpp src/extern_d.cpp)
target_link_libraries(toyc PRIVATE toy_core)

# Unit tests (pinned GoogleTest release)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# The buffer-based lexer is testable from plain strings: no stdin
# redirection, no temp files, safe under ctest -j.
add_executable(lexer_test test/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE gtest_main toy_core)

include(GoogleTest)
gtest_discover_tests(lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)

# lit + FileCheck: everything end-to-end (AST dumps, IR shape, JIT results,
# error messages with locations) is expressed as .k files with RUN lines.
find_program(LIT_TOOL NAMES lit llvm-lit)
if(LIT_TOOL)
  add_test(NAME filecheck
           COMMAND ${LIT_TOOL} -v ${CMAKE_CURRENT_SOURCE_DIR}/test/filecheck)
  set_tests_properties(filecheck PROPERTIES
    ENVIRONMENT "TOY_BIN=$<TARGET_FILE:toyc>;LIT_EXEC_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lit")
endif()
