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

// --- 4. Unary Expressions ---
class ParseUnaryExprTest : public ParserParamTest {};
TEST_P(ParseUnaryExprTest, parseUnary) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseUnary(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(UnaryTests, ParseUnaryExprTest, ::testing::Values(
    ParserTestCase{"SimpleUnary", "!x", true},
    ParserTestCase{"NestedUnary", "!!x", true},
    ParserTestCase{"UnaryPrimary", "42", true},
    ParserTestCase{"UnaryWithParens", "!(x + y)", true},
    ParserTestCase{"UnaryComplex", "!!(x < y)", true},
    ParserTestCase{"UnaryMissingOperand", "!", false},   // operator with nothing to apply to
    ParserTestCase{"UnaryDanglingParen", "!(x", false}   // operand fails to parse
), [](const auto& info) { return info.param.testName; });

// --- 5. Full Binary Expressions ---
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
    ParserTestCase{"Assignment", "x = 1", true},          // '=' is a binop with precedence 2
    ParserTestCase{"AssignmentOfExpr", "x = y + 1", true},
    ParserTestCase{"TrailingOperator", "10 +", false},
    ParserTestCase{"LeadingOperator", "+ 10", true},  // false -> true since Chapter 6: parses as unary '+'
    ParserTestCase{"DoubleOperator", "10 ++ 5", true}  // false -> true since Chapter 6: 10 + (+5)
), [](const auto& info) { return info.param.testName; });

// --- 6. Function Prototypes ---
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
    ParserTestCase{"InvalidArgSeparator", "foo(x, y)", false}, // Prototype uses space, not comma
    ParserTestCase{"UnaryProto", "unary!(v)", true},
    ParserTestCase{"BinaryProto", "binary@(v1 v2)", true},
    ParserTestCase{"BinaryWithPrecProto", "binary@ 10 (v1 v2)", true},
    ParserTestCase{"BinaryInvalidPrec", "binary@ 200 (v1 v2)", false},  // reaches the 1..100 precedence validation
    ParserTestCase{"BinaryMissingArg", "binary@ 10 (v1)", false},        // reaches the operand-count validation
    ParserTestCase{"BinaryPrecInsideParens", "binary@(200 v1 v2)", false}, // syntax error: precedence belongs before '('
    ParserTestCase{"UnaryMissingArg", "unary!()", false}
), [](const auto& info) { return info.param.testName; });

// --- 7. Function Definitions ---
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
    ParserTestCase{"MalformedProto", "def foo x) x", false},
    ParserTestCase{"DefUnary", "def unary!(v) 0 - v", true},
    ParserTestCase{"DefBinary", "def binary@(v1 v2) v1 + v2", true},
    ParserTestCase{"DefComplexBinary", "def binary| 1 (v1 v2) if v1 then v1 else v2", true},
    ParserTestCase{"DefRecursiveBinary", "def binary| 5 (a b) a | b", true}  // the operator is usable inside its own body
), [](const auto& info) { return info.param.testName; });

// --- 8. Extern Declarations ---
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

// --- 9. Tree shape ---
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

TEST_F(ParseTreeTest, UserOperatorChangesLaterParse) {
    // def binary| 5 (a b) a;  a | b + c
    // Parsing the definition installs '|' at precedence 5 (looser than '+'),
    // so the SAME parser instance then reads  a | b + c  as  a | (b + c).
    tmpPath = "_parser_tree_input_" + std::to_string(getpid()) + ".txt";
    std::ofstream(tmpPath) << "def binary| 5 (a b) a; a | b + c";
    ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    lexer = std::make_unique<Lexer>();
    parser = std::make_unique<Parser>(*lexer);
    parser->getNextToken();

    auto def = parser->parseDefinition();
    ASSERT_NE(def, nullptr);
    EXPECT_TRUE(def->getProto()->isBinaryOp());
    EXPECT_EQ(def->getProto()->getOperatorName(), '|');
    EXPECT_EQ(def->getProto()->getBinaryPrecedence(), 5u);
    EXPECT_EQ(parser->getCurToken(), ';');
    parser->getNextToken();   // eat ';'

    auto expr = parser->parseExpression();
    auto* bar = asBin(expr.get());
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(bar->getOp(), '|');
    EXPECT_EQ(asVar(bar->getLHS())->getName(), "a");
    auto* plus = asBin(bar->getRHS());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getOp(), '+');
}

TEST_F(ParseTreeTest, FailedRedefinitionRestoresPrecedence) {
    // def binary| 5 (a b) a;  def binary| 7 (a b) (a;  a | b + c
    // The second definition fails to parse its body (unclosed paren -- note a
    // bare ')' would NOT fail: parseUnary presumes any ASCII char is a unary
    // operator), so '|' must keep its ORIGINAL precedence 5 (looser than '+'):
    // a | (b + c), not (a | b) + c.
    tmpPath = "_parser_tree_input_" + std::to_string(getpid()) + ".txt";
    std::ofstream(tmpPath) << "def binary| 5 (a b) a; def binary| 7 (a b) (a; a | b + c";
    ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    lexer = std::make_unique<Lexer>();
    parser = std::make_unique<Parser>(*lexer);
    parser->getNextToken();

    ASSERT_NE(parser->parseDefinition(), nullptr);
    parser->getNextToken();                        // eat ';'
    EXPECT_EQ(parser->parseDefinition(), nullptr); // "expected ')'" at the ';'
    EXPECT_EQ(parser->getCurToken(), ';');
    parser->getNextToken();                        // error recovery: eat ';'

    auto expr = parser->parseExpression();
    auto* bar = asBin(expr.get());
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(bar->getOp(), '|');                  // still an operator, precedence 5
    auto* plus = asBin(bar->getRHS());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getOp(), '+');
}

TEST_F(ParseTreeTest, UnaryTree) {
    // !!x  ==>  Unary('!', Unary('!', Var x))
    auto expr = parseExpr("!!x");
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->getKind(), ExprAST::Expr_Unary);
    auto* outer = static_cast<UnaryExprAST*>(expr.get());
    EXPECT_EQ(outer->getOpcode(), '!');
    ASSERT_EQ(outer->getOperand()->getKind(), ExprAST::Expr_Unary);
    auto* inner = static_cast<UnaryExprAST*>(outer->getOperand());
    EXPECT_EQ(asVar(inner->getOperand())->getName(), "x");
}

TEST_F(ParseTreeTest, AssignmentBindsLoosest) {
    // x = y + 1  ==>  '='(x, '+'(y, 1)): precedence 2 is below every other operator
    auto expr = parseExpr("x = y + 1");
    auto* assign = asBin(expr.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->getOp(), '=');
    EXPECT_EQ(asVar(assign->getLHS())->getName(), "x");
    auto* plus = asBin(assign->getRHS());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getOp(), '+');
}

TEST_F(ParseTreeTest, VarTree) {
    // var a = 1, b in a + b  ==>  VarExprAST{[a: Num 1, b: null], body '+'}
    auto expr = parseExpr("var a = 1, b in a + b");
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(expr->getKind(), ExprAST::Expr_VarDecl);
    auto* v = static_cast<VarExprAST*>(expr.get());
    ASSERT_EQ(v->getVarNames().size(), 2u);
    EXPECT_EQ(v->getVarNames()[0].first, "a");
    ASSERT_NE(asNum(v->getVarNames()[0].second.get()), nullptr);
    EXPECT_EQ(v->getVarNames()[1].first, "b");
    EXPECT_EQ(v->getVarNames()[1].second.get(), nullptr);   // no initializer
    auto* body = asBin(v->getBody());
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->getOp(), '+');
}

TEST_F(ParseTreeTest, NodesCarryLocations) {
    // Chapter 9: a + b * c  -- every node knows where its token was
    auto expr = parseExpr("a + b * c");
    auto* plus = asBin(expr.get());
    ASSERT_NE(plus, nullptr);
    EXPECT_EQ(plus->getLine(), 1);
    EXPECT_EQ(plus->getCol(), 3);                       // the '+'
    EXPECT_EQ(asVar(plus->getLHS())->getCol(), 1);      // a
    auto* times = asBin(plus->getRHS());
    ASSERT_NE(times, nullptr);
    EXPECT_EQ(times->getCol(), 7);                      // the '*'
    EXPECT_EQ(asVar(times->getLHS())->getCol(), 5);     // b
    EXPECT_EQ(asVar(times->getRHS())->getCol(), 9);     // c
}

TEST_F(ParseTreeTest, PrototypeLineAndMainName) {
    // A definition on line 3 records that line; a top-level expression becomes `main`.
    tmpPath = "_parser_tree_input_" + std::to_string(getpid()) + ".txt";
    std::ofstream(tmpPath) << "\n\ndef foo(x) x;\n1 + 2";
    ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    lexer = std::make_unique<Lexer>();
    parser = std::make_unique<Parser>(*lexer);
    parser->getNextToken();

    auto def = parser->parseDefinition();
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->getProto()->getLine(), 3);
    parser->getNextToken();   // eat ';'

    auto top = parser->parseTopLevelExpr();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->getProto()->getName(), "main");
    EXPECT_EQ(top->getProto()->getLine(), 4);
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

// --- 10. If Expressions ---
class ParseIfExprTest : public ParserParamTest {};
TEST_P(ParseIfExprTest, parseIfExpr) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseIfExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(IfTests, ParseIfExprTest, ::testing::Values(
    ParserTestCase{"SimpleIf", "if 1 < 2 then 3 else 4", true},
    ParserTestCase{"NestedIf", "if x then (if y then 1 else 2) else 3", true},
    ParserTestCase{"MissingThen", "if 1 < 2 3 else 4", false},
    ParserTestCase{"MissingElse", "if 1 < 2 then 3", false}
), [](const auto& info) { return info.param.testName; });

// --- 11. For Expressions ---
class ParseForExprTest : public ParserParamTest {};
TEST_P(ParseForExprTest, parseForExpr) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseForExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(ForTests, ParseForExprTest, ::testing::Values(
    ParserTestCase{"SimpleFor", "for i = 1, i < 10, 1 in i * 2", true},
    ParserTestCase{"ForWithoutStep", "for i = 1, i < 10 in i * 2", true}, // Step is optional
    ParserTestCase{"MissingAssign", "for i 1, i < 10, 1 in i", false},
    ParserTestCase{"MissingIn", "for i = 1, i < 10, 1 i * 2", false}
), [](const auto& info) { return info.param.testName; });

// --- 12. Mutable Variables ---
class ParseVarExprTest : public ParserParamTest {};
TEST_P(ParseVarExprTest, parseVarExpr) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseVarExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(VarTests, ParseVarExprTest, ::testing::Values(
    ParserTestCase{"SimpleVar", "var x = 1 in x", true},
    ParserTestCase{"MultipleVars", "var x = 1, y = 2, z = 3 in x + y + z", true},
    ParserTestCase{"VarsWithoutInit", "var x, y in x + y", true},
    ParserTestCase{"MixedInit", "var x = 1, y, z = 3 in x + y + z", true},
    ParserTestCase{"MissingIn", "var x = 1 x + 1", false},
    ParserTestCase{"MissingIdentifier", "var = 1 in 2", false}
), [](const auto& info) { return info.param.testName; });
