#include "Lexer.h"
#include <gtest/gtest.h>
#include <vector>
#include <string>

// Structure to hold our test cases
struct LexerTestCase {
    std::string input;
    Token::TokenKind expectedKind;
    std::string expectedText;
};

class LexerParameterizedTest : public ::testing::TestWithParam<LexerTestCase> {};

TEST_P(LexerParameterizedTest, HandlesSingleToken) {
    const auto& param = GetParam();

    // Initialize Lexer with the input string
    llvm::StringRef buffer(param.input);
    Lexer lexer(buffer);
    Token token;

    // Execute the lexer
    lexer.next(token);

    // Assertions
    EXPECT_EQ(token.getKind(), param.expectedKind)
        << "Failed for input: " << param.input;
    EXPECT_EQ(token.getText().str(), param.expectedText);
}

// Define the test suite with a variety of cases
INSTANTIATE_TEST_SUITE_P(
    CommonTokens,
    LexerParameterizedTest,
    ::testing::Values(
        // Keywords & Identifiers
        LexerTestCase{"with", Token::KW_with, "with"},
        LexerTestCase{"variable", Token::ident, "variable"},

        // Numbers
        LexerTestCase{"12345", Token::number, "12345"},

        // Operators & Punctuators
        LexerTestCase{"+", Token::plus, "+"},
        LexerTestCase{"-", Token::minus, "-"},
        LexerTestCase{"*", Token::star, "*"},
        LexerTestCase{"/", Token::slash, "/"},
        LexerTestCase{"(", Token::l_paren, "("},
        LexerTestCase{")", Token::r_paren, ")"},
        LexerTestCase{":", Token::colon, ":"},
        LexerTestCase{",", Token::comma, ","},

        // Unknown
        LexerTestCase{"$", Token::unknown, "$"}
    )
);
//-------------------------------------------------

// Represents a single token expected in a sequence
struct ExpectedToken {
    Token::TokenKind kind;
    std::string text;
};

// Data structure for the parameterized test
struct LexerSequenceCase {
    std::string input;
    std::vector<ExpectedToken> expectedTokens;
};

class LexerSequenceTest : public ::testing::TestWithParam<LexerSequenceCase> {};

TEST_P(LexerSequenceTest, ValidatesTokenStream) {
    const auto& param = GetParam();
    llvm::StringRef buffer(param.input);
    Lexer lexer(buffer);
    Token token;

    for (const auto& expected : param.expectedTokens) {
        lexer.next(token);

        // Assert kind and text for each token in the stream
        ASSERT_EQ(token.getKind(), expected.kind)
            << "Mismatch in " << param.input << " at token: " << expected.text;
        ASSERT_EQ(token.getText().str(), expected.text);
    }

    // After the defined sequence, we should always hit End of Input
    lexer.next(token);
    EXPECT_EQ(token.getKind(), Token::eoi);
}

INSTANTIATE_TEST_SUITE_P(
    ComplexInputs,
    LexerSequenceTest,
    ::testing::Values(
        // Test 1: Standard 'with' assignment-like syntax
        LexerSequenceCase{
            "with var : 100",
            {
                {Token::KW_with, "with"},
                {Token::ident, "var"},
                {Token::colon, ":"},
                {Token::number, "100"}
            }
        },
        // Test 2: Arithmetic expression with parentheses
        LexerSequenceCase{
            "(a + 5) * 10",
            {
                {Token::l_paren, "("},
                {Token::ident, "a"},
                {Token::plus, "+"},
                {Token::number, "5"},
                {Token::r_paren, ")"},
                {Token::star, "*"},
                {Token::number, "10"}
            }
        },
        // Test 3: Comma separated list with mixed whitespace
        LexerSequenceCase{
            "x,   y, 42",
            {
                {Token::ident, "x"},
                {Token::comma, ","},
                {Token::ident, "y"},
                {Token::comma, ","},
                {Token::number, "42"}
            }
        },
        // Test 4: Handling unknown characters in a stream
        LexerSequenceCase{
            "var $ 123",
            {
                {Token::ident, "var"},
                {Token::unknown, "$"},
                {Token::number, "123"}
            }
        }
    )
);