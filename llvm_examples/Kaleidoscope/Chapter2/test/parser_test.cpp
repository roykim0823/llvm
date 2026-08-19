#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <memory>
#include <unistd.h>
#include "parser.h"

using namespace toy;

// --- Base Fixture ---
struct ParserTestCase {
    std::string testName;
    std::string input;
    bool shouldPass;
};

class ParserParamTest : public ::testing::TestWithParam<ParserTestCase> {
protected:
    void SetUp() override {
        // Suffix with the pid: gtest_discover_tests registers each case as its
        // own ctest test, so `ctest -j` runs cases of this binary concurrently
        // in the same working directory.
        tmpPath = "_parser_input_" + std::to_string(getpid()) + ".txt";
        std::ofstream tmpFile(tmpPath);
        tmpFile << GetParam().input;
        tmpFile.close();
        ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    }
    void TearDown() override {
        std::remove(tmpPath.c_str());
    }
    std::string tmpPath;
};

template<typename T>
void verifyTest(bool shouldPass, std::unique_ptr<T> result, const std::string& input) {
    if (shouldPass) {
        EXPECT_NE(result, nullptr) << "Failed to parse: " << input;
    } else {
        EXPECT_EQ(result, nullptr) << "Should have failed to parse: " << input;
    }
}

// --- 1. Number Expressions ---
class ParseNumberExprTest : public ParserParamTest {};
TEST_P(ParseNumberExprTest, parseNumberExpr) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseNumberExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(NumberTests, ParseNumberExprTest, ::testing::Values(
    ParserTestCase{"Integer", "42", true},
    ParserTestCase{"Decimal", "3.1415", true},
    ParserTestCase{"Zero", "0", true},
    ParserTestCase{"LargeNumber", "1.234567", true},
    ParserTestCase{"LeadingDotNumber", ".5", true} // deliberate deviation: the lexer validates the leading dot, so .5 lexes as 0.5
), [](const auto& info) { return info.param.testName; });

// --- 2. Identifier & Call Expressions ---
class ParseIdentifierExprTest : public ParserParamTest {};
TEST_P(ParseIdentifierExprTest, parseIdentifierExpr) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseIdentifierExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(IdentifierTests, ParseIdentifierExprTest, ::testing::Values(
    ParserTestCase{"SimpleVar", "x", true},
    ParserTestCase{"UnderscoreStopsIdentifier", "my_var_123", true}, // identifiers don't allow '_': lexes as just "my", parsed as VariableExprAST("my")
    ParserTestCase{"CallNoArgs", "foo()", true},
    ParserTestCase{"CallThreeArgs", "foo(a, b, c)", true},
    ParserTestCase{"NestedCall", "foo(bar(z))", true},
    ParserTestCase{"CallMissingComma", "foo(a b)", false},
    ParserTestCase{"CallTrailingComma", "foo(a,)", false},
    ParserTestCase{"EmptyArgInMiddle", "foo(a,,b)", false}
), [](const auto& info) { return info.param.testName; });

// --- 3. Parentheses Expressions ---
class ParseParenExprTest : public ParserParamTest {};
TEST_P(ParseParenExprTest, parseParenExpr) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseParenExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(ParenTests, ParseParenExprTest, ::testing::Values(
    ParserTestCase{"SimpleParen", "(42)", true},
    ParserTestCase{"ExpressionInParen", "(a + b)", true},
    ParserTestCase{"DeeplyNested", "((((10))))", true},
    ParserTestCase{"UnclosedParen", "(1 + 2", false},
    ParserTestCase{"EmptyParen", "()", false}, // parseExpression returns nullptr for empty
    ParserTestCase{"MismatchedParen", "(1 + 2]", false}
), [](const auto& info) { return info.param.testName; });

// --- 4. Full Binary Expressions ---
class ParseExpressionTest : public ParserParamTest {};
TEST_P(ParseExpressionTest, parseExpression) {
    // This test covers the full expression parsing logic, including operator precedence and associativity.
    // parser.parseExpression() will call parsePrimary() and parseBinOpRHS() to build the AST according to the grammar.
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseExpression(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(ExpressionTests, ParseExpressionTest, ::testing::Values(
    ParserTestCase{"Addition", "1 + 2", true},
    ParserTestCase{"OrderOfOps", "1 + 2 * 3", true},
    ParserTestCase{"PrecedenceMix", "a * b + c * d", true},
    ParserTestCase{"Associativity", "a - b - c", true},
    ParserTestCase{"Comparison", "x < y", true},
    ParserTestCase{"TrailingOperator", "10 +", false},
    ParserTestCase{"LeadingOperator", "+ 10", false},
    ParserTestCase{"DoubleOperator", "10 ++ 5", false}
), [](const auto& info) { return info.param.testName; });

// --- 5. Function Prototypes ---
class ParsePrototypeTest : public ParserParamTest {};
TEST_P(ParsePrototypeTest, parsePrototype) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parsePrototype(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(PrototypeTests, ParsePrototypeTest, ::testing::Values(
    ParserTestCase{"SimpleProto", "foo(x y)", true},
    ParserTestCase{"NoArgProto", "bar()", true},
    ParserTestCase{"ManyArgs", "func(a b c d e)", true},
    ParserTestCase{"DigitInName", "foo123(x)", true},
    ParserTestCase{"NumericStart", "123foo(x)", false},
    ParserTestCase{"ArgMissingName", "foo(x , z)", false},
    ParserTestCase{"InvalidArgSeparator", "foo(x, y)", false} // Prototype uses space, not comma
), [](const auto& info) { return info.param.testName; });

// --- 6. Function Definitions ---
class ParseDefinitionTest : public ParserParamTest {};
TEST_P(ParseDefinitionTest, parseDefinition) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseDefinition(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(DefinitionTests, ParseDefinitionTest, ::testing::Values(
    ParserTestCase{"DefSimple", "def foo(x) x", true},
    ParserTestCase{"DefMultiLineLogic", "def bar(x y) (x + y) * (x - y)", true},
    ParserTestCase{"MissingBody", "def foo(x)", false},
    ParserTestCase{"KeywordInName", "def def(x) x", false},
    ParserTestCase{"MalformedProto", "def foo x) x", false}
), [](const auto& info) { return info.param.testName; });

// --- 7. Extern Declarations ---
class ParseExternTest : public ParserParamTest {};
TEST_P(ParseExternTest, parseExtern) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseExtern(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(ExternTests, ParseExternTest, ::testing::Values(
    ParserTestCase{"ExternCos", "extern cos(x)", true},
    ParserTestCase{"ExternSin", "extern sin(y)", true},
    ParserTestCase{"ExternMissingKeyword", "cos(x)", false}
), [](const auto& info) { return info.param.testName; });