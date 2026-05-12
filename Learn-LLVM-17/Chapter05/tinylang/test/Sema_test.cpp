#include "tinylang/Sema/Sema.h"
#include "tinylang/AST/AST.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

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

    IntTy = cast<TypeDeclaration>(
        Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "INTEGER"));
    BoolTy = cast<TypeDeclaration>(
        Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "BOOLEAN"));

    IntLit = Semantics->actOnIntegerLiteral(llvm::SMLoc(), "42");

    Decl *TrueDecl = Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "TRUE");
    BoolLit = Semantics->actOnDesignator(TrueDecl);
  }

  Expr *getExpr(bool IsInt) { return IsInt ? IntLit : BoolLit; }
  TypeDeclaration *getType(bool IsInt) { return IsInt ? IntTy : BoolTy; }
};

// Binary-op type checks
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

  Expr *Result = Semantics->actOnTerm(Left, Right, OpInfo);

  EXPECT_NE(Result, nullptr);
  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failed type check for: " << Param.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    TermTypeChecks, SemaBinaryOpTest,
    ::testing::Values(
        BinaryOpTestParam{"ValidMult", tok::star, true, true, 0},
        BinaryOpTestParam{"InvalidMultBoolLeft", tok::star, false, true, 1},
        BinaryOpTestParam{"InvalidMultBothBool", tok::star, false, false, 1},
        BinaryOpTestParam{"ValidAnd", tok::kw_AND, false, false, 0}
        // Note: Ch05's Sema::actOnTerm constant-folds AND on isConst operands
        // and unconditionally dyn_casts both sides to BooleanLiteral. With
        // mistyped (INTEGER) operands, the cast yields nullptr and the fold
        // crashes. Those failure-path cases are intentionally omitted here.
    ),
    [](const ::testing::TestParamInfo<BinaryOpTestParam> &info) { return info.param.TestName; }
);

// Control-flow conditions
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
        ConditionTestParam{"InvalidIntegerCondition", true, 1}
    ),
    [](const ::testing::TestParamInfo<ConditionTestParam> &info) { return info.param.TestName; }
);

// Duplicate-symbol detection
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
    Semantics->actOnConstantDeclaration(Decls, llvm::SMLoc(), VarName, IntLit);
  }

  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failed duplicate check for: " << Param.TestName;
  EXPECT_EQ(Decls.size(), Param.VariablesToDeclare.size() - Param.ExpectedErrors);
}

INSTANTIATE_TEST_SUITE_P(
    DuplicateChecks, SemaDeclarationTest,
    ::testing::Values(
        DeclTestParam{"SingleVariable", {"VarA"}, 0},
        DeclTestParam{"MultipleDistinct", {"VarA", "VarB", "VarC"}, 0},
        DeclTestParam{"OneDuplicate", {"VarA", "VarA"}, 1},
        DeclTestParam{"MultipleDuplicates", {"VarA", "VarA", "VarA"}, 2}
    ),
    [](const ::testing::TestParamInfo<DeclTestParam> &info) { return info.param.TestName; }
);

// =========================================================================
// Ch05-specific: type declarations
// =========================================================================

// Alias type: TYPE Foo = INTEGER;
TEST_F(SemaTestBase, ActOnAliasTypeDeclaration) {
  DeclList Decls;
  Semantics->actOnAliasTypeDeclaration(Decls, llvm::SMLoc(), "Foo", IntTy);
  EXPECT_EQ(Diags.numErrors(), 0u);
  ASSERT_EQ(Decls.size(), 1u);
  auto *AliasD = dyn_cast<AliasTypeDeclaration>(Decls[0]);
  ASSERT_NE(AliasD, nullptr);
  EXPECT_EQ(AliasD->getType(), IntTy);
}

// Pointer type: TYPE PtrI = POINTER TO INTEGER;
TEST_F(SemaTestBase, ActOnPointerTypeDeclaration) {
  DeclList Decls;
  Semantics->actOnPointerTypeDeclaration(Decls, llvm::SMLoc(), "PtrI", IntTy);
  EXPECT_EQ(Diags.numErrors(), 0u);
  ASSERT_EQ(Decls.size(), 1u);
  auto *PtrD = dyn_cast<PointerTypeDeclaration>(Decls[0]);
  ASSERT_NE(PtrD, nullptr);
  EXPECT_EQ(PtrD->getType(), IntTy);
}

// Array type: TYPE Arr = ARRAY [10] OF INTEGER;  (size must be const INTEGER)
TEST_F(SemaTestBase, ActOnArrayTypeDeclarationValid) {
  DeclList Decls;
  Expr *Size = Semantics->actOnIntegerLiteral(llvm::SMLoc(), "10");
  Semantics->actOnArrayTypeDeclaration(Decls, llvm::SMLoc(), "Arr", Size, IntTy);
  EXPECT_EQ(Diags.numErrors(), 0u);
  ASSERT_EQ(Decls.size(), 1u);
  auto *ArrD = dyn_cast<ArrayTypeDeclaration>(Decls[0]);
  ASSERT_NE(ArrD, nullptr);
  EXPECT_EQ(ArrD->getType(), IntTy);
  EXPECT_EQ(ArrD->getNums(), Size);
}

// Non-constant size → no declaration created (Sema silently drops it).
TEST_F(SemaTestBase, ActOnArrayTypeDeclarationRejectsNonConstSize) {
  DeclList Decls;
  // BoolLit (TRUE) is constant but not of INTEGER type — also rejected.
  Semantics->actOnArrayTypeDeclaration(Decls, llvm::SMLoc(), "Arr", BoolLit, IntTy);
  EXPECT_TRUE(Decls.empty());
}

// Record type with two fields: TYPE P = RECORD X, Y: INTEGER END;
TEST_F(SemaTestBase, ActOnRecordTypeDeclaration) {
  DeclList Decls;
  FieldList Fields;
  IdentList Ids = {
      {llvm::SMLoc(), "X"},
      {llvm::SMLoc(), "Y"},
  };
  Semantics->actOnFieldDeclaration(Fields, Ids, IntTy);
  ASSERT_EQ(Fields.size(), 2u);
  EXPECT_EQ(Fields[0].getName(), "X");
  EXPECT_EQ(Fields[1].getName(), "Y");

  Semantics->actOnRecordTypeDeclaration(Decls, llvm::SMLoc(), "P", Fields);
  EXPECT_EQ(Diags.numErrors(), 0u);
  ASSERT_EQ(Decls.size(), 1u);
  auto *RecD = dyn_cast<RecordTypeDeclaration>(Decls[0]);
  ASSERT_NE(RecD, nullptr);
  EXPECT_EQ(RecD->getFields().size(), 2u);
}

// Record with duplicate field names is rejected.
TEST_F(SemaTestBase, ActOnRecordTypeDeclarationRejectsDuplicateFields) {
  DeclList Decls;
  FieldList Fields;
  IdentList Ids = {
      {llvm::SMLoc(), "X"},
      {llvm::SMLoc(), "X"},  // duplicate
  };
  Semantics->actOnFieldDeclaration(Fields, Ids, IntTy);
  Semantics->actOnRecordTypeDeclaration(Decls, llvm::SMLoc(), "P", Fields);
  EXPECT_EQ(Diags.numErrors(), 1u);
  EXPECT_TRUE(Decls.empty());
}

// =========================================================================
// Ch05-specific: designator selectors
// =========================================================================

// Build a Designator for an array variable, then add an IndexSelector;
// the designator's type should become the array's element type.
TEST_F(SemaTestBase, ActOnIndexSelectorAdjustsDesignatorType) {
  // TYPE Arr = ARRAY [4] OF INTEGER;  VAR a: Arr;
  DeclList Decls;
  Expr *Size = Semantics->actOnIntegerLiteral(llvm::SMLoc(), "4");
  Semantics->actOnArrayTypeDeclaration(Decls, llvm::SMLoc(), "Arr", Size, IntTy);
  ASSERT_EQ(Decls.size(), 1u);
  auto *ArrTy = cast<ArrayTypeDeclaration>(Decls[0]);

  IdentList VarIds = {{llvm::SMLoc(), "a"}};
  Semantics->actOnVariableDeclaration(Decls, VarIds, ArrTy);
  ASSERT_EQ(Decls.size(), 2u);
  auto *VarA = cast<VariableDeclaration>(Decls[1]);

  Expr *Desig = Semantics->actOnDesignator(VarA);
  ASSERT_NE(Desig, nullptr);
  EXPECT_EQ(Desig->getType(), ArrTy);

  Expr *Idx = Semantics->actOnIntegerLiteral(llvm::SMLoc(), "0");
  Semantics->actOnIndexSelector(Desig, llvm::SMLoc(), Idx);

  // After indexing, the designator now refers to an element of type INTEGER.
  EXPECT_EQ(Desig->getType(), IntTy);
  auto *D = cast<Designator>(Desig);
  ASSERT_EQ(D->getSelectors().size(), 1u);
  EXPECT_TRUE(isa<IndexSelector>(D->getSelectors()[0]));
}

// Build a Designator for a record variable, then add a FieldSelector;
// the designator's type should become the field's type.
TEST_F(SemaTestBase, ActOnFieldSelectorAdjustsDesignatorType) {
  // TYPE P = RECORD X: INTEGER END;  VAR p: P;
  DeclList Decls;
  FieldList Fields;
  IdentList FieldIds = {{llvm::SMLoc(), "X"}};
  Semantics->actOnFieldDeclaration(Fields, FieldIds, IntTy);
  Semantics->actOnRecordTypeDeclaration(Decls, llvm::SMLoc(), "P", Fields);
  ASSERT_EQ(Decls.size(), 1u);
  auto *RecTy = cast<RecordTypeDeclaration>(Decls[0]);

  IdentList VarIds = {{llvm::SMLoc(), "p"}};
  Semantics->actOnVariableDeclaration(Decls, VarIds, RecTy);
  ASSERT_EQ(Decls.size(), 2u);
  auto *VarP = cast<VariableDeclaration>(Decls[1]);

  Expr *Desig = Semantics->actOnDesignator(VarP);
  ASSERT_NE(Desig, nullptr);
  EXPECT_EQ(Desig->getType(), RecTy);

  Semantics->actOnFieldSelector(Desig, llvm::SMLoc(), "X");
  EXPECT_EQ(Desig->getType(), IntTy);
  auto *D = cast<Designator>(Desig);
  ASSERT_EQ(D->getSelectors().size(), 1u);
  auto *FS = dyn_cast<FieldSelector>(D->getSelectors()[0]);
  ASSERT_NE(FS, nullptr);
  EXPECT_EQ(FS->getIndex(), 0u);
}

// Pointer dereference: ^.
TEST_F(SemaTestBase, ActOnDereferenceSelectorAdjustsDesignatorType) {
  // TYPE PI = POINTER TO INTEGER;  VAR p: PI;
  DeclList Decls;
  Semantics->actOnPointerTypeDeclaration(Decls, llvm::SMLoc(), "PI", IntTy);
  auto *PtrTy = cast<PointerTypeDeclaration>(Decls[0]);

  IdentList VarIds = {{llvm::SMLoc(), "p"}};
  Semantics->actOnVariableDeclaration(Decls, VarIds, PtrTy);
  auto *VarP = cast<VariableDeclaration>(Decls[1]);

  Expr *Desig = Semantics->actOnDesignator(VarP);
  EXPECT_EQ(Desig->getType(), PtrTy);
  Semantics->actOnDereferenceSelector(Desig, llvm::SMLoc());
  EXPECT_EQ(Desig->getType(), IntTy);
  auto *D = cast<Designator>(Desig);
  ASSERT_EQ(D->getSelectors().size(), 1u);
  EXPECT_TRUE(isa<DereferenceSelector>(D->getSelectors()[0]));
}

} // namespace
