cmake_minimum_required(VERSION 3.20)

# Match the deployment target to the host macOS release so this project and
# the Homebrew LLVM libraries agree (avoids version-mismatch link warnings),
# and pin the SDK explicitly. CMake no longer fills CMAKE_OSX_SYSROOT in by
# itself for a non-Apple compiler; without it, the SDK's /usr/include (which
# the imported LLVM targets carry along, having found zlib/libedit/ffi there)
# reaches the compile line as -isystem ahead of Homebrew clang's own libc++
# headers, and libc++'s #include_next chain fails with "<cstdio> tried
# including <stdio.h> but didn't find libc++'s <stdio.h> header". With the
# sysroot set, CMake knows those paths are implicit and drops them.
if(APPLE)
  execute_process(COMMAND sw_vers -productVersion
                  OUTPUT_VARIABLE _macos_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${_macos_version}" CACHE STRING "macOS Deployment Target" FORCE)

  if(NOT CMAKE_OSX_SYSROOT)
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE _macos_sdk
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_macos_sdk)
      set(CMAKE_OSX_SYSROOT "${_macos_sdk}" CACHE STRING "macOS SDK" FORCE)
    endif()
  endif()
endif()

project(KaleidoscopeV3)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(llvm_libs core orcjit native)

# Core library: AST dumper, sema, and the decomposed backend (IRGen /
# Optimizer / DebugInfo behind the CodeGenSession facade). Lexer, Parser,
# and Diagnostics are header-only.
add_library(toy_core STATIC
    src/AST.cpp
    src/Sema.cpp
    src/IRGen.cpp
    src/Optimizer.cpp
    src/DebugInfo.cpp
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

# Sema + diagnostics are testable the same way: parse a string, run
# checkModule, inspect the collected diagnostics -- no LLVM IR involved.
add_executable(sema_test test/sema_test.cpp)
target_link_libraries(sema_test PRIVATE gtest_main toy_core)

include(GoogleTest)
gtest_discover_tests(lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(sema_test PROPERTIES NO_PRETTY_VALUES TRUE)

# lit + FileCheck: everything end-to-end (AST dumps, IR shape, JIT results,
# error messages with locations) is expressed as .k files with RUN lines.
find_program(LIT_TOOL NAMES lit llvm-lit)
if(LIT_TOOL)
  add_test(NAME filecheck
           COMMAND ${LIT_TOOL} -v ${CMAKE_CURRENT_SOURCE_DIR}/test/filecheck)
  set_tests_properties(filecheck PROPERTIES
    ENVIRONMENT "TOY_BIN=$<TARGET_FILE:toyc>;LIT_EXEC_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lit")
endif()
