#include "Parser.h"
#include "Lexer.h"
#include <gtest/gtest.h>
#include <string>

struct ParserTestCase {
    std::string input;
    bool expectedError;
};

class ParserMinimalTest : public ::testing::TestWithParam<ParserTestCase> {};

TEST_P(ParserMinimalTest, ValidatesSyntaxAcceptance) {
    const auto& param = GetParam();

    llvm::StringRef buffer(param.input);
    Lexer lexer(buffer);
    Parser parser(lexer);

    auto tree = parser.parse();

    // Strictly test if the parser accepted or rejected the syntax
    EXPECT_EQ(parser.hasError(), param.expectedError)
        << "Failed on input: " << param.input;

    // Note: This minimal approach leaks the 'tree' memory because
    // there is no built-in way to recursively delete the AST nodes
    // without a Visitor or adding destructors to the AST classes.
}

INSTANTIATE_TEST_SUITE_P(
    SyntaxValidation,
    ParserMinimalTest,
    ::testing::Values(
        // --- Valid Syntax (Should parse cleanly) ---
        ParserTestCase{"1 + 2 * 3", false},
        ParserTestCase{"(1 + 2) * 3", false},
        ParserTestCase{"1 - 2 - 3", false},
        ParserTestCase{"with x : x + 1", false},
        ParserTestCase{"with a, b, c : a * b - c", false},

        // --- Invalid Syntax (Should trigger errors) ---
        ParserTestCase{"with x x + 1", true}, // missing colon
        ParserTestCase{"(1 + 2", true},       // unbalanced paren
        ParserTestCase{"1 + ", true},         // missing right operand
        ParserTestCase{"* 5", true},          // missing left operand
        ParserTestCase{"", true}              // empty input
    )
);