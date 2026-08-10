#include <gtest/gtest.h>
#include "ast.h"
#include "ir_gen_ctx.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;

class CodegenTest : public ::testing::Test {
protected:
    std::unique_ptr<IRGenContext> ctx;

    void SetUp() override {
        ctx = std::make_unique<IRGenContext>();
    }

    // Helper to convert LLVM value to string for comparison
    std::string IRToString(llvm::Value* V) {
        std::string s;
        llvm::raw_string_ostream os(s);
        V->print(os);
        return s;
    }
};

TEST_F(CodegenTest, NumberExprGen) {
    auto expr = std::make_unique<NumberExprAST>(42.0);
    llvm::Value *V = expr->codegen(*ctx);

    ASSERT_NE(V, nullptr);
    EXPECT_TRUE(llvm::isa<llvm::ConstantFP>(V));

    // Check if the value is actually 42.0
    auto *ConstFP = llvm::cast<llvm::ConstantFP>(V);
    EXPECT_DOUBLE_EQ(ConstFP->getValueAPF().convertToDouble(), 42.0);
}


TEST_F(CodegenTest, CallExprGen) {
    // 1. Mock a function in the module to call
    std::vector<std::string> argNames = {"a"};
    auto proto = std::make_unique<PrototypeAST>("calleeFunc", std::move(argNames));
    proto->codegen(*ctx);

    // 2. Create the call expression
    std::vector<std::unique_ptr<ExprAST>> args;
    args.push_back(std::make_unique<NumberExprAST>(42.0));
    auto call = std::make_unique<CallExprAST>("calleeFunc", std::move(args));

    // 3. Verify codegen
    llvm::Value *V = call->codegen(*ctx);
    ASSERT_NE(V, nullptr);
    EXPECT_TRUE(llvm::isa<llvm::CallInst>(V));
}

TEST_F(CodegenTest, CallExprUnknownFunction) {
    // No prototype registered in this context: hits the "Unknown function referenced" path
    auto call = std::make_unique<CallExprAST>("calleeFunc", std::vector<std::unique_ptr<ExprAST>>());
    EXPECT_EQ(call->codegen(*ctx), nullptr);
}

TEST_F(CodegenTest, CallExprArgumentMismatch) {
    // Callee expects 1 arg, we give 0: hits the "Incorrect # arguments passed" path
    std::vector<std::string> argNames = {"a"};
    auto proto = std::make_unique<PrototypeAST>("calleeFunc", std::move(argNames));
    ASSERT_NE(proto->codegen(*ctx), nullptr);

    auto call = std::make_unique<CallExprAST>("calleeFunc", std::vector<std::unique_ptr<ExprAST>>());
    EXPECT_EQ(call->codegen(*ctx), nullptr);
}

TEST_F(CodegenTest, FunctionGen) {
    // Create Prototype: double test(x)
    std::vector<std::string> args = {"x"};
    auto proto = std::make_unique<PrototypeAST>("testFunc", std::move(args));

    // Create Body: return x + 1.0
    auto lhs = std::make_unique<VariableExprAST>("x");
    auto rhs = std::make_unique<NumberExprAST>(1.0);
    auto body = std::make_unique<BinaryExprAST>('+', std::move(lhs), std::move(rhs));

    auto function = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

    llvm::Function *F = function->codegen(*ctx);

    ASSERT_NE(F, nullptr);
    EXPECT_FALSE(F->empty()); // Should have basic blocks

    // Check for the "entry" block and a return instruction
    auto &BB = F->getEntryBlock();
    EXPECT_EQ(BB.getName(), "entry");
    EXPECT_TRUE(llvm::isa<llvm::ReturnInst>(BB.getTerminator()));
}


//-----------------------------------------------------------------------------
struct BinaryOpParam {
    char op;
    std::string expectedInstr;
    bool valid;          // false: codegen must fail and return nullptr
    double expectedFold; // expected result of constant-folding "1.0 <op> 2.0"
};

class BinaryOpTest : public CodegenTest, public ::testing::WithParamInterface<BinaryOpParam> {};

TEST_P(BinaryOpTest, GeneratedIRInst) {
    auto params = GetParam();

    // 1. Create a dummy function to provide non-constant arguments
    std::vector<llvm::Type*> argTypes(2, llvm::Type::getDoubleTy(*ctx->theContext));
    llvm::FunctionType *FT = llvm::FunctionType::get(llvm::Type::getDoubleTy(*ctx->theContext), argTypes, false);
    llvm::Function *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "tmp", ctx->theModule.get());
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(*ctx->theContext, "entry", F);
    ctx->builder->SetInsertPoint(BB);

    // 2. Build AST using variables: "a <op> b"
    auto lhs = std::make_unique<VariableExprAST>("a");
    auto rhs = std::make_unique<VariableExprAST>("b");

    // 3. Manually map "a" and "b" to the function's arguments in our context
    auto argIt = F->arg_begin();
    ctx->namedValues["a"] = &(*argIt++);
    ctx->namedValues["b"] = &(*argIt);

    auto expr = std::make_unique<BinaryExprAST>(params.op, std::move(lhs), std::move(rhs));

    // 4. Generate IR
    llvm::Value *V = expr->codegen(*ctx);

    if (!params.valid) {
        // Invalid operator: codegen must fail cleanly
        EXPECT_EQ(V, nullptr);
        return;
    }
    ASSERT_NE(V, nullptr);

    // 5. FIX: Stringify the entire Basic Block to see all generated instructions
    std::string bbStr;
    llvm::raw_string_ostream os(bbStr);
    BB->print(os); // This prints the entire block, including 'fcmp' AND 'uitofp'

    EXPECT_TRUE(bbStr.find(params.expectedInstr) != std::string::npos)
        << "Expected instruction '" << params.expectedInstr << "' not found in IR: " << bbStr;
}

TEST_P(BinaryOpTest, BinaryOpResult) {
    auto params = GetParam();
    auto lhs = std::make_unique<NumberExprAST>(1.0);
    auto rhs = std::make_unique<NumberExprAST>(2.0);
    auto expr = std::make_unique<BinaryExprAST>(params.op, std::move(lhs), std::move(rhs));

    llvm::Value *V = expr->codegen(*ctx);

    if (!params.valid) {
        // Invalid operator: codegen must fail cleanly
        EXPECT_EQ(V, nullptr);
        return;
    }
    ASSERT_NE(V, nullptr);

    // "1.0 <op> 2.0" is constant-folded by IRBuilder; check the folded value
    // itself instead of substring-matching LLVM's textual float format.
    auto *CF = llvm::dyn_cast<llvm::ConstantFP>(V);
    ASSERT_NE(CF, nullptr) << "expected a folded constant, got: " << IRToString(V);
    EXPECT_DOUBLE_EQ(CF->getValueAPF().convertToDouble(), params.expectedFold);
}

INSTANTIATE_TEST_SUITE_P(
    OperatorTests,
    BinaryOpTest,
    ::testing::Values(
        BinaryOpParam{'+', "fadd", true, 3.0},
        BinaryOpParam{'-', "fsub", true, -1.0},
        BinaryOpParam{'*', "fmul", true, 2.0},
        BinaryOpParam{'<', "fcmp", true, 1.0}, // 1.0 < 2.0 folds to 1.0 (true); uses fcmp then uitofp
        BinaryOpParam{'?', "", false, 0.0}     // Invalid operator, must fail codegen and return nullptr
    )
);

//-----------------------------------------------------------------------------
// Optimization pass behavior (CSE, reassociation, constant folding) is now
// verified with lit + FileCheck in test/filecheck/opt.k, where instruction
// counts and def-use structure can be expressed directly (CHECK / CHECK-NEXT /
// CHECK-NOT) instead of substring-counting loops over the printed IR.


TEST_F(CodegenTest, JIT) {
    // Test case: def add_two(x) x + 2.0
    std::vector<std::string> args = {"x"};
    auto proto = std::make_unique<PrototypeAST>("add_two", std::move(args));

    auto varX = std::make_unique<VariableExprAST>("x");
    auto num2 = std::make_unique<NumberExprAST>(2.0);
    auto body = std::make_unique<BinaryExprAST>('+', std::move(varX), std::move(num2));

    auto function = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

    llvm::Function *F = function->codegen(*ctx);
    ASSERT_NE(F, nullptr);

    // Add function module to JIT
    auto TSM = llvm::orc::ThreadSafeModule(std::move(ctx->theModule), std::move(ctx->theContext));
    ctx->ExitOnErr(ctx->theJIT->addModule(std::move(TSM)));
    ctx->InitializeModuleAndPassManager();

    // Look up the defined function
    auto ExprSymbol = ctx->ExitOnErr(ctx->theJIT->lookup("add_two"));

    // Cast to a function pointer that takes a double and returns a double
    double (*FP)(double) = ExprSymbol.getAddress().toPtr<double (*)(double)>();

    // Execute with an argument
    EXPECT_DOUBLE_EQ(FP(5.5), 7.5);
}

//-----------------------------------------------------------------------------
// JIT AST Execution Tests: These tests will build simple ASTs, generate IR,
// and execute them in the JIT to verify correct results at runtime.

// 1. Define the parameters for the JIT AST execution tests
struct JITASTParam {
    std::string testName;
    char op;
    double lhsVal;
    double rhsVal;
    double expectedResult;
};

// 2. Create the Parametric Fixture inheriting from CodegenTest
class JITASTExecutionTest : public CodegenTest, public ::testing::WithParamInterface<JITASTParam> {};

// 3. The actual test body
TEST_P(JITASTExecutionTest, EvaluateBinaryExpression) {
    auto params = GetParam();

    // Step A: Manually build the AST for "lhs <op> rhs"
    auto lhs = std::make_unique<NumberExprAST>(params.lhsVal);
    auto rhs = std::make_unique<NumberExprAST>(params.rhsVal);
    auto expr = std::make_unique<BinaryExprAST>(params.op, std::move(lhs), std::move(rhs));

    // Wrap the expression in an anonymous function prototype so the JIT can call it
    auto proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
    auto function = std::make_unique<FunctionAST>(std::move(proto), std::move(expr));

    // Step B: Generate LLVM IR
    llvm::Function *F = function->codegen(*ctx);
    ASSERT_NE(F, nullptr) << "Codegen failed for operator: " << params.op;

    // Step C: Execute in JIT
    // Create a ResourceTracker to track and free the memory for this specific expression
    auto RT = ctx->theJIT->getMainJITDylib().createResourceTracker();

    // Package the module and context, then hand it to the JIT
    auto TSM = llvm::orc::ThreadSafeModule(std::move(ctx->theModule), std::move(ctx->theContext));
    ctx->ExitOnErr(ctx->theJIT->addModule(std::move(TSM), RT));

    // Immediately re-initialize the context's module and pass manager to maintain state integrity
    ctx->InitializeModuleAndPassManager();

    // Look up the compiled symbol
    auto ExprSymbol = ctx->ExitOnErr(ctx->theJIT->lookup("__anon_expr"));

    // Cast the symbol address to a callable C++ function pointer
    double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
    
    // Execute and verify
    EXPECT_DOUBLE_EQ(FP(), params.expectedResult);

    // Clean up JIT memory
    ctx->ExitOnErr(RT->remove());
}

// 4. Instantiate the test suite with various math operations
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
    [](const ::testing::TestParamInfo<JITASTParam>& info) {
        return info.param.testName;
    }
);