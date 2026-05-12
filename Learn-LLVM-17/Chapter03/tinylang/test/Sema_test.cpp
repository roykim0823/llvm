/// \file
/// \brief Unit tests for tinylang::Sema's action methods.
///
/// Instead of going through Lexer + Parser, these tests construct AST
/// fragments by calling `actOn*` directly, then verify the resulting
/// diagnostics and (where applicable) the produced statement/expression
/// kind. Three suites:
///  - `SemaBinaryOpTest`   — `actOnTerm` type compatibility.
///  - `SemaConditionTest`  — `actOnIfStatement` rejects non-BOOLEAN conditions.
///  - `SemaDeclarationTest`— `actOnConstantDeclaration` rejects duplicates.
///
/// The fixture reuses one `IntLit` and one `BoolLit` for every test, so a
/// binary-op test that wants two INTEGER operands passes the same Expr*
/// twice. This is fine because Sema treats operands as immutable inputs.

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

  // Helper to produce a *non-const* expression of the desired type.
  //
  // Needed because the constant-folding paths in actOnSimpleExpression
  // (`kw_OR`) and actOnPrefixExpression (`kw_NOT`) `dyn_cast` a const operand
  // to BooleanLiteral and dereference the result without a null guard — see
  // the parallel `HK: Crash!` note in actOnTerm. Tests that just want to
  // verify diagnostics should stay away from those paths by using
  // VariableAccess instead of an IntegerLiteral / BooleanLiteral.
  Expr *getNonConstExpr(bool IsInt) {
    auto *Var = new VariableDeclaration(nullptr, llvm::SMLoc(),
                                        IsInt ? "iv" : "bv",
                                        IsInt ? IntTy : BoolTy);
    return Semantics->actOnVariable(Var);
  }
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

// Tests: Additive expressions (`+`, `-`, `OR`)
//
// `actOnSimpleExpression` doesn't call `isOperatorForType`, but it does
// require both operands to share a type — same shape as actOnTerm, different
// implementation path. Worth covering separately because the two functions
// have diverged before in other Modula-2 frontends.
struct SimpleExprTestParam {
  std::string TestName;
  tok::TokenKind Op;
  bool LeftIsInt;
  bool RightIsInt;
  unsigned ExpectedErrors;
};

class SemaSimpleExprTest : public SemaTestBase,
                           public ::testing::WithParamInterface<SimpleExprTestParam> {};

TEST_P(SemaSimpleExprTest, ActOnSimpleExpressionTypeChecking) {
  const auto &Param = GetParam();
  OperatorInfo OpInfo(llvm::SMLoc(), Param.Op);
  // Use non-const operands so a kw_OR case never enters the constant-folding
  // branch — see getNonConstExpr's comment.
  Expr *Left = getNonConstExpr(Param.LeftIsInt);
  Expr *Right = getNonConstExpr(Param.RightIsInt);

  Expr *Result = Semantics->actOnSimpleExpression(Left, Right, OpInfo);

  EXPECT_NE(Result, nullptr);
  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failed type check for: " << Param.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    AddOpTypeChecks, SemaSimpleExprTest,
    ::testing::Values(
        // INTEGER + INTEGER OK.
        SimpleExprTestParam{"ValidIntPlus", tok::plus, true, true, 0},
        // INTEGER + BOOLEAN -> type mismatch.
        SimpleExprTestParam{"InvalidMixedPlus", tok::plus, true, false, 1},
        // BOOLEAN OR BOOLEAN OK (non-const operands, skips the constant fold).
        SimpleExprTestParam{"ValidBoolOr", tok::kw_OR, false, false, 0},
        // OR with one INTEGER operand -> type mismatch.
        SimpleExprTestParam{"InvalidIntOr", tok::kw_OR, true, false, 1}
    ),
    [](const ::testing::TestParamInfo<SimpleExprTestParam> &info) { return info.param.TestName; }
);

// Tests: Relational expressions
//
// `actOnExpression` is the relop handler. Result type is always BOOLEAN and
// the operands must share a type. The relop kind itself isn't restricted by
// operand type — `INTEGER = INTEGER` and `BOOLEAN = BOOLEAN` are both valid.
TEST_F(SemaTestBase, RelationalEqualOnSameTypeIsBoolean) {
  OperatorInfo OpInfo(llvm::SMLoc(), tok::equal);
  Expr *Result = Semantics->actOnExpression(IntLit, IntLit, OpInfo);
  ASSERT_NE(Result, nullptr);
  EXPECT_EQ(Result->getType(), BoolTy);
  EXPECT_EQ(Diags.numErrors(), 0u);
}

TEST_F(SemaTestBase, RelationalOnMismatchedTypesReportsError) {
  OperatorInfo OpInfo(llvm::SMLoc(), tok::less);
  Expr *Result = Semantics->actOnExpression(IntLit, BoolLit, OpInfo);
  ASSERT_NE(Result, nullptr);
  EXPECT_EQ(Diags.numErrors(), 1u);
}

// Tests: Prefix expressions (unary `-`, `NOT`)
TEST_F(SemaTestBase, PrefixNotOnBooleanIsOk) {
  OperatorInfo OpInfo(llvm::SMLoc(), tok::kw_NOT);
  Expr *Result = Semantics->actOnPrefixExpression(BoolLit, OpInfo);
  ASSERT_NE(Result, nullptr);
  EXPECT_EQ(Diags.numErrors(), 0u);
}

TEST_F(SemaTestBase, PrefixNotOnIntegerReportsError) {
  // `NOT` is only defined for BOOLEAN. Sema rejects this even though the
  // expression still gets built (so the parser can keep going).
  //
  // Important: we use a non-const integer (VariableAccess) here. Using IntLit
  // would also hit the type mismatch, but Sema then enters the constant-folding
  // branch (`E->isConst() && Op == NOT`), dyn_casts E to BooleanLiteral, gets
  // a null pointer, and dereferences it — see the parallel `HK: Crash!` note
  // in actOnTerm. That's an existing Sema bug, not something this test should
  // smoke out (the dedicated regression for it would have to be opt-in).
  auto *Var = new VariableDeclaration(nullptr, llvm::SMLoc(), "x", IntTy);
  Expr *VarRef = Semantics->actOnVariable(Var);
  OperatorInfo OpInfo(llvm::SMLoc(), tok::kw_NOT);
  Semantics->actOnPrefixExpression(VarRef, OpInfo);
  EXPECT_EQ(Diags.numErrors(), 1u);
}

TEST_F(SemaTestBase, UnaryMinusOnIntegerLiteralIsUnambiguous) {
  // Unary minus on an unambiguous operand (literal, var, const, * or /
  // sub-expression) doesn't warn. Anything else fires `warn_ambigous_negation`.
  OperatorInfo OpInfo(llvm::SMLoc(), tok::minus);
  Expr *Result = Semantics->actOnPrefixExpression(IntLit, OpInfo);
  ASSERT_NE(Result, nullptr);
  EXPECT_EQ(Diags.numErrors(), 0u);
}

// Tests: Integer-literal radix decoding
//
// The lexer hands the literal spelling through; Sema decides between decimal
// (radix 10) and hex (radix 16, trailing 'H'). Anything else is a Ch5+
// extension and not tested here.
TEST_F(SemaTestBase, IntegerLiteralDecimalRadix) {
  auto *Lit = cast<IntegerLiteral>(
      Semantics->actOnIntegerLiteral(llvm::SMLoc(), "100"));
  EXPECT_EQ(Lit->getValue().getZExtValue(), 100u);
}

TEST_F(SemaTestBase, IntegerLiteralHexRadix) {
  // 0FFH = 0xFF = 255. Confirms the 'H' suffix is stripped *and* the radix is 16.
  auto *Lit = cast<IntegerLiteral>(
      Semantics->actOnIntegerLiteral(llvm::SMLoc(), "0FFH"));
  EXPECT_EQ(Lit->getValue().getZExtValue(), 255u);
}

// Tests: WHILE statement condition
//
// Mirrors the IF test above. Kept as its own pair because the diagnostic id
// is distinct (`err_while_expr_must_be_bool`) and historically the WHILE path
// has been broken when only IF was tested.
TEST_F(SemaTestBase, WhileConditionMustBeBoolean) {
  StmtList Stmts, Body;
  Semantics->actOnWhileStatement(Stmts, llvm::SMLoc(), BoolLit, Body);
  EXPECT_EQ(Diags.numErrors(), 0u);
  ASSERT_EQ(Stmts.size(), 1u);
  EXPECT_TRUE(isa<WhileStatement>(Stmts[0]));
}

TEST_F(SemaTestBase, WhileConditionIntegerReportsError) {
  StmtList Stmts, Body;
  Semantics->actOnWhileStatement(Stmts, llvm::SMLoc(), IntLit, Body);
  EXPECT_EQ(Diags.numErrors(), 1u);
}

// Tests: Assignment statement
//
// `actOnAssignment` is reached only via a Decl* whose dynamic kind is
// `VariableDeclaration`. Building one by hand keeps the test independent of
// the parser. We don't insert it into the current scope because Sema doesn't
// re-resolve the name here — the parser does that upstream.
TEST_F(SemaTestBase, AssignmentMatchingTypesProducesNoError) {
  auto *Var = new VariableDeclaration(nullptr, llvm::SMLoc(), "x", IntTy);
  StmtList Stmts;
  Semantics->actOnAssignment(Stmts, llvm::SMLoc(), Var, IntLit);
  EXPECT_EQ(Diags.numErrors(), 0u);
  ASSERT_EQ(Stmts.size(), 1u);
  EXPECT_TRUE(isa<AssignmentStatement>(Stmts[0]));
}

TEST_F(SemaTestBase, AssignmentMismatchedTypesReportsError) {
  auto *Var = new VariableDeclaration(nullptr, llvm::SMLoc(), "x", IntTy);
  StmtList Stmts;
  // INTEGER var := BOOLEAN expression -> err_types_for_operator_not_compatible.
  Semantics->actOnAssignment(Stmts, llvm::SMLoc(), Var, BoolLit);
  EXPECT_EQ(Diags.numErrors(), 1u);
}

// Tests: Name resolution
//
// `actOnQualIdentPart` is invoked once per dotted segment of `Mod.x`. The
// first-segment path (Prev = nullptr) goes through the current scope; this
// test only covers that path. The cross-module path is exercised end-to-end
// by Parser_test once imports are implemented (Ch04+).
TEST_F(SemaTestBase, QualIdentPartFindsBuiltinType) {
  Decl *D = Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "INTEGER");
  ASSERT_NE(D, nullptr);
  EXPECT_TRUE(isa<TypeDeclaration>(D));
  EXPECT_EQ(Diags.numErrors(), 0u);
}

TEST_F(SemaTestBase, QualIdentPartUndeclaredNameReportsError) {
  Decl *D = Semantics->actOnQualIdentPart(nullptr, llvm::SMLoc(), "NoSuchName");
  EXPECT_EQ(D, nullptr);
  EXPECT_EQ(Diags.numErrors(), 1u);
}

// Tests: Import handler is a Ch03 stub
//
// `actOnImport` always emits `err_not_yet_implemented` in Chapter 3. Pinning
// this in a test makes the eventual Ch04+ removal a deliberate change.
TEST_F(SemaTestBase, ImportIsNotYetImplemented) {
  IdentList Ids;
  Semantics->actOnImport("SomeModule", Ids);
  EXPECT_EQ(Diags.numErrors(), 1u);
}

} // namespace