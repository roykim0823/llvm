#include <gtest/gtest.h>

#include "ast.h"
#include "codegen.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;

// All tests go through the public CodeGenSession API: build an AST by hand,
// emit it, and inspect the resulting llvm::Function. Nothing here can reach
// the builder or the symbol table -- those are private to the Impl.
class CodegenTest : public ::testing::Test {
protected:
    CodeGenSession codegen;

    // --- AST builders ---
    static std::unique_ptr<PrototypeAST> makeProto(const std::string &name,
                                                   std::vector<std::string> args = {}) {
        return std::make_unique<PrototypeAST>(name, std::move(args));
    }

    static std::unique_ptr<FunctionAST> makeFunction(const std::string &name,
                                                     std::vector<std::string> args,
                                                     std::unique_ptr<ExprAST> body) {
        return std::make_unique<FunctionAST>(makeProto(name, std::move(args)), std::move(body));
    }

    static std::unique_ptr<ExprAST> num(double v) { return std::make_unique<NumberExprAST>(v); }
    static std::unique_ptr<ExprAST> var(const std::string &n) { return std::make_unique<VariableExprAST>(n); }
    static std::unique_ptr<ExprAST> binop(char op, std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r) {
        return std::make_unique<BinaryExprAST>(op, std::move(l), std::move(r));
    }
    static std::unique_ptr<ExprAST> call(const std::string &callee,
                                         std::vector<std::unique_ptr<ExprAST>> args = {}) {
        return std::make_unique<CallExprAST>(callee, std::move(args));
    }

    // --- IR inspection ---
    // The value returned by a Chapter 3 function: its entry block ends in `ret <value>`.
    static llvm::Value *returnValue(llvm::Function *F) {
        auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(F->getEntryBlock().getTerminator());
        return Ret ? Ret->getReturnValue() : nullptr;
    }

    static std::string IRToString(const llvm::Value *V) {
        std::string s;
        llvm::raw_string_ostream os(s);
        V->print(os);
        return s;
    }

    static std::string blockToString(const llvm::BasicBlock &BB) {
        std::string s;
        llvm::raw_string_ostream os(s);
        BB.print(os);
        return s;
    }
};

//-----------------------------------------------------------------------------
// Expressions (observed through the function that returns them)
//-----------------------------------------------------------------------------

TEST_F(CodegenTest, NumberExprGen) {
    // def f() 42.0  ->  ret double 42.0
    auto fn = makeFunction("f", {}, num(42.0));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);

    auto *ConstFP = llvm::dyn_cast<llvm::ConstantFP>(returnValue(F));
    ASSERT_NE(ConstFP, nullptr);
    EXPECT_DOUBLE_EQ(ConstFP->getValueAPF().convertToDouble(), 42.0);
}

TEST_F(CodegenTest, VariableExprGen) {
    // def f(x) x  ->  the symbol table maps "x" to the function argument
    auto fn = makeFunction("f", {"x"}, var("x"));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);

    llvm::Value *Ret = returnValue(F);
    ASSERT_NE(Ret, nullptr);
    EXPECT_EQ(Ret, F->getArg(0));
    EXPECT_EQ(Ret->getName(), "x");
}

TEST_F(CodegenTest, UnknownVariable) {
    // def f(x) y  ->  "Unknown variable name"; the half-built function is erased
    auto fn = makeFunction("f", {"x"}, var("y"));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
    EXPECT_EQ(codegen.currentModule().getFunction("f"), nullptr);
}

TEST_F(CodegenTest, CallExprGen) {
    // extern calleeFunc(a); def f() calleeFunc(42.0)
    auto proto = makeProto("calleeFunc", {"a"});
    llvm::Function *Callee = codegen.emitPrototype(*proto);
    ASSERT_NE(Callee, nullptr);

    std::vector<std::unique_ptr<ExprAST>> args;
    args.push_back(num(42.0));
    auto fn = makeFunction("f", {}, call("calleeFunc", std::move(args)));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);

    auto *Call = llvm::dyn_cast<llvm::CallInst>(returnValue(F));
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getCalledFunction(), Callee);
    EXPECT_EQ(Call->arg_size(), 1u);
}

TEST_F(CodegenTest, CallExprUnknownFunction) {
    // No prototype registered in this session: hits the "Unknown function referenced" path
    auto fn = makeFunction("f", {}, call("calleeFunc"));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
}

TEST_F(CodegenTest, CallExprArgumentMismatch) {
    // Callee expects 1 arg, we give 0: hits the "Incorrect # arguments passed" path
    auto proto = makeProto("calleeFunc", {"a"});
    ASSERT_NE(codegen.emitPrototype(*proto), nullptr);

    auto fn = makeFunction("f", {}, call("calleeFunc"));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
}

//-----------------------------------------------------------------------------
// Prototypes and functions
//-----------------------------------------------------------------------------

TEST_F(CodegenTest, PrototypeGen) {
    // extern cos(x)  ->  declare double @cos(double %x)
    auto proto = makeProto("cos", {"x"});
    llvm::Function *F = codegen.emitPrototype(*proto);
    ASSERT_NE(F, nullptr);

    EXPECT_TRUE(F->empty()); // a declaration: no basic blocks
    EXPECT_EQ(F->arg_size(), 1u);
    EXPECT_EQ(F->getArg(0)->getName(), "x");
    EXPECT_TRUE(F->getReturnType()->isDoubleTy());
    EXPECT_EQ(codegen.currentModule().getFunction("cos"), F);
}

TEST_F(CodegenTest, FunctionGen) {
    // def testFunc(x) x + 1.0
    auto fn = makeFunction("testFunc", {"x"}, binop('+', var("x"), num(1.0)));
    llvm::Function *F = codegen.emitFunction(*fn);

    ASSERT_NE(F, nullptr);
    EXPECT_FALSE(F->empty()); // Should have basic blocks

    // Check for the "entry" block and a return instruction
    auto &BB = F->getEntryBlock();
    EXPECT_EQ(BB.getName(), "entry");
    EXPECT_TRUE(llvm::isa<llvm::ReturnInst>(BB.getTerminator()));
}

TEST_F(CodegenTest, ExternThenDefinition) {
    // extern foo(x); def foo(x) x  ->  the definition fills in the declaration
    auto proto = makeProto("foo", {"x"});
    llvm::Function *Decl = codegen.emitPrototype(*proto);
    ASSERT_NE(Decl, nullptr);
    EXPECT_TRUE(Decl->empty());

    auto fn = makeFunction("foo", {"x"}, var("x"));
    llvm::Function *Def = codegen.emitFunction(*fn);
    EXPECT_EQ(Def, Decl);      // same llvm::Function, reused rather than recreated
    EXPECT_FALSE(Decl->empty());
}

TEST_F(CodegenTest, FunctionRedefinition) {
    // def good(x) x; def good(x) x + 1  ->  "Function cannot be redefined."
    auto first = makeFunction("good", {"x"}, var("x"));
    llvm::Function *F = codegen.emitFunction(*first);
    ASSERT_NE(F, nullptr);

    auto second = makeFunction("good", {"x"}, binop('+', var("x"), num(1.0)));
    EXPECT_EQ(codegen.emitFunction(*second), nullptr);

    // The original definition is untouched.
    EXPECT_EQ(codegen.currentModule().getFunction("good"), F);
    EXPECT_FALSE(F->empty());
}

TEST_F(CodegenTest, EraseFunctionRemovesIt) {
    // The driver discards each top-level expression this way.
    auto fn = makeFunction("__anon_expr", {}, num(1.0));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    codegen.eraseFunction(F);
    EXPECT_EQ(codegen.currentModule().getFunction("__anon_expr"), nullptr);

    // ...and the name is free again for the next one.
    auto again = makeFunction("__anon_expr", {}, num(2.0));
    ASSERT_NE(codegen.emitFunction(*again), nullptr);
}

//-----------------------------------------------------------------------------
// Binary operators
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

    // def tmp(a b) a <op> b  -- arguments are non-constant, so no folding happens
    auto fn = makeFunction("tmp", {"a", "b"}, binop(params.op, var("a"), var("b")));
    llvm::Function *F = codegen.emitFunction(*fn);

    if (!params.valid) {
        // Invalid operator: codegen must fail cleanly and leave nothing behind
        EXPECT_EQ(F, nullptr);
        EXPECT_EQ(codegen.currentModule().getFunction("tmp"), nullptr);
        return;
    }
    ASSERT_NE(F, nullptr);

    // Stringify the entire entry block to see all generated instructions
    // (for '<' that is 'fcmp' AND 'uitofp').
    std::string bbStr = blockToString(F->getEntryBlock());
    EXPECT_TRUE(bbStr.find(params.expectedInstr) != std::string::npos)
        << "Expected instruction '" << params.expectedInstr << "' not found in IR: " << bbStr;
}

TEST_P(BinaryOpTest, BinaryOpResult) {
    auto params = GetParam();

    // def tmp() 1.0 <op> 2.0
    auto fn = makeFunction("tmp", {}, binop(params.op, num(1.0), num(2.0)));
    llvm::Function *F = codegen.emitFunction(*fn);

    if (!params.valid) {
        // Invalid operator: codegen must fail cleanly
        EXPECT_EQ(F, nullptr);
        return;
    }
    ASSERT_NE(F, nullptr);

    // "1.0 <op> 2.0" is constant-folded by IRBuilder; check the folded value
    // itself instead of substring-matching LLVM's textual float format.
    llvm::Value *V = returnValue(F);
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
