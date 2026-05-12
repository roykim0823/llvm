# Unit tests
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

llvm_map_components_to_libnames(llvm_libs Support)

enable_testing()

add_executable(Diagnostic_test
    Diagnostic_test.cpp ../lib/Basic/Diagnostic.cpp
)
target_include_directories(Diagnostic_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Diagnostic_test PRIVATE gtest_main ${llvm_libs} tinylangBasic)

add_executable(TokenKinds_test
    TokenKinds_test.cpp ../lib/Basic/TokenKinds.cpp
)
target_include_directories(TokenKinds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(TokenKinds_test PRIVATE gtest_main ${llvm_libs} tinylangBasic)

add_executable(Lexer_test
    Lexer_test.cpp ../lib/Lexer/Lexer.cpp
)
target_include_directories(Lexer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Lexer_test PRIVATE gtest_main ${llvm_libs} tinylangLexer)

add_executable(Parser_test
    Parser_test.cpp ../lib/Parser/Parser.cpp ../lib/Sema/Sema.cpp
)
target_include_directories(Parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Parser_test PRIVATE gtest_main gmock ${llvm_libs} tinylangParser tinylangSema)

add_executable(Scope_test
    Scope_test.cpp ../lib/Sema/Scope.cpp
)
target_include_directories(Scope_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Scope_test PRIVATE gtest_main gmock ${llvm_libs})

add_executable(Sema_test
    Sema_test.cpp ../lib/Sema/Scope.cpp ../lib/Sema/Sema.cpp
)
target_include_directories(Sema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Sema_test PRIVATE gtest_main ${llvm_libs} tinylangSema)

llvm_map_components_to_libnames(llvm_codegen_libs Support Core IRReader Target AsmParser
    native)

add_executable(CGModule_test
    CGModule_test.cpp
)
target_include_directories(CGModule_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CGModule_test PRIVATE gtest_main ${llvm_codegen_libs} tinylangCodeGen tinylangSema)

add_executable(CGProcedure_test
    CGProcedure_test.cpp
)
target_include_directories(CGProcedure_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CGProcedure_test PRIVATE gtest_main ${llvm_codegen_libs} tinylangCodeGen tinylangSema)

add_executable(CodeGenerator_test
    CodeGenerator_test.cpp
)
target_include_directories(CodeGenerator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CodeGenerator_test PRIVATE gtest_main ${llvm_codegen_libs}
    tinylangCodeGen tinylangParser tinylangSema tinylangLexer tinylangBasic)

include(GoogleTest)
gtest_discover_tests(Diagnostic_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(TokenKinds_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(Lexer_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(Parser_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(Scope_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(Sema_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(CGModule_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(CGProcedure_test PROPERTIES NO_PRETTY_VALUES TRUE)
gtest_discover_tests(CodeGenerator_test PROPERTIES NO_PRETTY_VALUES TRUE)
