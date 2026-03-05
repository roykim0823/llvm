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

TEST_F(CodegenTest, CallExprArgumentMismatch) {
    // Callee expects 1 arg, we give 0
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
    std::string expectedResult; // Optional: for verifying constant folding results
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

    // Verify the Binary Expression is correctly folded to the expected result by constant folding
    // (e.g., "3.0" for addition, "-1.0" for subtraction)
    if (params.expectedResult.empty()) {
        // If no expected result is provided, we just check that codegen succeeded
        EXPECT_EQ(V, nullptr);
    } else {
        // For comparison, we check if the generated IR contains the expected constant value
        EXPECT_TRUE(IRToString(V).find(params.expectedResult) != std::string::npos)
            << "Expected result '" << params.expectedResult << "' not found in IR: " << IRToString(V);
    }
}

INSTANTIATE_TEST_SUITE_P(
    OperatorTests,
    BinaryOpTest,
    ::testing::Values(
        BinaryOpParam{'+', "fadd", "3.0"},
        BinaryOpParam{'-', "fsub", "-1.0"},
        BinaryOpParam{'*', "fmul", "2.0"},
        BinaryOpParam{'<', "fcmp", "0"}, // Result of '<' uses fcmp then uitofp
        BinaryOpParam{'?', "", ""} // Invalid operator, should fail codegen and return nullptr
    )
);

//-----------------------------------------------------------------------------
// Optimization Pass Tests: These tests will build specific ASTs that should trigger 
// certain optimizations (like reassociation and common subexpression elimination),
// generate IR, and verify that the
// expected optimizations are present in the generated IR.

class OptimizationPassTest : public CodegenTest {};

// Helper lambdas for cleaner AST construction in tests
static auto num = [](double v) { return std::make_unique<NumberExprAST>(v); };
static auto var = [](const std::string& name) { return std::make_unique<VariableExprAST>(name); };
static auto add = [](std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs) {
    return std::make_unique<BinaryExprAST>('+', std::move(lhs), std::move(rhs));
};
static auto mul = [](std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs) {
    return std::make_unique<BinaryExprAST>('*', std::move(lhs), std::move(rhs));
};

TEST_F(OptimizationPassTest, ReassociatePass) {
    // Test: (x+5) * (5+x) -> Reassociate & InstCombine should each one addtmp
    std::vector<std::string> args = {"x"};
    auto proto = std::make_unique<PrototypeAST>("test_reassoc", std::move(args));
    
    auto body = add(add(var("x"), num(5.0)), add(num(5.0), var("x"))); // (x+5) + (5+x)
    auto function = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

    llvm::Function *F = function->codegen(*ctx);
    ASSERT_NE(F, nullptr);

    std::string irStr = IRToString(F);

    // Count the number of fmul instructions. There should be exactly one.
    size_t faddCount = 0;
    size_t pos = irStr.find("fadd");
    while (pos != std::string::npos) {
        faddCount++;
        pos = irStr.find("fadd", pos + 4);
    }
    
    EXPECT_EQ(faddCount, 2) 
        << "ReassociatePass failed to eliminate the common subexpression. Expected 1 fadd, found " 
        << faddCount << ".\n" << irStr;
}

TEST_F(OptimizationPassTest, CommonSubexpressionElimination) {
    // Test: (x * y) + (x * y) -> GVN should eliminate the redundant multiplication
    std::vector<std::string> args = {"x", "y"};
    auto proto = std::make_unique<PrototypeAST>("test_cse", std::move(args));
    
    auto body = add(mul(var("x"), var("y")), mul(var("x"), var("y")));
    auto function = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

    llvm::Function *F = function->codegen(*ctx);
    ASSERT_NE(F, nullptr);

    std::string irStr = IRToString(F);

    // Count the number of fmul instructions. There should be exactly one.
    size_t fmulCount = 0;
    size_t pos = irStr.find("fmul");
    while (pos != std::string::npos) {
        fmulCount++;
        pos = irStr.find("fmul", pos + 4);
    }
    
    EXPECT_EQ(fmulCount, 1) 
        << "GVN failed to eliminate the common subexpression. Expected 1 fmul, found " 
        << fmulCount << ".\n" << irStr;
}

TEST_F(OptimizationPassTest, ReassociateAndCSECombined) {
    // Test: (1.0 + 2.0 + x) + (x + (1.0 + 2.0))
    // Expected optimization path:
    // 1. InstCombine: (3.0 + x) + (x + 3.0)
    // 2. Reassociate: (x + 3.0) + (x + 3.0) -> addtmp = x + 3.0; addtmp + addtmp    
    std::vector<std::string> args = {"x"};
    auto proto = std::make_unique<PrototypeAST>("test_combined", std::move(args));
    
    // Construct LHS: (1.0 + 2.0) + x
    auto lhs = add(add(num(1.0), num(2.0)), var("x"));
    // Construct RHS: x + (1.0 + 2.0)
    auto rhs = add(var("x"), add(num(1.0), num(2.0)));
    // Outer expression
    auto body = add(std::move(lhs), std::move(rhs));
    
    auto function = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

    llvm::Function *F = function->codegen(*ctx);
    ASSERT_NE(F, nullptr);

    std::string irStr = IRToString(F);

    // 1. Verify the constants folded entirely into 6.0
    EXPECT_TRUE(irStr.find("3.0") != std::string::npos) 
        << "Expected fully folded constant 3.0 in IR:\n" << irStr;

    // 2. Count total arithmetic instructions (should be heavily reduced)
    size_t faddCount = 0;
    size_t pos = irStr.find("fadd");
    while (pos != std::string::npos) { faddCount++; pos = irStr.find("fadd", pos + 4); }

    size_t fmulCount = 0;
    pos = irStr.find("fmul");
    while (pos != std::string::npos) { fmulCount++; pos = irStr.find("fmul", pos + 4); }

    // Unoptimized this would be 5 additions. 
    // Optimized, it should be at most 2 instructions: 
    // either two additions addtmp+addtmp
    EXPECT_LE(faddCount + fmulCount, 2) 
        << "Expected 2 or fewer arithmetic instructions after combined optimization.\n" << irStr;
}


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