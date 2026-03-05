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