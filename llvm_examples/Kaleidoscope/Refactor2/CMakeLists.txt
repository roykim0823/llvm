cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0")
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
llvm_map_components_to_libnames(llvm_libs core)

# Define source files with their new paths
set(SOURCES
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/log.cpp
)

add_executable(toy ${SOURCES})

# This is the magic line: it adds 'include/' to the header search path
target_include_directories(toy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(toy PRIVATE ${llvm_libs})

# Unit tests
# T1: Fetch and Enable GoogleTest
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE  # Add this line
)

# T2: Add test executable
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(lexer_test
    test/lexer_test.cpp src/lexer.cpp src/log.cpp
)
target_include_directories(lexer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lexer_test PRIVATE gtest_main ${llvm_libs})

add_executable(parser_test
    test/parser_test.cpp src/parser.cpp src/lexer.cpp src/log.cpp
)
target_include_directories(parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(parser_test PRIVATE gtest_main ${llvm_libs})

# Discover tests automatically
include(GoogleTest)
gtest_discover_tests(lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(parser_test PROPERTIES NO_PRETTY_VALUES TRUE)
