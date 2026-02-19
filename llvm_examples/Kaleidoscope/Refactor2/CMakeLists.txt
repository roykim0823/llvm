cmake_minimum_required(VERSION 3.20)
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
)

# T2: Add test executable
FetchContent_MakeAvailable(googletest)

enable_testing()
# Add this immediately after the project() call
cmake_minimum_required(VERSION 3.20)
project(Kaleidoscope)

if(APPLE)
    # Force the compiler to use Homebrew's libc++ headers and library exclusively
    set(HOMEBREW_LLVM "/opt/homebrew/opt/llvm")

    # 1. Compiler Flags: Force the specific ABI and standard library
    add_compile_options("-stdlib=libc++")
    include_directories(BEFORE SYSTEM "${HOMEBREW_LLVM}/include/c++/v1")

    # 2. Linker Flags: Force the linker to use Homebrew's library files first
    add_link_options("-stdlib=libc++")
    add_link_options("-L${HOMEBREW_LLVM}/lib")
    add_link_options("-Wl,-rpath,${HOMEBREW_LLVM}/lib")

    # 3. Force these onto FetchContent (GoogleTest)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -I${HOMEBREW_LLVM}/include/c++/v1")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -L${HOMEBREW_LLVM}/lib -Wl,-rpath,${HOMEBREW_LLVM}/lib")
endif()

# ... find_package(LLVM) and other logic ...

add_executable(lexer_test
    test/lexer_test.cpp src/lexer.cpp
)
target_include_directories(lexer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lexer_test PRIVATE gtest_main ${llvm_libs})

# ADD THIS LINE:
# It ensures the linker uses the correct standard library and architecture flags
# set_target_properties(lexer_test PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS}")
# target_link_options(lexer_test PRIVATE ${LLVM_LDFLAGS})

# Discover tests automatically
include(GoogleTest)
gtest_discover_tests(lexer_test)
