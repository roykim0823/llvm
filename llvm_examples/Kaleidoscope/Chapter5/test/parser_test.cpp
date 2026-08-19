#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <memory>
#include <unistd.h>
#include "parser.h"
#include "ir_gen_ctx.h"

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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
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
    IRGenContext ctx;
    Parser parser(lexer, ctx);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseExtern(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(ExternTests, ParseExternTest, ::testing::Values(
    ParserTestCase{"ExternCos", "extern cos(x)", true},
    ParserTestCase{"ExternSin", "extern sin(y)", true},
    ParserTestCase{"ExternMissingKeyword", "cos(x)", false}
), [](const auto& info) { return info.param.testName; });

// --- 8. If Expressions ---
class ParseIfExprTest : public ParserParamTest {};
TEST_P(ParseIfExprTest, parseIfExpr) {
    Lexer lexer;
    IRGenContext ctx;
    Parser parser(lexer, ctx);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseIfExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(IfTests, ParseIfExprTest, ::testing::Values(
    ParserTestCase{"SimpleIf", "if 1 < 2 then 3 else 4", true},
    ParserTestCase{"NestedIf", "if x then (if y then 1 else 2) else 3", true},
    ParserTestCase{"MissingThen", "if 1 < 2 3 else 4", false},
    ParserTestCase{"MissingElse", "if 1 < 2 then 3", false}
), [](const auto& info) { return info.param.testName; });

// --- 9. For Expressions ---
class ParseForExprTest : public ParserParamTest {};
TEST_P(ParseForExprTest, parseForExpr) {
    Lexer lexer;
    IRGenContext ctx;
    Parser parser(lexer, ctx);
    parser.getNextToken();
    verifyTest(GetParam().shouldPass, parser.parseForExpr(), GetParam().input);
}
INSTANTIATE_TEST_SUITE_P(ForTests, ParseForExprTest, ::testing::Values(
    ParserTestCase{"SimpleFor", "for i = 1, i < 10, 1 in i * 2", true},
    ParserTestCase{"ForWithoutStep", "for i = 1, i < 10 in i * 2", true}, // Step is optional
    ParserTestCase{"MissingAssign", "for i 1, i < 10, 1 in i", false},
    ParserTestCase{"MissingIn", "for i = 1, i < 10, 1 i * 2", false}
), [](const auto& info) { return info.param.testName; });

// -----------------------------------------------------------------------------
// JIT Execution Tests: These tests will parse an expression, generate LLVM IR, execute it
// in the JIT, and verify the runtime result matches the expected value. This will test the full
// pipeline from parsing to code generation to execution for various expressions.

// 1. Define the parameters for the JIT execution tests
struct JITTestCase {
    std::string testName;
    std::string expression;
    double expectedResult;
};

// 2. Create the Parametric Fixture inheriting from CodegenTest
class JITExecutionParamTest : public ::testing::TestWithParam<JITTestCase> {
protected:
    std::unique_ptr<IRGenContext> ctx;

    void SetUp() override {
        // Initialize a fresh IRGenContext for each test to ensure a clean slate
        // for code generation and JIT execution
        ctx = std::make_unique<IRGenContext>();

        // Write the expression to a temporary file and redirect stdin to read from it,
        // so the parser can read the expression as if it were user input.
        // pid suffix: `ctest -j` runs cases of this binary concurrently.
        tmpPath = "_jit_param_input_" + std::to_string(getpid()) + ".txt";
        std::ofstream tmpFile(tmpPath);
        tmpFile << GetParam().expression;
        tmpFile.close();
        ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    }

    void TearDown() override {
        // Clean up the temporary file and reset the context to free resources
        std::remove(tmpPath.c_str());
        ctx.reset();
    }

    std::string tmpPath;
};

TEST_P(JITExecutionParamTest, EvaluateExpression) {
    Lexer lexer;
    Parser parser(lexer, *ctx);

    // Prime the parser by reading the first token, which is necessary before calling parseTopLevelExpr
    parser.getNextToken();

    // 1. Parse the expression into an AST
    auto ast = parser.parseTopLevelExpr();
    ASSERT_NE(ast, nullptr) << "Failed to parse expression: " << GetParam().expression;

    // 2. Generate LLVM IR from the AST
    llvm::Function *F = ast->codegen(*ctx);
    ASSERT_NE(F, nullptr) << "Failed to generate IR for expression: " << GetParam().expression;

    // 3. Execute the generated IR in the JIT and verify the result
    auto RT = ctx->theJIT->getMainJITDylib().createResourceTracker();

    // 4. Package the module and context, then hand it to the JIT
    auto TSM = llvm::orc::ThreadSafeModule(std::move(ctx->theModule), std::move(ctx->theContext));
    ctx->ExitOnErr(ctx->theJIT->addModule(std::move(TSM), RT));

    // 5. Immediately re-initialize the context's module and pass manager to maintain state integrity
    ctx->InitializeModuleAndPassManager();

    // 6. Look up the compiled symbol for the anonymous expression function
    auto ExprSymbol = ctx->ExitOnErr(ctx->theJIT->lookup("__anon_expr"));

    // 7. Cast the symbol address to a callable C++ function pointer and execute it
    double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
    double actualResult = FP();

    // 8. Verify the result matches the expected value
    EXPECT_DOUBLE_EQ(actualResult, GetParam().expectedResult)
        << "Mismatch in expression: " << GetParam().expression;

    // 9. Clean up JIT memory for this test case
    ctx->ExitOnErr(RT->remove());
}

INSTANTIATE_TEST_SUITE_P(
  MathOperations,
  JITExecutionParamTest,
  ::testing::Values(
    // Basic arithmetic operations
    JITTestCase{"Addition", "4.0 + 5.0", 9.0},
    JITTestCase{"Subtraction", "10.0 - 2.5", 7.5},
    JITTestCase{"Multiplication", "3.0 * 3.0", 9.0},
    // Operator precedence and associativity
    JITTestCase{"Precedence", "2.0 + 3.0 * 4.0", 14.0},
    JITTestCase{"Parentheses", "(2.0 + 3.0) * 4.0", 20.0},
    // Comparison operations (< operator returns 1.0 if true, 0.0 if false)
    JITTestCase{"ComparisonTrue", "1.0 < 5.0", 1.0},
    JITTestCase{"ComparisonFalse", "5.0 < 1.0", 0.0},
    JITTestCase{"ComplexExpression", "(1.0 + 2.0) * (5.0 < 10.0) + 4.0", 7.0},

    // Chapter 5 control flow
    JITTestCase{"IfTrue", "if 1.0 < 2.0 then 42.0 else 0.0", 42.0},
    JITTestCase{"IfFalse", "if 5.0 < 2.0 then 42.0 else 0.0", 0.0},
    JITTestCase{"ForLoopExecution",
    // The end condition is tested after the body with the pre-increment i
    // (do-while semantics), so the body runs for i = 1, 2, 3, 4.
    // The for expression itself always evaluates to 0.0 per the tutorial spec.
      "for i = 1.0, i < 4.0, 1.0 in i * 2.0", 0.0}
  ),
  [](const auto& info) { return info.param.testName; }
);