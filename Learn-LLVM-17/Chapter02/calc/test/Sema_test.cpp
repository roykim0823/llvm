#include "Sema.h"
#include "Parser.h"
#include "Lexer.h"
#include "AST.h"
#include <gtest/gtest.h>
#include <string>

struct SemaTestCase {
    std::string input;
    bool expectedSemaError;
};

class SemaParamTest : public ::testing::TestWithParam<SemaTestCase> {};

TEST_P(SemaParamTest, ValidatesScopeRules) {
    const auto& param = GetParam();

    llvm::StringRef buffer(param.input);
    Lexer lexer(buffer);
    Parser parser(lexer);
    auto tree = parser.parse();

    // We only want to test Sema, so we assume Parser succeeds
    ASSERT_FALSE(parser.hasError()) << "Parser syntax error on input: " << param.input;
    ASSERT_NE(tree, nullptr);

    // 1. Run Semantic Analysis
    Sema sema;
    bool hasError = sema.semantic(tree.get());

    // 2. Check if Sema caught the scope errors
    EXPECT_EQ(hasError, param.expectedSemaError)
        << "Sema expectation failed for input: " << param.input;

    // 3. Clean up
    // ASTDeleter deleter;
    // tree->accept(deleter);
}

INSTANTIATE_TEST_SUITE_P(
    ScopeRules,
    SemaParamTest,
    ::testing::Values(
        // --- VALID CASES (Should return false for error) ---
        SemaTestCase{"with x : x + 1", false},
        SemaTestCase{"with a, b, c : a * b - c", false},
        SemaTestCase{"1 + 2 * 3", false}, // No variables, so no scope needed

        // --- INVALID CASES (Should return true for error) ---

        // Undeclared variable 'y'
        SemaTestCase{"with x : y + 1", true},

        // Undeclared variables (no 'with' block at all)
        SemaTestCase{"a + 5", true},

        // Duplicate declaration of 'x'
        SemaTestCase{"with x, x : x + 1", true},

        // One valid, one undeclared ('b' is missing)
        SemaTestCase{"with a : a * b", true}
    )
);