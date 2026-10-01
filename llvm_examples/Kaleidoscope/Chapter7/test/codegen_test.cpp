#include <gtest/gtest.h>

#include "ast.h"
#include "codegen.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;

// All tests go through the public CodeGenSession API: build an AST by hand,
// emit it, and inspect the resulting llvm::Function. Nothing here can reach
// the builder, the symbol table or the pass managers -- those are private to
// the Impl. Note that Chapter 4 runs the optimization pipeline on every
// function, so these tests see *optimized* IR (fine for what they check;
// pass-specific shapes are pinned by test/filecheck/opt.k). Since Chapter 7
// that pipeline starts with mem2reg, so the allocas the frontend emits are
// never visible here: variable semantics are checked through what the
// promoted IR computes (test/filecheck/mutablevars.k pins that no alloca
// survives). Everything that needs a JIT lives in jit_test.cpp.
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

TEST_F(CodegenTest, TakeModuleOpensFreshModule) {
    // After takeModule() the session works on a brand-new, empty module.
    auto fn = makeFunction("f", {"x"}, var("x"));
    ASSERT_NE(codegen.emitFunction(*fn), nullptr);
    llvm::Module *before = &codegen.currentModule();
    EXPECT_NE(before->getFunction("f"), nullptr);

    llvm::orc::ThreadSafeModule tsm = codegen.takeModule();
    EXPECT_TRUE(static_cast<bool>(tsm));               // we got the old module
    tsm.withModuleDo([](llvm::Module &m) { EXPECT_NE(m.getFunction("f"), nullptr); });

    llvm::Module &after = codegen.currentModule();
    EXPECT_NE(&after, before);
    EXPECT_EQ(after.getFunction("f"), nullptr);        // fresh module: nothing in it yet
    EXPECT_TRUE(after.empty());
}

TEST_F(CodegenTest, CrossModuleCallRedeclares) {
    // def f(x) x;  <take>  def g() f(1.0)
    // f lives in an earlier module; the call site gets a fresh declaration
    // from the prototype registry.
    auto f = makeFunction("f", {"x"}, var("x"));
    ASSERT_NE(codegen.emitFunction(*f), nullptr);
    codegen.takeModule();

    std::vector<std::unique_ptr<ExprAST>> args;
    args.push_back(num(1.0));
    auto g = makeFunction("g", {}, call("f", std::move(args)));
    llvm::Function *G = codegen.emitFunction(*g);
    ASSERT_NE(G, nullptr);

    llvm::Function *FDecl = codegen.currentModule().getFunction("f");
    ASSERT_NE(FDecl, nullptr);
    EXPECT_TRUE(FDecl->empty());                       // a declaration, not a copy of the body
    EXPECT_EQ(FDecl->arg_size(), 1u);
    auto *Call = llvm::dyn_cast<llvm::CallInst>(returnValue(G));
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getCalledFunction(), FDecl);
}

TEST_F(CodegenTest, ExternSurvivesTakeModule) {
    // extern cos(x);  <take>  def g(x) cos(x)  -- the extern's prototype is registered too
    auto proto = makeProto("cos", {"x"});
    ASSERT_NE(codegen.emitPrototype(*proto), nullptr);
    codegen.takeModule();

    std::vector<std::unique_ptr<ExprAST>> args;
    args.push_back(var("x"));
    auto g = makeFunction("g", {"x"}, call("cos", std::move(args)));
    ASSERT_NE(codegen.emitFunction(*g), nullptr);
    llvm::Function *CosDecl = codegen.currentModule().getFunction("cos");
    ASSERT_NE(CosDecl, nullptr);
    EXPECT_TRUE(CosDecl->empty());
}

TEST_F(CodegenTest, RedefinitionInFreshModuleIsAccepted) {
    // Chapter 3 refused `def good(x) ...` twice. Chapter 4 drops that guard:
    // each definition lives in its own module, so codegen happily builds the
    // second one (the JIT, not codegen, is what rejects the duplicate symbol).
    auto first = makeFunction("good", {"x"}, var("x"));
    ASSERT_NE(codegen.emitFunction(*first), nullptr);
    codegen.takeModule();

    auto second = makeFunction("good", {"x"}, binop('+', var("x"), num(1.0)));
    llvm::Function *F = codegen.emitFunction(*second);
    ASSERT_NE(F, nullptr);
    EXPECT_FALSE(F->empty());
    EXPECT_EQ(codegen.currentModule().getFunction("good"), F);
}

TEST_F(CodegenTest, NewestPrototypeWins) {
    // extern foo(a);  <take>  def foo(b) b  -- the definition's own argument
    // names are used (Chapter 3's extern-then-def name bug is gone).
    auto proto = makeProto("foo", {"a"});
    ASSERT_NE(codegen.emitPrototype(*proto), nullptr);
    codegen.takeModule();

    auto def = makeFunction("foo", {"b"}, var("b"));
    llvm::Function *F = codegen.emitFunction(*def);
    ASSERT_NE(F, nullptr);
    EXPECT_EQ(F->getArg(0)->getName(), "b");
    EXPECT_EQ(returnValue(F), F->getArg(0));
}

TEST_F(CodegenTest, FailedBodyIsNotRegistered) {
    // def f(x) y;  <take>  def g() f(1.0)
    // f's body failed, so f is not in the registry: the call is an ordinary
    // "Unknown function referenced" (nullptr), not a dangling declaration.
    auto bad = makeFunction("f", {"x"}, var("y"));
    EXPECT_EQ(codegen.emitFunction(*bad), nullptr);
    codegen.takeModule();

    std::vector<std::unique_ptr<ExprAST>> args;
    args.push_back(num(1.0));
    auto g = makeFunction("g", {}, call("f", std::move(args)));
    EXPECT_EQ(codegen.emitFunction(*g), nullptr);
    EXPECT_EQ(codegen.currentModule().getFunction("f"), nullptr);
}

TEST_F(CodegenTest, OptimizerRunsOnEveryFunction) {
    // def f(x y) x*y + x*y  -- GVN dedups the two multiplies (the shape opt.k pins textually)
    auto body = binop('+', binop('*', var("x"), var("y")), binop('*', var("x"), var("y")));
    auto fn = makeFunction("f", {"x", "y"}, std::move(body));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);

    unsigned fmuls = 0;
    for (auto &I : F->getEntryBlock())
        if (auto *B = llvm::dyn_cast<llvm::BinaryOperator>(&I); B && B->getOpcode() == llvm::Instruction::FMul)
            ++fmuls;
    EXPECT_EQ(fmuls, 1u);
}

//-----------------------------------------------------------------------------
// Control flow (Chapter 5). The CFG shapes themselves are pinned textually in
// test/filecheck/controlflow.k; here we check what the API sees.

TEST_F(CodegenTest, IfWithOpaqueArmsKeepsDiamondAndPhi) {
    // extern foo(); extern bar(); def f(x) if x then foo() else bar()
    // Calls may have side effects, so SimplifyCFG cannot if-convert: the merge
    // block still carries the phi, and it is what the function returns.
    ASSERT_NE(codegen.emitPrototype(*makeProto("foo")), nullptr);
    ASSERT_NE(codegen.emitPrototype(*makeProto("bar")), nullptr);
    auto body = std::make_unique<IfExprAST>(var("x"), call("foo"), call("bar"));
    auto fn = makeFunction("f", {"x"}, std::move(body));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);

    EXPECT_EQ(F->size(), 4u);   // entry, then, else, ifcont
    llvm::BasicBlock &Merge = F->back();
    EXPECT_EQ(Merge.getName(), "ifcont");
    auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Merge.getTerminator());
    ASSERT_NE(Ret, nullptr);
    EXPECT_TRUE(llvm::isa<llvm::PHINode>(Ret->getReturnValue()));
}

TEST_F(CodegenTest, ForRestoresShadowedVariable) {
    // def f(i) (for i = 1, i < 3 in i) + i
    // Inside the loop `i` is the phi; afterwards it must refer to the argument
    // again, so the sum is 0.0 (the loop's value) + %i (the argument).
    auto loop = std::make_unique<ForExprAST>("i", num(1.0), binop('<', var("i"), num(3.0)),
                                             nullptr, var("i"));
    auto fn = makeFunction("f", {"i"}, binop('+', std::move(loop), var("i")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);

    // (x + 0.0 is not an identity for -0.0, so no pass folds it: the function
    // returns `fadd %i, 0.0` with %i the ARGUMENT -- not the loop's phi.)
    auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(F->back().getTerminator());
    ASSERT_NE(Ret, nullptr);
    auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(Ret->getReturnValue());
    ASSERT_NE(Add, nullptr);
    EXPECT_EQ(Add->getOpcode(), llvm::Instruction::FAdd);
    EXPECT_TRUE(Add->getOperand(0) == F->getArg(0) || Add->getOperand(1) == F->getArg(0));
    EXPECT_FALSE(llvm::isa<llvm::PHINode>(Add->getOperand(0)) || llvm::isa<llvm::PHINode>(Add->getOperand(1)));
}

TEST_F(CodegenTest, ForBodyErrorLeavesNothingBehind) {
    // def f(n) for i = 1, i < n in y   -- unknown variable inside the loop body
    auto loop = std::make_unique<ForExprAST>("i", num(1.0), binop('<', var("i"), var("n")),
                                             nullptr, var("y"));
    auto fn = makeFunction("f", {"n"}, std::move(loop));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
    EXPECT_EQ(codegen.currentModule().getFunction("f"), nullptr);
}

//-----------------------------------------------------------------------------
// Mutable variables (Chapter 7). Allocas are promoted before we can see them,
// so each test asks what the function RETURNS after mem2reg.

static std::unique_ptr<ExprAST> varIn(std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars,
                                      std::unique_ptr<ExprAST> body) {
    return std::make_unique<VarExprAST>(std::move(vars), std::move(body));
}

TEST_F(CodegenTest, VarInitializerAndRead) {
    // def f(x) var a = x in a   ->  returns x itself once the alloca is promoted
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("a", var("x"));
    auto fn = makeFunction("f", {"x"}, varIn(std::move(vars), var("a")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    EXPECT_EQ(returnValue(F), F->getArg(0));
    for (auto &BB : *F) for (auto &I : BB) EXPECT_FALSE(llvm::isa<llvm::AllocaInst>(I));
}

TEST_F(CodegenTest, VarDefaultsToZero) {
    // def f() var a in a   ->  ret 0.0
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("a", nullptr);
    auto fn = makeFunction("f", {}, varIn(std::move(vars), var("a")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *CF = llvm::dyn_cast<llvm::ConstantFP>(returnValue(F));
    ASSERT_NE(CF, nullptr);
    EXPECT_DOUBLE_EQ(CF->getValueAPF().convertToDouble(), 0.0);
}

TEST_F(CodegenTest, AssignmentYieldsValueAndStores) {
    // def f(x) var a = 0 in (a = x * 2)   ->  the assignment's value: fmul x, 2
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("a", num(0.0));
    auto body = binop('=', var("a"), binop('*', var("x"), num(2.0)));
    auto fn = makeFunction("f", {"x"}, varIn(std::move(vars), std::move(body)));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Mul = llvm::dyn_cast<llvm::BinaryOperator>(returnValue(F));
    ASSERT_NE(Mul, nullptr);
    EXPECT_EQ(Mul->getOpcode(), llvm::Instruction::FMul);
}

TEST_F(CodegenTest, ParameterIsMutable) {
    // def f(x) var t = (x = x * 2) in x   ->  x read after the assignment: fmul
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("t", binop('=', var("x"), binop('*', var("x"), num(2.0))));
    auto fn = makeFunction("f", {"x"}, varIn(std::move(vars), var("x")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Mul = llvm::dyn_cast<llvm::BinaryOperator>(returnValue(F));
    ASSERT_NE(Mul, nullptr);
    EXPECT_EQ(Mul->getOpcode(), llvm::Instruction::FMul);
}

TEST_F(CodegenTest, AssignmentToNonVariableIsAnError) {
    // def f(x) (x + 1) = 2   ->  "destination of '=' must be a variable" (upstream: UB)
    auto fn = makeFunction("f", {"x"}, binop('=', binop('+', var("x"), num(1.0)), num(2.0)));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
    EXPECT_EQ(codegen.currentModule().getFunction("f"), nullptr);
}

TEST_F(CodegenTest, AssignmentToUnknownVariableIsAnError) {
    auto fn = makeFunction("f", {"x"}, binop('=', var("y"), num(2.0)));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
}

TEST_F(CodegenTest, VarInitializerSeesOuterBinding) {
    // def f(a) var a = a + 1 in a   ->  the inner a is initialized from the ARGUMENT: fadd %a, 1
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("a", binop('+', var("a"), num(1.0)));
    auto fn = makeFunction("f", {"a"}, varIn(std::move(vars), var("a")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(returnValue(F));
    ASSERT_NE(Add, nullptr);
    EXPECT_TRUE(Add->getOperand(0) == F->getArg(0) || Add->getOperand(1) == F->getArg(0));
}

// Scope restoration. With the upstream (forward-order) restore loop, a name
// declared twice in one `var` leaks its FIRST inner binding out of the scope:
// the trailing `a` below would read the inner 1.0 and the whole function would
// fold to a constant. With reverse-order restoration it reads the argument.
TEST_F(CodegenTest, VarScopeRestoresArgumentAfterDoubleShadow) {
    // def f(a) (var a = 1, a = 2 in a) + a   ->  2.0 + %a (argument), not 2.0 + 1.0
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("a", num(1.0));
    vars.emplace_back("a", num(2.0));
    auto fn = makeFunction("f", {"a"}, binop('+', varIn(std::move(vars), var("a")), var("a")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(returnValue(F));
    ASSERT_NE(Add, nullptr) << "folded to a constant: the inner binding leaked";
    EXPECT_TRUE(Add->getOperand(0) == F->getArg(0) || Add->getOperand(1) == F->getArg(0));
}

TEST_F(CodegenTest, VarScopeRestoresArgumentAfterChainOfFour) {
    // def f(a) (var a = 1, a = 2, a = 3, a = 4 in a) + a   ->  4.0 + %a
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    for (double v : {1.0, 2.0, 3.0, 4.0}) vars.emplace_back("a", num(v));
    auto fn = makeFunction("f", {"a"}, binop('+', varIn(std::move(vars), var("a")), var("a")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(returnValue(F));
    ASSERT_NE(Add, nullptr) << "folded to a constant: an inner binding leaked";
    EXPECT_TRUE(Add->getOperand(0) == F->getArg(0) || Add->getOperand(1) == F->getArg(0));
}

TEST_F(CodegenTest, VarScopeUnbindsNewNames) {
    // def f(x) (var b = 1 in b) + b   ->  after the var expr, b is unknown again
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> vars;
    vars.emplace_back("b", num(1.0));
    auto fn = makeFunction("f", {"x"}, binop('+', varIn(std::move(vars), var("b")), var("b")));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
}

//-----------------------------------------------------------------------------
// User-defined operators (Chapter 6): desugared into calls

TEST_F(CodegenTest, UnaryOperatorDesugarsToCall) {
    // extern unary!(v);  then  def f(x) !x
    auto proto = std::make_unique<PrototypeAST>("unary!", std::vector<std::string>{"v"}, true, 0);
    llvm::Function *Op = codegen.emitPrototype(*proto);
    ASSERT_NE(Op, nullptr);

    auto fn = makeFunction("f", {"x"}, std::make_unique<UnaryExprAST>('!', var("x")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Call = llvm::dyn_cast<llvm::CallInst>(returnValue(F));
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getCalledFunction(), Op);
    EXPECT_EQ(Call->getCalledFunction()->getName(), "unary!");
}

TEST_F(CodegenTest, UnknownUnaryOperatorIsAnError) {
    auto fn = makeFunction("f", {"x"}, std::make_unique<UnaryExprAST>('~', var("x")));
    EXPECT_EQ(codegen.emitFunction(*fn), nullptr);
}

TEST_F(CodegenTest, UserBinaryOperatorDesugarsToCall) {
    // extern binary@ 50 (a b);  then  def f(a b) a @ b   ('@' is not a builtin)
    auto proto = std::make_unique<PrototypeAST>("binary@", std::vector<std::string>{"a", "b"}, true, 50);
    llvm::Function *Op = codegen.emitPrototype(*proto);
    ASSERT_NE(Op, nullptr);

    auto fn = makeFunction("f", {"a", "b"}, binop('@', var("a"), var("b")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    auto *Call = llvm::dyn_cast<llvm::CallInst>(returnValue(F));
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getCalledFunction(), Op);
    EXPECT_EQ(Call->arg_size(), 2u);
}

TEST_F(CodegenTest, BuiltinOperatorsStayInline) {
    // Even with a "binary+" declared, '+' is a builtin: fadd, not a call.
    auto proto = std::make_unique<PrototypeAST>("binary+", std::vector<std::string>{"a", "b"}, true, 20);
    ASSERT_NE(codegen.emitPrototype(*proto), nullptr);
    auto fn = makeFunction("f", {"a", "b"}, binop('+', var("a"), var("b")));
    llvm::Function *F = codegen.emitFunction(*fn);
    ASSERT_NE(F, nullptr);
    EXPECT_TRUE(llvm::isa<llvm::BinaryOperator>(returnValue(F)));
}

TEST_F(CodegenTest, EraseFunctionRemovesItAndItsCachedAnalyses) {
    // Erase an optimized function, then emit more functions in the same module
    // (repeatedly, so the freed Function* is likely to be reused). Any analysis
    // the pass managers cached for the erased function must have gone with it;
    // if it dangles, mem2reg on the newcomer reads a stale dominator tree.
    for (int i = 0; i < 50; ++i) {
        auto anon = makeFunction("__anon_expr", {}, num(1.0));
        llvm::Function *F = codegen.emitFunction(*anon);
        ASSERT_NE(F, nullptr);
        codegen.eraseFunction(F);
        EXPECT_EQ(codegen.currentModule().getFunction("__anon_expr"), nullptr);

        // a function with a real CFG, so the passes actually consult analyses
        auto body = std::make_unique<IfExprAST>(var("x"), num(1.0), binop('+', var("x"), num(2.0)));
        auto g = makeFunction("g" + std::to_string(i), {"x"}, std::move(body));
        ASSERT_NE(codegen.emitFunction(*g), nullptr);
    }
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
        BinaryOpParam{'?', "", false, 0.0}     // No builtin and no "binary?": "Unknown binary operator", nullptr
                                               // (upstream asserts here instead; see README, Deviations)
    )
);
