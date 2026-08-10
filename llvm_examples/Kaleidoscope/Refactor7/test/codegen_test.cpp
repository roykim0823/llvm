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

    // Create a host function with an entry block and point the builder into it,
    // so expression codegen has a block to insert instructions into. Without
    // this, non-folded instructions (e.g. calls) are created parentless and leak.
    llvm::Function *createInsertionPoint(const char *name = "testHost") {
        llvm::FunctionType *FT =
            llvm::FunctionType::get(llvm::Type::getDoubleTy(*ctx->theContext), false);
        llvm::Function *F = llvm::Function::Create(
            FT, llvm::Function::ExternalLinkage, name, ctx->theModule.get());
        llvm::BasicBlock *BB = llvm::BasicBlock::Create(*ctx->theContext, "entry", F);
        ctx->builder->SetInsertPoint(BB);
        return F;
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
    // ctx->namedValues["a"] = &(*argIt++);
    // ctx->namedValues["b"] = &(*argIt);

    // Helper to create an alloca and store an argument into it
    auto setupVar = [&](std::string name, llvm::Value* arg) {
        // Create the stack slot
        llvm::AllocaInst *alloca = ctx->builder->CreateAlloca(
            llvm::Type::getDoubleTy(*ctx->theContext), nullptr, name);
        // Store the incoming argument value into that slot
        ctx->builder->CreateStore(arg, alloca);
        // Put the alloca (not the argument) into the symbol table
        ctx->namedValues[name] = alloca;
    };

    setupVar("a", &(*argIt++));
    setupVar("b", &(*argIt));

    auto expr = std::make_unique<BinaryExprAST>(params.op, std::move(lhs), std::move(rhs));

    // 4. Generate IR
    llvm::Value *V = expr->codegen(*ctx);
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
        BinaryOpParam{'+', "fadd", 3.0},
        BinaryOpParam{'-', "fsub", -1.0},
        BinaryOpParam{'*', "fmul", 2.0},
        BinaryOpParam{'<', "fcmp", 1.0} // 1.0 < 2.0 folds to 1.0 (true); uses fcmp then uitofp
        // NOTE: an invalid operator like '?' is NOT tested here. Since Chapter 6
        // (user-defined operators), upstream toy.cpp handles an unknown binary op
        // with assert(F && "binary operator not found!"), which aborts in a debug
        // build and is UB in release -- there is no error return to assert on.
        // Kept for fidelity with the tutorial; the v2 refactor should restore
        // logErrorV here and re-enable this case.
    )
);

//-----------------------------------------------------------------------------
struct UnaryOpParam {
    char op;
    std::string expectedCall;
};

class UnaryOpTest : public CodegenTest, public ::testing::WithParamInterface<UnaryOpParam> {};

TEST_P(UnaryOpTest, UnaryOpGen) {
    auto params = GetParam();
    createInsertionPoint();

    // 1. Mock the unary operator function
    std::string funcName = std::string("unary") + params.op;
    auto proto = std::make_unique<PrototypeAST>(funcName, std::vector<std::string>{"v"}, true, 0);
    proto->codegen(*ctx);
    ctx->functionProtos[funcName] = std::move(proto);

    // 2. Create the unary expression
    auto operand = std::make_unique<NumberExprAST>(42.0);
    auto unary = std::make_unique<UnaryExprAST>(params.op, std::move(operand));

    // 3. Verify codegen
    llvm::Value *V = unary->codegen(*ctx);
    ASSERT_NE(V, nullptr);
    EXPECT_TRUE(llvm::isa<llvm::CallInst>(V));
    EXPECT_TRUE(IRToString(V).find(params.expectedCall) != std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    UnaryOperatorTests,
    UnaryOpTest,
    ::testing::Values(
        UnaryOpParam{'!', "unary!"},
        UnaryOpParam{'~', "unary~"}
    )
);

//-----------------------------------------------------------------------------
struct CustomBinOpParam {
    char op;
    unsigned precedence;
    std::string expectedCall;
};

class CustomBinOpTest : public CodegenTest, public ::testing::WithParamInterface<CustomBinOpParam> {};

TEST_P(CustomBinOpTest, CustomBinOpGen) {
    auto params = GetParam();
    createInsertionPoint();

    // 1. Mock the binary operator function
    std::string funcName = std::string("binary") + params.op;
    auto proto = std::make_unique<PrototypeAST>(funcName, std::vector<std::string>{"a", "b"}, true, params.precedence);
    proto->codegen(*ctx);
    ctx->functionProtos[funcName] = std::move(proto);

    // 2. Create the binary expression
    auto lhs = std::make_unique<NumberExprAST>(1.0);
    auto rhs = std::make_unique<NumberExprAST>(2.0);
    auto binop = std::make_unique<BinaryExprAST>(params.op, std::move(lhs), std::move(rhs));

    // 3. Verify codegen
    llvm::Value *V = binop->codegen(*ctx);
    ASSERT_NE(V, nullptr);
    EXPECT_TRUE(llvm::isa<llvm::CallInst>(V));
    EXPECT_TRUE(IRToString(V).find(params.expectedCall) != std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    CustomBinaryOperatorTests,
    CustomBinOpTest,
    ::testing::Values(
        CustomBinOpParam{'@', 30, "binary@"},
        CustomBinOpParam{'|', 5, "binary|"}
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

// if/for IR structure (diamond CFG, phi nodes, loop blocks, if-conversion to
// select) is now verified with lit + FileCheck in test/filecheck/controlflow.k.
// Chapter 7 additionally checks there (test/filecheck/mutablevars.k) that
// mem2reg promotes every alloca out of the optimized IR.

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

//------------------------------------------------------
struct VarExprTestParam {
  std::vector<std::pair<std::string, double>> var;  // VarName and init value
  std::string testName;
};

// 2. Create the Parametric Fixture inheriting from CodegenTest
class VarExprTest : public CodegenTest, public ::testing::WithParamInterface<VarExprTestParam> {
protected:
    llvm::Function *testFunc;

    void SetUp() override {
      CodegenTest::SetUp();
      // Create a dummy function to host the code, with the builder pointed at
      // its entry block. This function is NOT run through the FPM, so the raw
      // alloca/store structure VarExprAST emits is observable.
      testFunc = createInsertionPoint("test_func");
    }
};

TEST_P(VarExprTest, HandleMutableVariable) {
    auto params = GetParam();

    // 1. Pre-fill the symbol table to test shadowing of an existing outer variable
    llvm::AllocaInst* outerAlloca = ctx->builder->CreateAlloca(llvm::Type::getDoubleTy(*ctx->theContext), nullptr, "outer_var");
    ctx->namedValues["a"] = outerAlloca;

    // 2. Build VarExprAST with NumberExprAST initializers
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> varNames;
    for (const auto& p : params.var) {
        varNames.push_back({p.first, std::make_unique<NumberExprAST>(p.second)});
    }

    auto body = std::make_unique<VariableExprAST>(params.var[0].first);
    VarExprAST varNode(std::move(varNames), std::move(body));

    // 3. Execute codegen
    llvm::Value *result = varNode.codegen(*ctx);

    // A. The body is a variable reference, so the result must be a load of its alloca
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(llvm::isa<llvm::LoadInst>(result));

    // B. Symbol table restoration: every binding must return to its pre-codegen
    // state -- "a" back to outerAlloca, names that did not exist before back to
    // their sentinel (the null entry operator[] created when saving OldBindings).
    EXPECT_EQ(ctx->namedValues["a"], outerAlloca) << "Shadowing failed to restore old binding for 'a'";
    for (const auto& p : params.var) {
        if (p.first != "a") {
            EXPECT_EQ(ctx->namedValues[p.first], nullptr)
                << "Binding for '" << p.first << "' leaked out of the var scope";
        }
    }

    // C. Typed IR inspection: one alloca per declared var (plus the outer one),
    // and each initializer value stored exactly once, in declaration order.
    size_t allocaCount = 0;
    std::vector<double> storedInits;
    for (auto &I : testFunc->getEntryBlock()) {
        if (llvm::isa<llvm::AllocaInst>(I))
            ++allocaCount;
        else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
            if (auto *CF = llvm::dyn_cast<llvm::ConstantFP>(SI->getValueOperand()))
                storedInits.push_back(CF->getValueAPF().convertToDouble());
    }
    EXPECT_EQ(allocaCount, params.var.size() + 1) << "one alloca per var (plus outer_var) expected";
    ASSERT_EQ(storedInits.size(), params.var.size()) << "one store per initializer expected";
    for (size_t i = 0; i < params.var.size(); ++i)
        EXPECT_DOUBLE_EQ(storedInits[i], params.var[i].second);

    // D. Entry-block hoisting: the first instruction must be an alloca
    auto &entryBlock = testFunc->getEntryBlock();
    EXPECT_TRUE(llvm::isa<llvm::AllocaInst>(entryBlock.front())) << "Alloca was not hoisted to entry block start";
}

// Instantiate the tests with different scenarios
INSTANTIATE_TEST_SUITE_P(
    VarCodeGenScenarios,
    VarExprTest,
    ::testing::Values(
    // Case 1: Simple shadowing (outer 'a' vs inner 'a')
    VarExprTestParam{{{"a", 10.0}}, "OuterShadowing"},

    // Case 2: Multi-step shadowing (var a=1, a=2 in a)
    VarExprTestParam{{{"a", 1.0}, {"a", 2.0}}, "InternalShadowing"},

    // Case 3: Mixed variables
    VarExprTestParam{{{"x", 5.0}, {"y", 10.0}, {"x", 15.0}}, "MixedInterleaved"},

    // Case 4: Zero initialization
    VarExprTestParam{{{"z", 0.0}}, "ZeroInit"},

    // Case 5: Multi-step shadowing (var a=1, a=2, a=3, a=4 in a)
    VarExprTestParam{{{"a", 1.0}, {"a", 2.0}, {"a", 3}, {"a", 4}}, "InternalShadowingLength4"}
    ),
    [](const ::testing::TestParamInfo<VarExprTestParam>& info) {
        return info.param.testName;
    }
);
