/// \file
/// \brief Unit tests for \ref tinylang::CGProcedure (Chapter 4).
///
/// Three parametric test suites:
///   - **Signatures** — function-type generation across return-type and
///     parameter shapes (value + VAR params),
///   - **InfixOps** — every binary operator emits the expected LLVM
///     instruction (`add nsw`, `icmp sle`, `and`, …),
///   - **ControlFlow** — IF (with/without ELSE) and WHILE produce the
///     expected basic-block labels and verify clean.

#include "tinylang/CodeGen/CGProcedure.h"
#include "tinylang/CodeGen/CGModule.h"
#include "tinylang/AST/AST.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

// Base fixture: sets up LLVM infrastructure and CGModule
class CGProcedureTestBase : public ::testing::Test {
protected:
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> LLVMMod;
  std::unique_ptr<CGModule> CGM;

  // Reusable AST type nodes
  TypeDeclaration IntTy{nullptr, llvm::SMLoc(), "INTEGER"};
  TypeDeclaration BoolTy{nullptr, llvm::SMLoc(), "BOOLEAN"};
  ModuleDeclaration ModDecl{nullptr, llvm::SMLoc(), "TestMod"};

  void SetUp() override {
    LLVMMod = std::make_unique<llvm::Module>("test", Ctx);
    CGM = std::make_unique<CGModule>(LLVMMod.get());
  }
};

// ---------------------------------------------------------------
// 1. Parametric: procedure signature generation (void/int return,
//    varying parameter counts and types)
// ---------------------------------------------------------------
struct ProcSignatureParam {
  std::string TestName;
  bool HasReturnType;       // true => INTEGER return
  unsigned NumIntParams;
  unsigned NumVarParams;    // VAR params (passed by reference)
  unsigned ExpectedArgCount;
};

class CGProcedureSignatureTest
    : public CGProcedureTestBase,
      public ::testing::WithParamInterface<ProcSignatureParam> {};

TEST_P(CGProcedureSignatureTest, GeneratesCorrectSignature) {
  const auto &P = GetParam();

  // Reserve capacity upfront so push_back never reallocates.
  // Decl stores StringRef, so the underlying strings must stay alive
  // and at a stable address for the duration of the test.
  std::vector<std::string> ParamNames;
  ParamNames.reserve(P.NumIntParams + P.NumVarParams);

  FormalParamList Params;
  std::vector<std::unique_ptr<FormalParameterDeclaration>> ParamStorage;

  for (unsigned i = 0; i < P.NumIntParams; ++i) {
    ParamNames.push_back("p" + std::to_string(i));
    ParamStorage.push_back(std::make_unique<FormalParameterDeclaration>(
        nullptr, llvm::SMLoc(), ParamNames.back(), &IntTy, /*IsVar=*/false));
    Params.push_back(ParamStorage.back().get());
  }
  for (unsigned i = 0; i < P.NumVarParams; ++i) {
    ParamNames.push_back("v" + std::to_string(i));
    ParamStorage.push_back(std::make_unique<FormalParameterDeclaration>(
        nullptr, llvm::SMLoc(), ParamNames.back(), &IntTy, /*IsVar=*/true));
    Params.push_back(ParamStorage.back().get());
  }

  TypeDeclaration *RetType = P.HasReturnType ? &IntTy : nullptr;
  DeclList Decls;
  StmtList Stmts;

  // Build a minimal procedure: just returns void or a literal
  IntegerLiteral *Lit = nullptr;
  if (P.HasReturnType) {
    llvm::APSInt Val(64, /*isUnsigned=*/false);
    Val = 0;
    Lit = new IntegerLiteral(llvm::SMLoc(), Val, &IntTy);
    Stmts.push_back(new ReturnStatement(Lit));
  }

  ProcedureDeclaration ProcDecl(
      &ModDecl, llvm::SMLoc(), "TestProc", Params, RetType, Decls, Stmts);

  CGProcedure CGP(*CGM);
  CGP.run(&ProcDecl);

  // Find the generated function in the module
  llvm::Function *Fn = LLVMMod->getFunction(CGM->mangleName(&ProcDecl));
  ASSERT_NE(Fn, nullptr) << "Function not found for: " << P.TestName;

  // Check return type
  if (P.HasReturnType) {
    EXPECT_TRUE(Fn->getReturnType()->isIntegerTy(64))
        << "Expected i64 return for: " << P.TestName;
  } else {
    EXPECT_TRUE(Fn->getReturnType()->isVoidTy())
        << "Expected void return for: " << P.TestName;
  }

  // Check argument count
  EXPECT_EQ(Fn->arg_size(), P.ExpectedArgCount)
      << "Wrong arg count for: " << P.TestName;

  // Verify the function IR is well-formed
  EXPECT_FALSE(llvm::verifyFunction(*Fn, &llvm::errs()))
      << "Verification failed for: " << P.TestName;

  // Cleanup AST nodes allocated with new
  for (auto *S : Stmts) delete S;
  delete Lit;
}

INSTANTIATE_TEST_SUITE_P(
    Signatures, CGProcedureSignatureTest,
    ::testing::Values(
        ProcSignatureParam{"VoidNoArgs", false, 0, 0, 0},
        ProcSignatureParam{"VoidOneArg", false, 1, 0, 1},
        ProcSignatureParam{"VoidTwoArgs", false, 2, 0, 2},
        ProcSignatureParam{"IntRetNoArgs", true, 0, 0, 0},
        ProcSignatureParam{"IntRetTwoArgs", true, 2, 0, 2},
        ProcSignatureParam{"VoidOneVarParam", false, 0, 1, 1},
        ProcSignatureParam{"MixedParams", false, 1, 1, 2}
    ),
    [](const ::testing::TestParamInfo<ProcSignatureParam> &info) {
      return info.param.TestName;
    });

// ---------------------------------------------------------------
// 2. Parametric: expression code generation via emitExpr
//    Tests infix operations produce valid IR
// ---------------------------------------------------------------
struct InfixExprParam {
  std::string TestName;
  tok::TokenKind Op;
  bool IsIntOp;       // true = INTEGER operands, false = BOOLEAN
  std::string ExpectedOpName;  // substring in the generated IR instruction
};

class CGProcedureInfixTest
    : public CGProcedureTestBase,
      public ::testing::WithParamInterface<InfixExprParam> {};

TEST_P(CGProcedureInfixTest, EmitsCorrectInstruction) {
  const auto &P = GetParam();

  TypeDeclaration *OpTy = P.IsIntOp ? &IntTy : &BoolTy;

  // Build: PROCEDURE Test(a, b: <type>) : <type>; BEGIN RETURN a <op> b; END Test;
  FormalParamList Params;
  auto ParamA = std::make_unique<FormalParameterDeclaration>(
      nullptr, llvm::SMLoc(), "a", OpTy, false);
  auto ParamB = std::make_unique<FormalParameterDeclaration>(
      nullptr, llvm::SMLoc(), "b", OpTy, false);
  Params.push_back(ParamA.get());
  Params.push_back(ParamB.get());

  // Expression: a <op> b
  VariableAccess VarA(ParamA.get());
  VariableAccess VarB(ParamB.get());
  OperatorInfo OpInfo(llvm::SMLoc(), P.Op);

  // Determine result type for comparisons vs arithmetic
  TypeDeclaration *ResultTy = OpTy;
  bool IsComparison = (P.Op == tok::equal || P.Op == tok::hash ||
                       P.Op == tok::less || P.Op == tok::lessequal ||
                       P.Op == tok::greater || P.Op == tok::greaterequal);
  if (IsComparison)
    ResultTy = &BoolTy;

  InfixExpression Infix(&VarA, &VarB, OpInfo, ResultTy, false);
  ReturnStatement RetStmt(&Infix);

  StmtList Stmts;
  Stmts.push_back(&RetStmt);

  DeclList Decls;
  ProcedureDeclaration ProcDecl(
      &ModDecl, llvm::SMLoc(), "TestOp", Params, ResultTy, Decls, Stmts);

  CGProcedure CGP(*CGM);
  CGP.run(&ProcDecl);

  llvm::Function *Fn = LLVMMod->getFunction(CGM->mangleName(&ProcDecl));
  ASSERT_NE(Fn, nullptr) << "Function not found for: " << P.TestName;
  EXPECT_FALSE(llvm::verifyFunction(*Fn, &llvm::errs()))
      << "Verification failed for: " << P.TestName;

  // Print IR to a string to check the expected instruction is present
  std::string IRStr;
  llvm::raw_string_ostream OS(IRStr);
  Fn->print(OS);
  OS.flush();

  EXPECT_NE(IRStr.find(P.ExpectedOpName), std::string::npos)
      << "Expected '" << P.ExpectedOpName << "' in IR for: " << P.TestName
      << "\nGot:\n" << IRStr;
}

INSTANTIATE_TEST_SUITE_P(
    InfixOps, CGProcedureInfixTest,
    ::testing::Values(
        InfixExprParam{"Add", tok::plus, true, "add nsw"},
        InfixExprParam{"Sub", tok::minus, true, "sub nsw"},
        InfixExprParam{"Mul", tok::star, true, "mul nsw"},
        InfixExprParam{"Div", tok::kw_DIV, true, "sdiv"},
        InfixExprParam{"Mod", tok::kw_MOD, true, "srem"},
        InfixExprParam{"CmpEq", tok::equal, true, "icmp eq"},
        InfixExprParam{"CmpNe", tok::hash, true, "icmp ne"},
        InfixExprParam{"CmpLt", tok::less, true, "icmp slt"},
        InfixExprParam{"CmpLe", tok::lessequal, true, "icmp sle"},
        InfixExprParam{"CmpGt", tok::greater, true, "icmp sgt"},
        InfixExprParam{"CmpGe", tok::greaterequal, true, "icmp sge"},
        InfixExprParam{"And", tok::kw_AND, false, "and"},
        InfixExprParam{"Or", tok::kw_OR, false, "or"}
    ),
    [](const ::testing::TestParamInfo<InfixExprParam> &info) {
      return info.param.TestName;
    });

// ---------------------------------------------------------------
// 3. Parametric: control flow statement generation
// ---------------------------------------------------------------
struct ControlFlowParam {
  std::string TestName;
  bool UseIf;       // true = IF, false = WHILE
  bool HasElse;     // only meaningful for IF
  std::string ExpectedLabel;  // label substring in IR
};

class CGProcedureControlFlowTest
    : public CGProcedureTestBase,
      public ::testing::WithParamInterface<ControlFlowParam> {};

TEST_P(CGProcedureControlFlowTest, GeneratesCorrectCFG) {
  const auto &P = GetParam();

  // Build: PROCEDURE Test(cond: BOOLEAN);
  FormalParamList Params;
  auto CondParam = std::make_unique<FormalParameterDeclaration>(
      nullptr, llvm::SMLoc(), "cond", &BoolTy, false);
  Params.push_back(CondParam.get());

  VariableAccess CondAccess(CondParam.get());

  StmtList BodyStmts;  // empty body for simplicity
  StmtList ElseStmts;

  // For HasElse, add a dummy return to make the else branch non-empty
  std::unique_ptr<ReturnStatement> ElseRet;
  if (P.UseIf && P.HasElse) {
    ElseRet = std::make_unique<ReturnStatement>(nullptr);
    ElseStmts.push_back(ElseRet.get());
  }

  // Build the control flow statement (stack-allocated)
  IfStatement IfStmt(&CondAccess, BodyStmts, ElseStmts);
  WhileStatement WhileStmt(&CondAccess, BodyStmts);

  StmtList ProcStmts;
  ProcStmts.push_back(P.UseIf ? static_cast<Stmt *>(&IfStmt)
                               : static_cast<Stmt *>(&WhileStmt));

  DeclList Decls;
  ProcedureDeclaration ProcDecl(
      &ModDecl, llvm::SMLoc(), "TestCF", Params, nullptr, Decls, ProcStmts);

  CGProcedure CGP(*CGM);
  CGP.run(&ProcDecl);

  llvm::Function *Fn = LLVMMod->getFunction(CGM->mangleName(&ProcDecl));
  ASSERT_NE(Fn, nullptr) << "Function not found for: " << P.TestName;
  EXPECT_FALSE(llvm::verifyFunction(*Fn, &llvm::errs()))
      << "Verification failed for: " << P.TestName;

  // Verify expected basic block labels appear
  std::string IRStr;
  llvm::raw_string_ostream OS(IRStr);
  Fn->print(OS);
  OS.flush();

  EXPECT_NE(IRStr.find(P.ExpectedLabel), std::string::npos)
      << "Expected label '" << P.ExpectedLabel << "' in IR for: " << P.TestName
      << "\nGot:\n" << IRStr;
}

INSTANTIATE_TEST_SUITE_P(
    ControlFlow, CGProcedureControlFlowTest,
    ::testing::Values(
        ControlFlowParam{"IfNoElse", true, false, "if.body"},
        ControlFlowParam{"IfWithElse", true, true, "else.body"},
        ControlFlowParam{"WhileLoop", false, false, "while.cond"}
    ),
    [](const ::testing::TestParamInfo<ControlFlowParam> &info) {
      return info.param.TestName;
    });

} // namespace
