#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <unistd.h>

#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

#include "../../include/KaleidoscopeJIT.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/TargetSelect.h"

using namespace toy;

// Everything that needs a JIT lives here. The fixture plays the driver's part:
// it owns a KaleidoscopeJIT and a CodeGenSession configured with the JIT's
// data layout, and hands modules across with takeModule() exactly like
// Driver::handle*() does. Numeric results come back as real doubles, which is
// what makes gtest the right layer for them (see README, Tests).
class JITTest : public ::testing::Test {
protected:
    llvm::ExitOnError ExitOnErr;
    std::unique_ptr<llvm::orc::KaleidoscopeJIT> theJIT;
    std::unique_ptr<CodeGenSession> codegen;

    static void SetUpTestSuite() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    }

    void SetUp() override {
        theJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
        codegen = std::make_unique<CodeGenSession>();
        codegen->setDataLayout(theJIT->getDataLayout());
    }

    void TearDown() override {
        codegen.reset();
        theJIT.reset();
    }

    // Emit `fn`, hand its module to the JIT under a fresh ResourceTracker, run
    // the named zero-argument function, free it, and return the result.
    double runAnon(FunctionAST &fn, const char *name = "__anon_expr") {
        llvm::Function *F = codegen->emitFunction(fn);
        if (!F) { ADD_FAILURE() << "codegen failed"; return 0.0; }

        auto RT = theJIT->getMainJITDylib().createResourceTracker();
        ExitOnErr(theJIT->addModule(codegen->takeModule(), RT));
        auto Sym = ExitOnErr(theJIT->lookup(name));
        double (*FP)() = Sym.getAddress().toPtr<double (*)()>();
        double result = FP();
        ExitOnErr(RT->remove());
        return result;
    }
};

//-----------------------------------------------------------------------------
// Hand-built AST -> JIT

TEST_F(JITTest, HandBuiltFunctionWithArgument) {
    // def add_two(x) x + 2.0 ; then call the native code with 5.5
    auto proto = std::make_unique<PrototypeAST>("add_two", std::vector<std::string>{"x"});
    auto body = std::make_unique<BinaryExprAST>('+', std::make_unique<VariableExprAST>("x"),
                                                std::make_unique<NumberExprAST>(2.0));
    auto fn = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

    ASSERT_NE(codegen->emitFunction(*fn), nullptr);
    ExitOnErr(theJIT->addModule(codegen->takeModule()));   // a definition: stays resident

    auto Sym = ExitOnErr(theJIT->lookup("add_two"));
    double (*FP)(double) = Sym.getAddress().toPtr<double (*)(double)>();
    EXPECT_DOUBLE_EQ(FP(5.5), 7.5);
}

TEST_F(JITTest, CrossModuleCall) {
    // def five() 5.0;  (module 1, resident)   then   five() * 2.0  (module 2, run once)
    auto five = std::make_unique<FunctionAST>(
        std::make_unique<PrototypeAST>("five", std::vector<std::string>{}),
        std::make_unique<NumberExprAST>(5.0));
    ASSERT_NE(codegen->emitFunction(*five), nullptr);
    ExitOnErr(theJIT->addModule(codegen->takeModule()));

    auto callFive = std::make_unique<CallExprAST>("five", std::vector<std::unique_ptr<ExprAST>>{});
    auto body = std::make_unique<BinaryExprAST>('*', std::move(callFive), std::make_unique<NumberExprAST>(2.0));
    auto anon = std::make_unique<FunctionAST>(
        std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>{}), std::move(body));
    EXPECT_DOUBLE_EQ(runAnon(*anon), 10.0);
}

TEST_F(JITTest, ExternResolvesIntoProcess) {
    // extern sin(x);  sin(0.0)  -- the JIT falls back to dlsym on the running process (libm)
    auto proto = std::make_unique<PrototypeAST>("sin", std::vector<std::string>{"x"});
    ASSERT_NE(codegen->emitPrototype(*proto), nullptr);
    ExitOnErr(theJIT->addModule(codegen->takeModule()));

    std::vector<std::unique_ptr<ExprAST>> args;
    args.push_back(std::make_unique<NumberExprAST>(0.0));
    auto anon = std::make_unique<FunctionAST>(
        std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>{}),
        std::make_unique<CallExprAST>("sin", std::move(args)));
    EXPECT_DOUBLE_EQ(runAnon(*anon), 0.0);
}

TEST_F(JITTest, RecursiveFib) {
    // def fib(x) if x < 3 then 1 else fib(x-1) + fib(x-2);   fib(10) == 55
    // Recursion finally has a base case; the recursive calls resolve within the
    // same module (fib is declared there before its body is emitted).
    auto cond = std::make_unique<BinaryExprAST>('<', std::make_unique<VariableExprAST>("x"),
                                                std::make_unique<NumberExprAST>(3.0));
    auto callFib = [](double k) {
        std::vector<std::unique_ptr<ExprAST>> args;
        args.push_back(std::make_unique<BinaryExprAST>('-', std::make_unique<VariableExprAST>("x"),
                                                       std::make_unique<NumberExprAST>(k)));
        return std::make_unique<CallExprAST>("fib", std::move(args));
    };
    auto elseArm = std::make_unique<BinaryExprAST>('+', callFib(1.0), callFib(2.0));
    auto body = std::make_unique<IfExprAST>(std::move(cond), std::make_unique<NumberExprAST>(1.0),
                                            std::move(elseArm));
    auto fib = std::make_unique<FunctionAST>(
        std::make_unique<PrototypeAST>("fib", std::vector<std::string>{"x"}), std::move(body));
    ASSERT_NE(codegen->emitFunction(*fib), nullptr);
    ExitOnErr(theJIT->addModule(codegen->takeModule()));

    auto Sym = ExitOnErr(theJIT->lookup("fib"));
    double (*FP)(double) = Sym.getAddress().toPtr<double (*)(double)>();
    EXPECT_DOUBLE_EQ(FP(10.0), 55.0);
}

//-----------------------------------------------------------------------------
// Hand-built binary expressions, evaluated natively

struct JITASTParam {
    std::string testName;
    char op;
    double lhsVal;
    double rhsVal;
    double expectedResult;
};

class JITASTExecutionTest : public JITTest, public ::testing::WithParamInterface<JITASTParam> {};

TEST_P(JITASTExecutionTest, EvaluateBinaryExpression) {
    auto params = GetParam();

    // Wrap "lhs <op> rhs" in an anonymous function so the JIT can call it
    auto expr = std::make_unique<BinaryExprAST>(params.op,
                                                std::make_unique<NumberExprAST>(params.lhsVal),
                                                std::make_unique<NumberExprAST>(params.rhsVal));
    auto anon = std::make_unique<FunctionAST>(
        std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>{}), std::move(expr));

    EXPECT_DOUBLE_EQ(runAnon(*anon), params.expectedResult);
}

INSTANTIATE_TEST_SUITE_P(
    JITBinaryOperations,
    JITASTExecutionTest,
    ::testing::Values(
        JITASTParam{"Addition", '+', 2.5, 3.5, 6.0},
        JITASTParam{"Subtraction", '-', 10.0, 2.5, 7.5},
        JITASTParam{"Multiplication", '*', 3.0, 4.0, 12.0},
        JITASTParam{"LessThan_True", '<', 1.0, 5.0, 1.0},
        JITASTParam{"LessThan_False", '<', 5.0, 1.0, 0.0}
    ),
    [](const auto& info) { return info.param.testName; }
);

//-----------------------------------------------------------------------------
// Full pipeline: source text -> Lexer -> Parser -> CodeGenSession -> JIT -> double

struct JITTestCase {
    std::string testName;
    std::string expression;
    double expectedResult;
};

class JITExecutionParamTest : public JITTest, public ::testing::WithParamInterface<JITTestCase> {
protected:
    void SetUp() override {
        JITTest::SetUp();
        // The lexer reads stdin (getchar), so redirect it to a file holding the
        // expression. pid suffix: `ctest -j` runs cases of this binary concurrently.
        tmpPath = "_jit_param_input_" + std::to_string(getpid()) + ".txt";
        std::ofstream tmpFile(tmpPath);
        tmpFile << GetParam().expression;
        tmpFile.close();
        ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    }

    void TearDown() override {
        std::remove(tmpPath.c_str());
        JITTest::TearDown();
    }

    std::string tmpPath;
};

TEST_P(JITExecutionParamTest, EvaluateExpression) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();   // prime the parser, as Driver::mainLoop does

    // 1. Parse the expression into an anonymous FunctionAST
    auto ast = parser.parseTopLevelExpr();
    ASSERT_NE(ast, nullptr) << "Failed to parse expression: " << GetParam().expression;

    // 2. Generate IR, JIT it, run it, free it
    EXPECT_DOUBLE_EQ(runAnon(*ast), GetParam().expectedResult)
        << "Mismatch in expression: " << GetParam().expression;
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
        JITTestCase{"NestedIf", "if 1.0 then (if 0.0 then 1.0 else 2.0) else 3.0", 2.0},
        // The end condition is tested after the body with the pre-increment i
        // (do-while semantics), so the body runs for i = 1, 2, 3, 4.
        // The for expression itself always evaluates to 0.0 per the tutorial spec.
        JITTestCase{"ForLoopExecution", "for i = 1.0, i < 4.0, 1.0 in i * 2.0", 0.0},
        JITTestCase{"ForWithoutStep", "for i = 1.0, i < 4.0 in i", 0.0},

        // Chapter 7: mutable variables. 'var/in' scoping and '=' assignment.
        JITTestCase{"VarSimple", "var a = 5.0 in a", 5.0},
        JITTestCase{"VarMultiple", "var a = 1.0, b = 2.0 in a + b", 3.0},
        JITTestCase{"VarDefaultInit", "var a in a + 1.0", 1.0},  // uninitialized vars default to 0.0
        JITTestCase{"AssignReturnsValue", "var x = 0.0 in (x = 42.0)", 42.0},
        JITTestCase{"AssignThenRead", "var a = 1.0 in (a = a + 2.0) + a", 6.0},  // a becomes 3.0, then 3.0 + 3.0
        JITTestCase{"VarInitFromOuter", "var a = 1.0 in (var a = a + 1.0 in a)", 2.0},
        JITTestCase{"DoubleShadowScope", "var a = 10.0 in (var a = 1.0, a = 2.0 in a) + a", 12.0},  // 2 + outer 10
        // Kaleidoscope's for-loop evaluates the end condition AFTER the body, using
        // the pre-increment value, so the body runs for j = 1, 2 AND 3:
        // i accumulates 1 + 2 + 3 = 6, and the for expression itself contributes 0.0.
        JITTestCase{"ForBodyMutation", "var i = 0.0 in ((for j = 1.0, j < 3.0 in (i = i + j)) + i)", 6.0},
        // the loop variable itself may be assigned in the body (the reload before the
        // increment sees it): j jumps to 10, so the loop body runs exactly once
        JITTestCase{"ForVarMutation", "var n = 0.0 in ((for j = 1.0, j < 5.0 in (var t = (n = n + 1.0) in (j = 10.0))) + n)", 1.0}
    ),
    [](const auto& info) { return info.param.testName; }
);

//-----------------------------------------------------------------------------
// User-defined operators end to end: a definition changes how the NEXT input
// parses, then the operator runs as a call into JIT'd code.

struct JITCustomOpTestCase {
    std::string testName;
    std::string setup;        // one `def`
    std::string expression;
    double expectedResult;
};

class JITCustomOperatorParamTest : public JITTest, public ::testing::WithParamInterface<JITCustomOpTestCase> {
protected:
    void SetUp() override {
        JITTest::SetUp();
        tmpPath = "_jit_custom_param_input_" + std::to_string(getpid()) + ".txt";
        std::ofstream tmpFile(tmpPath);
        tmpFile << GetParam().setup << "; " << GetParam().expression;
        tmpFile.close();
        ASSERT_TRUE(freopen(tmpPath.c_str(), "r", stdin) != nullptr);
    }
    void TearDown() override {
        std::remove(tmpPath.c_str());
        JITTest::TearDown();
    }
    std::string tmpPath;
};

TEST_P(JITCustomOperatorParamTest, Evaluate) {
    Lexer lexer;
    Parser parser(lexer);
    parser.getNextToken();   // prime

    // The definition: parsing it installs the operator's precedence in THIS
    // parser; emit it and hand the module to the JIT so it stays resident.
    auto fnAST = parser.parseDefinition();
    ASSERT_NE(fnAST, nullptr) << "Failed to parse setup definition";
    ASSERT_NE(codegen->emitFunction(*fnAST), nullptr) << "Failed to codegen setup definition";
    ExitOnErr(theJIT->addModule(codegen->takeModule()));
    ASSERT_EQ(parser.getCurToken(), ';');
    parser.getNextToken();   // eat ';'

    // The expression, parsed by the same parser with the new operator in place.
    auto ast = parser.parseTopLevelExpr();
    ASSERT_NE(ast, nullptr) << "Failed to parse expression: " << GetParam().expression;
    EXPECT_DOUBLE_EQ(runAnon(*ast), GetParam().expectedResult)
        << "Mismatch in expression: " << GetParam().expression;
}

INSTANTIATE_TEST_SUITE_P(
    CustomOperators,
    JITCustomOperatorParamTest,
    ::testing::Values(
        JITCustomOpTestCase{"Binary", "def binary@ 5 (a b) a + b", "1.0 @ 2.0", 3.0},
        JITCustomOpTestCase{"Unary", "def unary! (a) 0 - a", "!42.0", -42.0},
        // precedence 5 is looser than '+': 1 @ 2 + 3 == 1 @ (2 + 3) == 1 + 5 == 6
        JITCustomOpTestCase{"LoosePrecedence", "def binary@ 5 (a b) a + b", "1.0 @ 2.0 + 3.0", 6.0},
        // precedence 50 is tighter than '+': 2 @ 3 + 1 == (2 @ 3) + 1 == 6 + 1 == 7
        JITCustomOpTestCase{"TightPrecedence", "def binary@ 50 (a b) a * b", "2.0 @ 3.0 + 1.0", 7.0},
        // the operator may be used inside its own body (parser-owned precedence)
        JITCustomOpTestCase{"RecursiveUse", "def binary| 5 (a b) if a then a else (if b then 1 else 0)", "0.0 | 3.0", 1.0}
    ),
    [](const auto& info) { return info.param.testName; }
);
