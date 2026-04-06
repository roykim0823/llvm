#include "tinylang/Sema/Sema.h"
#include "tinylang/AST/AST.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

// Base Fixture and Setup
class SemaTestBase : public ::testing::Test {
protected:
  llvm::SourceMgr SrcMgr;
  DiagnosticsEngine Diags;
  std::unique_ptr<Sema> Semantics;

  TypeDeclaration *IntTy;
  TypeDeclaration *BoolTy;
  Expr *IntLit;
  Expr *BoolLit;

  SemaTestBase() : Diags(SrcMgr) {}

  void SetUp() override {
    Semantics = std::make_unique<Sema>(Diags);
    // Sema::initialize() is automatically called, populating the global scope.

    // Retrieve global types
    IntTy = cast<TypeDeclaration>(
        Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "INTEGER"));
    BoolTy = cast<TypeDeclaration>(
        Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "BOOLEAN"));

    // Create reusable literal expressions for testing
    IntLit = Semantics->actOnIntegerLiteral(llvm::SMLoc(), "42");

    Decl *TrueDecl = Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "TRUE");
    BoolLit = Semantics->actOnVariable(TrueDecl);
  }

  // Helper to easily get an expression of a desired type
  Expr *getExpr(bool IsInt) { return IsInt ? IntLit : BoolLit; }

  // Helper to easily get a type declaration
  TypeDeclaration *getType(bool IsInt) { return IsInt ? IntTy : BoolTy; }
};

// Tests: Type Checking for Expressions
struct BinaryOpTestParam {
  std::string TestName;
  tok::TokenKind Op;
  bool LeftIsInt;
  bool RightIsInt;
  unsigned ExpectedErrors;
};

class SemaBinaryOpTest : public SemaTestBase,
                         public ::testing::WithParamInterface<BinaryOpTestParam> {};

TEST_P(SemaBinaryOpTest, ActOnTermTypeChecking) {
  const auto &Param = GetParam();
  OperatorInfo OpInfo(llvm::SMLoc(), Param.Op);

  Expr *Left = getExpr(Param.LeftIsInt);
  Expr *Right = getExpr(Param.RightIsInt);

  // actOnTerm handles *, /, DIV, MOD, AND
  Expr *Result = Semantics->actOnTerm(Left, Right, OpInfo);

  EXPECT_NE(Result, nullptr);
  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failed type check for: " << Param.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    TermTypeChecks, SemaBinaryOpTest,
    ::testing::Values(
        // Multiplication (*) requires INTEGER
        BinaryOpTestParam{"ValidMult", tok::star, true, true, 0},
        BinaryOpTestParam{"InvalidMultBoolLeft", tok::star, false, true, 1},
        BinaryOpTestParam{"InvalidMultBothBool", tok::star, false, false, 1},

        // Logical AND requires BOOLEAN
        BinaryOpTestParam{"ValidAnd", tok::kw_AND, false, false, 0},
        BinaryOpTestParam{"InvalidAndIntLeft", tok::kw_AND, true, false, 1},
        BinaryOpTestParam{"InvalidAndBothInt", tok::kw_AND, true, true, 1}
    ),
    [](const ::testing::TestParamInfo<BinaryOpTestParam> &info) { return info.param.TestName; }
);

// Tests: Control Flow Conditions
struct ConditionTestParam {
  std::string TestName;
  bool CondIsInt;
  unsigned ExpectedErrors;
};

class SemaConditionTest : public SemaTestBase,
                          public ::testing::WithParamInterface<ConditionTestParam> {};

TEST_P(SemaConditionTest, IfStatementCondition) {
  const auto &Param = GetParam();
  StmtList Stmts, IfStmts, ElseStmts;

  Expr *Cond = getExpr(Param.CondIsInt);

  Semantics->actOnIfStatement(Stmts, llvm::SMLoc(), Cond, IfStmts, ElseStmts);

  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failed condition check for: " << Param.TestName;
  ASSERT_EQ(Stmts.size(), 1u);
  EXPECT_TRUE(isa<IfStatement>(Stmts[0]));
}

INSTANTIATE_TEST_SUITE_P(
    ControlFlowChecks, SemaConditionTest,
    ::testing::Values(
        ConditionTestParam{"ValidBooleanCondition", false, 0},
        ConditionTestParam{"InvalidIntegerCondition", true, 1} // Triggers err_if_expr_must_be_bool
    ),
    [](const ::testing::TestParamInfo<ConditionTestParam> &info) { return info.param.TestName; }
);

// Tests: Symbol Declarations
struct DeclTestParam {
  std::string TestName;
  std::vector<std::string> VariablesToDeclare;
  unsigned ExpectedErrors;
};

class SemaDeclarationTest : public SemaTestBase,
                            public ::testing::WithParamInterface<DeclTestParam> {};

TEST_P(SemaDeclarationTest, PreventDuplicateSymbols) {
  const auto &Param = GetParam();
  DeclList Decls;

  for (const std::string &VarName : Param.VariablesToDeclare) {
    // Act as if we are declaring a constant for each name
    Semantics->actOnConstantDeclaration(Decls, llvm::SMLoc(), VarName, IntLit);
  }

  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failed duplicate check for: " << Param.TestName;

  // The number of successfully inserted declarations should be Total - Errors
  EXPECT_EQ(Decls.size(), Param.VariablesToDeclare.size() - Param.ExpectedErrors);
}

INSTANTIATE_TEST_SUITE_P(
    DuplicateChecks, SemaDeclarationTest,
    ::testing::Values(
        DeclTestParam{"SingleVariable", {"VarA"}, 0},
        DeclTestParam{"MultipleDistinct", {"VarA", "VarB", "VarC"}, 0},
        DeclTestParam{"OneDuplicate", {"VarA", "VarA"}, 1}, // Triggers err_symbold_declared
        DeclTestParam{"MultipleDuplicates", {"VarA", "VarA", "VarA"}, 2}
    ),
    [](const ::testing::TestParamInfo<DeclTestParam> &info) { return info.param.TestName; }
);

} // namespace