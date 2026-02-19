#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <memory>
#include "parser.h"

using namespace toy;

struct ParserTestCase {
    std::string testName;
    std::string input;
    bool shouldPass;
};

class ParserParamTest : public ::testing::TestWithParam<ParserTestCase> {
protected:
    void SetUp() override {
        // Write input to a temporary file
        std::ofstream tmpFile("_parser_input.txt");
        tmpFile << GetParam().input;
        tmpFile.close();

        // Redirect stdin for the lexer
        ASSERT_TRUE(freopen("_parser_input.txt", "r", stdin) != nullptr);
    }

    void TearDown() override {
        std::remove("_parser_input.txt");
    }
};

template<typename T>
void verifyTest(bool shouldPass, std::unique_ptr<T> result, const std::string& input) {
    if (shouldPass) {
        EXPECT_NE(result, nullptr) << "Failed to parse: " << input;
    } else {
        EXPECT_EQ(result, nullptr) << "Should have failed to parse: " << input;
    }
}

TEST_P(ParserParamTest, ThreeTopLevelFunctions) {
    Lexer lexer;
    Parser parser(lexer);

    // Bootstrap the first token
    parser.getNextToken();

    const auto& params = GetParam();

    if (params.input.find("def ") == 0) {
        auto result = parser.parseDefinition();
        verifyTest<FunctionAST>(params.shouldPass, std::move(result), params.input);
    } else if (params.input.find("extern ") == 0) {
        auto result = parser.parseExtern();
        verifyTest<PrototypeAST>(params.shouldPass, std::move(result), params.input);
    } else {  // For everything else, treat as a top-level expression
        auto result = parser.parseTopLevelExpr();
        verifyTest<FunctionAST>(params.shouldPass, std::move(result), params.input);
    }
}

INSTANTIATE_TEST_SUITE_P(
    ParserTests,
    ParserParamTest,
    ::testing::Values(
        // Valid expressions
        ParserTestCase{"SimpleAddition", "1 + 2", true},
        ParserTestCase{"Precedence", "1 + 2 * 3", true},
        ParserTestCase{"Parentheses", "(1 + 2) * 3", true},
        ParserTestCase{"DecimalNumber", "3.14159", true},

        // --- Identifiers & Calls ---
        ParserTestCase{"VariableRef", "myVar", true},
        ParserTestCase{"FunctionCall", "foo(a, b)", true},
        ParserTestCase{"NestedCall", "foo(bar(42), b)", true},

        // Valid definitions & externs
        ParserTestCase{"FunctionDef", "def foo(x y) x + y", true},
        ParserTestCase{"ExternLink", "extern cos(x)", true},
        ParserTestCase{"NoArgDef", "def pi() 3.14", true},

        // Syntax Errors
        ParserTestCase{"MissingParen", "(1 + 2", false},
        ParserTestCase{"InvalidDef", "def foo(123) x", false},
        ParserTestCase{"MissingCommaInCall", "foo(a b)", false},
        ParserTestCase{"TrailingOp", "10 + ", false},
        ParserTestCase{"EmptyExpr", "+ 5", false}
    ),
    [](const ::testing::TestParamInfo<ParserParamTest::ParamType>& info) {
        return info.param.testName;
    }
);

// class ParserInnerFuncTest : public ::testing::TestWithParam<ParserTestCase> {
// protected:
//     void SetUp() override {
//         // Write input to a temporary file
//         std::ofstream tmpFile("_parser_inner_input.txt");
//         tmpFile << GetParam().input;
//         tmpFile.close();

//         // Redirect stdin for the lexer
//         ASSERT_TRUE(freopen("_parser_inner_input.txt", "r", stdin) != nullptr);
//     }

//     void TearDown() override {
//         std::remove("_parser_inner_input.txt");
//     }
// };

// TEST_P(ParserInnerFuncTest, parseNumberExpr) {
//     Lexer lexer;
//     Parser parser(lexer);

//     // Bootstrap the first token
//     parser.getNextToken();

//     const auto& params = GetParam();
//     auto result = parser.parseNumberExpr();

//     if (params.shouldPass) {
//         EXPECT_NE(result, nullptr) << "Failed to parse number expression: " << params.input;
//     } else {
//         EXPECT_EQ(result, nullptr) << "Should have failed to parse number expression: " << params.input;
//     }
// }