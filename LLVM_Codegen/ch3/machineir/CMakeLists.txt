set(CMAKE_CXX_STANDARD 17)

cmake_minimum_required(VERSION 3.22)

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

project(CH3
        LANGUAGES CXX C)

include(../../cmake/utils/set-llvm-install-prefix.cmake)

set(CURR_TARGET
  build_mir
)

set(SRC
  "main.cpp"
  "solution/populate_function.cpp"
  "your_turn/populate_function.cpp"
)

set(INC
)


# Now build our tools
add_executable(${CURR_TARGET} ${SRC} ${INC})

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs support core AllTargetsCodeGens)

# Link against LLVM libraries
target_link_libraries(${CURR_TARGET} ${llvm_libs})
