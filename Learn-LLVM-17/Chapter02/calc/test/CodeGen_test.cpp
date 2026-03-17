#include "Lexer.h"
#include "Parser.h"
#include "Sema.h"
#include "CodeGen.h"
#include "AST.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

// Structure to hold our CodeGen test cases
struct CodeGenTestCase {
    std::string input;
    // We expect the generated IR to contain all of these substrings
    std::vector<std::string> expectedIRSubstrings;
};

class CodeGenParamTest : public ::testing::TestWithParam<CodeGenTestCase> {};

TEST_P(CodeGenParamTest, GeneratesCorrectLLVMIR) {
    const auto& param = GetParam();

    // 1. Setup Lexer and Parser
    llvm::StringRef buffer(param.input);
    Lexer lexer(buffer);
    Parser parser(lexer);

    // Using std::unique_ptr based on our previous AST updates
    std::unique_ptr<AST> tree = parser.parse();

    // Ensure parsing succeeded before proceeding
    ASSERT_FALSE(parser.hasError()) << "Parser failed on input: " << param.input;
    ASSERT_NE(tree, nullptr);

    // 2. Semantic Analysis
    Sema sema;
    ASSERT_FALSE(sema.semantic(tree.get())) << "Sema failed on input: " << param.input;

    // 3. Capture Standard Output and Run CodeGen
    testing::internal::CaptureStdout(); // Start capturing stdout

    CodeGen codegen;
    codegen.compile(tree.get());

    std::string generatedIR = testing::internal::GetCapturedStdout(); // Retrieve captured IR

    // 4. Assert Expected IR Instructions
    for (const auto& expected : param.expectedIRSubstrings) {
        EXPECT_TRUE(generatedIR.find(expected) != std::string::npos)
            << "Failed for input: '" << param.input << "'\n"
            << "Expected to find: '" << expected << "'\n"
            << "But generated IR was:\n" << generatedIR;
    }
}

// Define the test suite with various expressions
INSTANTIATE_TEST_SUITE_P(
    IRGeneration,
    CodeGenParamTest,
    ::testing::Values(
        // Test basic addition (IRBuilder folds 10 + 20 into 30)
        CodeGenTestCase{
            "10 + 20",
            {
                "define i32 @main",
                "call void @calc_write(i32 30)",
                "ret i32 0"
            }
        },

        // Test subtraction and multiplication (IRBuilder folds to 30)
        CodeGenTestCase{
            "50 - 10 * 2",
            {
                "call void @calc_write(i32 30)"
            }
        },

        // Test division (IRBuilder folds to 20)
        CodeGenTestCase{
            "100 / 5",
            {
                "call void @calc_write(i32 20)"
            }
        },

        // Test variables: IRBuilder CANNOT fold this, so 'add' is emitted
        CodeGenTestCase{
            "with x : x + 5",
            {
                "define i32 @main",
                "@x.str = private constant [2 x i8] c\"x\\00\"",
                "call i32 @calc_read(ptr",
                "add nsw i32",
                "call void @calc_write(i32"
            }
        },

        // Test multiple variables: IRBuilder CANNOT fold this, so 'mul' is emitted
        CodeGenTestCase{
            "with a, b : a * b",
            {
                "@a.str = private constant [2 x i8] c\"a\\00\"",
                "@b.str = private constant [2 x i8] c\"b\\00\"",
                "call i32 @calc_read(ptr",
                "mul nsw i32"
            }
        }
    )
);