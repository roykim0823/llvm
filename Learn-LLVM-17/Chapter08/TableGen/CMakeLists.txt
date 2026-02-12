cmake_minimum_required (VERSION 3.20.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)  # macOS system installed llvm is 21.1.8, Ubuntu installed llvm 17
  set(CMAKE_C_COMPILER "/Users/roy/llvm-project/llvm17/bin/clang")
  set(CMAKE_CXX_COMPILER "/Users/roy/llvm-project/llvm17/bin/clang++")
endif()

project ("tablegen")

if(APPLE)
  # Add this BEFORE find_package
  set(LLVM_DIR "/Users/roy/llvm-project/llvm17/lib/cmake/llvm" CACHE PATH "Path to LLVM config")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Match local LLVM target" FORCE)
endif()

# Locate LLVM
find_package(LLVM REQUIRED CONFIG)
message("Found LLVM ${LLVM_PACKAGE_VERSION}, build type ${LLVM_BUILD_TYPE}")
list(APPEND CMAKE_MODULE_PATH ${LLVM_DIR})

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
llvm_map_components_to_libnames(llvm_libs tablegen)

include(DetermineGCCCompatible)
if(MSVC)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" build)
  string(REGEX REPLACE "^[-/]?M([DT])(d?)$"
         "\\2\\1" CMAKE_MSVC_RUNTIME_LIBRARY
         ${LLVM_USE_CRT_${build}})
  string(REPLACE "T"
         "" CMAKE_MSVC_RUNTIME_LIBRARY
         ${CMAKE_MSVC_RUNTIME_LIBRARY})
  string(REPLACE "D"
         "DLL" CMAKE_MSVC_RUNTIME_LIBRARY
         ${CMAKE_MSVC_RUNTIME_LIBRARY})
  string(REPLACE "d"
         "Debug" CMAKE_MSVC_RUNTIME_LIBRARY
         ${CMAKE_MSVC_RUNTIME_LIBRARY})
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded${CMAKE_MSVC_RUNTIME_LIBRARY}")
  message("CMAKE_MSVC_RUNTIME_LIBRARY: ${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

if(LLVM_ENABLE_EH)
  if(MSVC)
    list(APPEND LLVM_COMPILE_FLAGS "/EHsc")
  endif()
else()
  if(LLVM_COMPILER_IS_GCC_COMPATIBLE)
    list(APPEND LLVM_COMPILE_FLAGS "-fno-exceptions")
    if(NOT LLVM_ENABLE_UNWIND_TABLES)
      list(APPEND LLVM_COMPILE_FLAGS "-fno-unwind-tables")
      list(APPEND LLVM_COMPILE_FLAGS "-fno-asynchronous-unwind-tables")
    endif()
  elseif(MSVC)
    list(APPEND LLVM_COMPILE_DEFINITIONS _HAS_EXCEPTIONS=0)
    list(APPEND LLVM_COMPILE_FLAGS "/EHs-c-")
  endif()
endif()

if(NOT LLVM_ENABLE_RTTI)
  if (LLVM_COMPILER_IS_GCC_COMPATIBLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
  elseif (MSVC)
    list(APPEND LLVM_COMPILE_FLAGS "/GR-")
  endif()
elseif(MSVC)
  list(APPEND LLVM_COMPILE_FLAGS "/GR")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")

add_executable (tinylang-tblgen
  TableGen.cpp
  TokenEmitter.cpp
  )
target_link_libraries(tinylang-tblgen PRIVATE ${llvm_libs})
