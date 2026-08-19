cmake_minimum_required(VERSION 3.20)

# Match the deployment target to the host macOS release so this project and
# the Homebrew LLVM libraries agree (avoids version-mismatch link warnings).
if(APPLE)
  execute_process(COMMAND sw_vers -productVersion
                  OUTPUT_VARIABLE _macos_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${_macos_version}" CACHE STRING "macOS Deployment Target" FORCE)
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
llvm_map_components_to_libnames(llvm_libs core orcjit native)

# Shared core library so toy and the test binaries compile each source once.
add_library(toy_core STATIC
    src/lexer.cpp
    src/parser.cpp
    src/codegen.cpp
    src/log.cpp
)
target_include_directories(toy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(toy_core PUBLIC ${llvm_libs})

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

add_executable(codegen_test test/codegen_test.cpp)
target_link_libraries(codegen_test PRIVATE gtest_main toy_core)

# Discover tests automatically
include(GoogleTest)
gtest_discover_tests(lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(parser_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(codegen_test PROPERTIES NO_PRETTY_VALUES TRUE)

# lit + FileCheck tests (end-to-end: feed a .k file to toy, check the printed IR)
find_program(LIT_TOOL NAMES lit llvm-lit)
if(LIT_TOOL)
  add_test(NAME filecheck
           COMMAND ${LIT_TOOL} -v ${CMAKE_CURRENT_SOURCE_DIR}/test/filecheck)
  set_tests_properties(filecheck PROPERTIES
    ENVIRONMENT "TOY_BIN=$<TARGET_FILE:toy>;LIT_EXEC_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lit")
endif()
