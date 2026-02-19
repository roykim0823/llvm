#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include "lexer.h"

using namespace toy;

// Structure to hold our test parameters
struct LexerTestCase {
    std::string input;
    int expectedToken;
    std::string expectedIdentifier = "";
    double expectedNumVal = 0.0;
};

class LexerParamTest : public ::testing::TestWithParam<LexerTestCase> {
protected:
    void SetUp() override {
        // Create a temporary file to simulate stdin
        std::ofstream tmpFile("_lexter_test_input.txt");
        tmpFile << GetParam().input;
        tmpFile.close();

        // Redirect stdin to our temp file
        ASSERT_TRUE(freopen("_lexer_test_input.txt", "r", stdin) != nullptr);
    }

    void TearDown() override {
        // Restore stdin is complex in C++, usually we just let the
        // OS handle it or refactor to use istreams.
        // Delete the temporary file after the test finishes
        std::remove("_lexer_test_input.txt");
    }
};

TEST_P(LexerParamTest, ProcessesInputCorrectly) {
    Lexer lexer;
    auto params = GetParam();

    int token = lexer.gettok();

    EXPECT_EQ(token, params.expectedToken);

    if (token == tok_identifier) {
        EXPECT_EQ(lexer.getIdentifierStr(), params.expectedIdentifier);
    } else if (token == tok_number) {
        EXPECT_DOUBLE_EQ(lexer.getNumVal(), params.expectedNumVal);
    }
}

// Define the test cases
INSTANTIATE_TEST_SUITE_P(
    LexerTests,
    LexerParamTest,
    ::testing::Values(
        // Keywords
        LexerTestCase{"", tok_eof},
        LexerTestCase{"def", tok_def},
        LexerTestCase{"extern", tok_extern},

        // Identifiers
        LexerTestCase{"myVar", tok_identifier, "myVar"},
        LexerTestCase{"x123", tok_identifier, "x123"},

        // Numbers
        LexerTestCase{"123.45", tok_number, "", 123.45},
        LexerTestCase{"0.001", tok_number, "", 0.001},

        // Single characters (ASCII)
        LexerTestCase{"+", '+'},
        LexerTestCase{"(", '('},

        // Comments and Whitespace (should skip and return next token)
        LexerTestCase{"# this is a comment\n42", tok_number, "", 42.0},
        LexerTestCase{"   \t\n  def", tok_def}
    )
);