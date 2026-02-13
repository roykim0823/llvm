cmake_minimum_required(VERSION 3.15)
project(LLVMToy)

# 1. Find the LLVM package
find_package(LLVM REQUIRED CONFIG)

# 2. Add the LLVM headers and definitions
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# 3. Create the executable
add_executable(toy toy.cpp)

# 4. Map the components to libraries
# The 'core' in your command corresponds to the LLVMCore component
llvm_map_components_to_libnames(llvm_libs orcjit native)

# 5. Link the libraries
target_link_libraries(toy PRIVATE ${llvm_libs})

# Export symbols for JIT visibility (equivalent to -rdynamic)
set_target_properties(toy PROPERTIES ENABLE_EXPORTS TRUE)

# Optional: Match the C++ standard required by your LLVM version
set_target_properties(toy PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)
