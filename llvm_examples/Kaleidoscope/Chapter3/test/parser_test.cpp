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

// --- 8. Tree shape ---
// The suites above only ask "did it parse?". With the kind tags and getters
// on the AST we can also ask "into what?" -- and pin the operator-precedence
// algorithm's actual output. (Chapter 3 swaps these getKind() checks for
// llvm::isa<>/llvm::cast<>, which call the same classof() under the hood.)
class ParseTreeTest : public ::testing::Test {
protected:
    // Feed `input` to the lexer via stdin and parse one expression.
    std::unique_ptr<ExprAST> parseExpr(const std::string& input) {
        tmpPath = "_parser_tree_input_" + std::to_string(getpid()) + ".txt";
        std::ofstream(tmpPath) << input;
        if (!freopen(tmpPath.c_str(), "r", stdin)) return nullptr;
        lexer = std::make_unique<Lexer>();
        parser = std::make_unique<Parser>(*lexer);
        parser->getNextToken();
        return parser->parseExpression();
    }
    void TearDown() override { std::remove(tmpPath.c_str()); }

    // Downcasts that fail the test (and return null) on a kind mismatch.
    static BinaryExprAST*   asBin(ExprAST* e)  { return kindIs(e, ExprAST::Expr_BinOp) ? static_cast<BinaryExprAST*>(e)   : nullptr; }
    static VariableExprAST* asVar(ExprAST* e)  { return kindIs(e, ExprAST::Expr_Var)   ? static_cast<VariableExprAST*>(e) : nullptr; }
    static NumberExprAST*   asNum(ExprAST* e)  { return kindIs(e, ExprAST::Expr_Num)   ? static_cast<NumberExprAST*>(e)   : nullptr; }
    static CallExprAST*     asCall(ExprAST* e) { return kindIs(e, ExprAST::Expr_Call)  ? static_cast<CallExprAST*>(e)     : nullptr; }
    static bool kindIs(ExprAST* e, ExprAST::ExprASTKind k) {
        if (!e) { ADD_FAILURE() << "null node"; return false; }
        if (e->getKind() != k) { ADD_FAILURE() << "wrong node kind: " << e->getKind() << " != " << k; return false; }
        return true;
    }

    std::string tmpPath;
    std::unique_ptr<Lexer> lexer;
    std::unique_ptr<Parser> parser;
};

TEST_F(ParseTreeTest, PrecedenceTrace) {
    // The README's trace: a + b * c - d  ==>  ((a + (b * c)) - d)
    auto expr = parseExpr("a + b * c - d");
    auto* minus = asBin(expr.get());
    ASSERT_NE(minus, nullptr);
    EXPECT_EQ(minus->getOp(), '-');

    auto* plus = asBin(minus->getLHS());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getOp(), '+');
    ASSERT_NE(asVar(plus->getLHS()), nullptr);
    EXPECT_EQ(asVar(plus->getLHS())->getName(), "a");

    auto* times = asBin(plus->getRHS());
    ASSERT_NE(times, nullptr);
    EXPECT_EQ(times->getOp(), '*');
    EXPECT_EQ(asVar(times->getLHS())->getName(), "b");
    EXPECT_EQ(asVar(times->getRHS())->getName(), "c");

    EXPECT_EQ(asVar(minus->getRHS())->getName(), "d");
}

TEST_F(ParseTreeTest, LeftAssociative) {
    // a - b - c  ==>  ((a - b) - c), never (a - (b - c))
    auto expr = parseExpr("a - b - c");
    auto* outer = asBin(expr.get());
    ASSERT_NE(outer, nullptr);
    auto* inner = asBin(outer->getLHS());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(asVar(inner->getLHS())->getName(), "a");
    EXPECT_EQ(asVar(inner->getRHS())->getName(), "b");
    EXPECT_EQ(asVar(outer->getRHS())->getName(), "c");
}

TEST_F(ParseTreeTest, TighterOperatorFirst) {
    // a * b + c  ==>  ((a * b) + c): no recursion needed, the loop merges as it goes
    auto expr = parseExpr("a * b + c");
    auto* plus = asBin(expr.get());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getOp(), '+');
    auto* times = asBin(plus->getLHS());
    ASSERT_NE(times, nullptr);
    EXPECT_EQ(times->getOp(), '*');
    EXPECT_EQ(asVar(plus->getRHS())->getName(), "c");
}

TEST_F(ParseTreeTest, ParenthesesLeaveNoNode) {
    // (a + b) * c  ==>  '*' over '+': the parentheses only shaped the tree
    auto expr = parseExpr("(a + b) * c");
    auto* times = asBin(expr.get());
    ASSERT_NE(times, nullptr);
    EXPECT_EQ(times->getOp(), '*');
    auto* plus = asBin(times->getLHS());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getOp(), '+');
    EXPECT_EQ(asVar(times->getRHS())->getName(), "c");
}

TEST_F(ParseTreeTest, CallWithArguments) {
    // foo(1, x)  ==>  CallExprAST("foo", [Number 1, Variable x])
    auto expr = parseExpr("foo(1, x)");
    auto* call = asCall(expr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->getCallee(), "foo");
    ASSERT_EQ(call->getArgs().size(), 2u);
    ASSERT_NE(asNum(call->getArgs()[0].get()), nullptr);
    EXPECT_DOUBLE_EQ(asNum(call->getArgs()[0].get())->getVal(), 1.0);
    ASSERT_NE(asVar(call->getArgs()[1].get()), nullptr);
    EXPECT_EQ(asVar(call->getArgs()[1].get())->getName(), "x");
}
