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

include_directories(../src)  # For the header files in src

add_executable(Lexer_test
    Lexer_test.cpp ../src/lexer.cpp
)
target_include_directories(Lexer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Lexer_test PRIVATE gtest_main ${llvm_libs})

add_executable(Parser_test
    Parser_test.cpp ../src/lexer.cpp ../src/parser.cpp
)
target_include_directories(Parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Parser_test PRIVATE gtest_main ${llvm_libs})

add_executable(AST_test
    AST_test.cpp ../src/lexer.cpp ../src/parser.cpp ../src/Sema.cpp
)
target_include_directories(AST_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AST_test PRIVATE gtest_main ${llvm_libs})

add_executable(Sema_test
    Sema_test.cpp ../src/lexer.cpp ../src/parser.cpp ../src/Sema.cpp
)
target_include_directories(Sema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Sema_test PRIVATE gtest_main ${llvm_libs})

add_executable(CodeGen_test
    CodeGen_test.cpp ../src/lexer.cpp ../src/parser.cpp ../src/Sema.cpp ../src/CodeGen.cpp
)
target_include_directories(CodeGen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CodeGen_test PRIVATE gtest_main ${llvm_libs})


# Discover tests automatically
include(GoogleTest)
gtest_discover_tests(Lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(Parser_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(AST_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(Sema_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(CodeGen_test PROPERTIES NO_PRETTY_VALUES TRUE)
