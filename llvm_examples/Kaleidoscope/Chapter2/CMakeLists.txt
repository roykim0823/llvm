cmake_minimum_required(VERSION 3.20)

project(Kaleidoscope)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No LLVM here: like upstream Chapter 2, this directory is a pure C++
# frontend (lexer + parser + AST). LLVM enters in Chapter3 with codegen.
# (FileCheck for the lit tests still comes from `llvm-config --bindir`
# via test/filecheck/lit.cfg — that's test tooling, not a link dependency.)

# Shared core library so toy and the test binaries compile each source once
add_library(toy_core STATIC
    src/lexer.cpp
    src/parser.cpp
    src/log.cpp
)
target_include_directories(toy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(toy src/main.cpp)
target_link_libraries(toy PRIVATE toy_core)

# Unit tests
# T1: Fetch and Enable GoogleTest (pinned release, not a moving branch)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# T2: Add test executable
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(lexer_test test/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE gtest_main toy_core)

add_executable(parser_test test/parser_test.cpp)
target_link_libraries(parser_test PRIVATE gtest_main toy_core)

# Discover tests automatically
include(GoogleTest)
gtest_discover_tests(lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(parser_test PROPERTIES NO_PRETTY_VALUES TRUE)

# lit + FileCheck tests (end-to-end: feed a .k file to toy, check its output)
find_program(LIT_TOOL NAMES lit llvm-lit)
if(LIT_TOOL)
  add_test(NAME filecheck
           COMMAND ${LIT_TOOL} -v ${CMAKE_CURRENT_SOURCE_DIR}/test/filecheck)
  set_tests_properties(filecheck PROPERTIES
    ENVIRONMENT "TOY_BIN=$<TARGET_FILE:toy>;LIT_EXEC_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lit")
endif()
