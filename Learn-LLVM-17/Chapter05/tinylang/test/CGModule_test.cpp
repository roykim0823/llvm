/// \file
/// \brief Unit tests for \ref tinylang::CGModule.
///
/// Inherits the Ch04 suites and adds Ch05-specific cases at the bottom:
///   - \ref ConvertArrayType / \ref ConvertRecordType / \ref ConvertAliasType —
///     `convertType()` on the new compound type declarations,
///   - \ref ConvertTypeIsCached — `TypeCache` returns the same `llvm::Type`
///     for repeated lookups,
///   - mangler now covers `Point::AssignX` (matches the symbol emitted from
///     `examples/Point.mod`).
///
/// All fixtures now thread an \ref tinylang::ASTContext into
/// \ref tinylang::CGModule and use \ref PervasiveTypeDeclaration for
/// INTEGER/BOOLEAN.

#include "tinylang/CodeGen/CGModule.h"
#include "tinylang/AST/AST.h"
#include "tinylang/AST/ASTContext.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

// Base fixture: LLVMContext, Module, ASTContext, CGModule.
class CGModuleTestBase : public ::testing::Test {
protected:
  llvm::LLVMContext Ctx;
  llvm::SourceMgr SrcMgr;
  std::unique_ptr<ASTContext> ASTCtx;
  std::unique_ptr<llvm::Module> LLVMMod;
  std::unique_ptr<CGModule> CGM;

  // Reusable AST nodes — INTEGER/BOOLEAN are now PervasiveTypeDeclarations in Ch05.
  ModuleDeclaration ModDecl{nullptr, llvm::SMLoc(), "TestMod"};
  PervasiveTypeDeclaration IntTy{nullptr, llvm::SMLoc(), "INTEGER"};
  PervasiveTypeDeclaration BoolTy{nullptr, llvm::SMLoc(), "BOOLEAN"};

  void SetUp() override {
    ASTCtx = std::make_unique<ASTContext>(SrcMgr, "test.mod");
    LLVMMod = std::make_unique<llvm::Module>("test", Ctx);
    CGM = std::make_unique<CGModule>(*ASTCtx, LLVMMod.get());
  }
};

// 1. initialize() sets up the primitive LLVM types
TEST_F(CGModuleTestBase, InitializeSetsUpTypes) {
  EXPECT_NE(CGM->VoidTy, nullptr);
  EXPECT_NE(CGM->Int1Ty, nullptr);
  EXPECT_NE(CGM->Int32Ty, nullptr);
  EXPECT_NE(CGM->Int64Ty, nullptr);
  EXPECT_NE(CGM->Int32Zero, nullptr);

  EXPECT_TRUE(CGM->VoidTy->isVoidTy());
  EXPECT_TRUE(CGM->Int1Ty->isIntegerTy(1));
  EXPECT_TRUE(CGM->Int32Ty->isIntegerTy(32));
  EXPECT_TRUE(CGM->Int64Ty->isIntegerTy(64));
}

// 2. convertType() on pervasive types
struct ConvertTypeParam {
  std::string TestName;
  std::string TypeName;
  unsigned ExpectedBitWidth;
};

class CGModuleConvertTypeTest
    : public CGModuleTestBase,
      public ::testing::WithParamInterface<ConvertTypeParam> {};

TEST_P(CGModuleConvertTypeTest, ConvertsPervasiveToCorrectLLVMType) {
  const auto &P = GetParam();
  PervasiveTypeDeclaration Ty(nullptr, llvm::SMLoc(), P.TypeName);
  llvm::Type *Result = CGM->convertType(&Ty);
  ASSERT_NE(Result, nullptr);
  EXPECT_TRUE(Result->isIntegerTy(P.ExpectedBitWidth))
      << "Failed for: " << P.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    TypeConversion, CGModuleConvertTypeTest,
    ::testing::Values(
        ConvertTypeParam{"IntegerToI64", "INTEGER", 64},
        ConvertTypeParam{"BooleanToI1", "BOOLEAN", 1}
    ),
    [](const ::testing::TestParamInfo<ConvertTypeParam> &info) {
      return info.param.TestName;
    });

// 3. mangleName parametric tests
struct MangleNameParam {
  std::string TestName;
  std::string DeclName;
  std::string EnclosingName;
  std::string Expected;
};

class CGModuleMangleNameTest
    : public CGModuleTestBase,
      public ::testing::WithParamInterface<MangleNameParam> {};

TEST_P(CGModuleMangleNameTest, ProducesExpectedMangling) {
  const auto &P = GetParam();

  ModuleDeclaration EnclosingMod(nullptr, llvm::SMLoc(), P.EnclosingName);
  Decl *Enclosing = P.EnclosingName.empty() ? nullptr : &EnclosingMod;

  ProcedureDeclaration Inner(Enclosing, llvm::SMLoc(), P.DeclName);

  std::string Result = CGM->mangleName(&Inner);
  EXPECT_EQ(Result, P.Expected)
      << "Failed for: " << P.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    NameMangling, CGModuleMangleNameTest,
    ::testing::Values(
        MangleNameParam{"SimpleProc", "GCD", "", "_t3GCD"},
        MangleNameParam{"WithModule", "GCD", "Gcd", "_t3Gcd3GCD"},
        MangleNameParam{"SingleChar", "X", "M", "_t1M1X"},
        // Ch05-relevant: matches the AssignX symbol emitted from Point.mod.
        MangleNameParam{"AssignXInPoint", "AssignX", "Point", "_t5Point7AssignX"}
    ),
    [](const ::testing::TestParamInfo<MangleNameParam> &info) {
      return info.param.TestName;
    });

// 4. run() creates a global variable per VariableDeclaration.
struct RunGlobalVarParam {
  std::string TestName;
  std::vector<std::string> VarNames;
  unsigned ExpectedGlobalCount;
};

class CGModuleRunTest
    : public CGModuleTestBase,
      public ::testing::WithParamInterface<RunGlobalVarParam> {};

TEST_P(CGModuleRunTest, CreatesGlobalVariables) {
  const auto &P = GetParam();

  ModuleDeclaration Mod(nullptr, llvm::SMLoc(), "TestMod");

  DeclList Decls;
  std::vector<VariableDeclaration> Vars;
  Vars.reserve(P.VarNames.size());
  for (const auto &Name : P.VarNames) {
    Vars.emplace_back(&Mod, llvm::SMLoc(), Name, &IntTy);
  }
  for (auto &V : Vars)
    Decls.push_back(&V);

  Mod.setDecls(Decls);
  StmtList EmptyStmts;
  Mod.setStmts(EmptyStmts);

  CGM->run(&Mod);

  unsigned GlobalCount = 0;
  for ([[maybe_unused]] auto &G : LLVMMod->globals())
    ++GlobalCount;

  EXPECT_EQ(GlobalCount, P.ExpectedGlobalCount)
      << "Failed for: " << P.TestName;

  for (auto &V : Vars) {
    EXPECT_NE(CGM->getGlobal(&V), nullptr)
        << "Missing global for: " << V.getName().str();
  }
}

INSTANTIATE_TEST_SUITE_P(
    GlobalCreation, CGModuleRunTest,
    ::testing::Values(
        RunGlobalVarParam{"NoVars", {}, 0},
        RunGlobalVarParam{"OneVar", {"x"}, 1},
        RunGlobalVarParam{"ThreeVars", {"x", "y", "z"}, 3}
    ),
    [](const ::testing::TestParamInfo<RunGlobalVarParam> &info) {
      return info.param.TestName;
    });

// =========================================================================
// Ch05-specific: convertType for derived types
// =========================================================================

// ARRAY [N] OF INTEGER → LLVM ArrayType with i64 element, N elements.
TEST_F(CGModuleTestBase, ConvertArrayType) {
  llvm::APSInt SizeVal(64, /*isUnsigned=*/false);
  SizeVal = 4;
  IntegerLiteral SizeLit(llvm::SMLoc(), SizeVal, &IntTy);

  ArrayTypeDeclaration Arr(nullptr, llvm::SMLoc(), "Arr",
                           &SizeLit, &IntTy);
  llvm::Type *T = CGM->convertType(&Arr);
  ASSERT_NE(T, nullptr);
  ASSERT_TRUE(T->isArrayTy());
  EXPECT_EQ(T->getArrayNumElements(), 4u);
  EXPECT_TRUE(T->getArrayElementType()->isIntegerTy(64));
}

// RECORD with two INTEGER fields → LLVM StructType with two i64 elements.
TEST_F(CGModuleTestBase, ConvertRecordType) {
  FieldList Fields;
  Fields.emplace_back(llvm::SMLoc(), "X", &IntTy);
  Fields.emplace_back(llvm::SMLoc(), "Y", &IntTy);

  RecordTypeDeclaration Rec(nullptr, llvm::SMLoc(), "Point", Fields);
  llvm::Type *T = CGM->convertType(&Rec);
  ASSERT_NE(T, nullptr);
  ASSERT_TRUE(T->isStructTy());
  auto *ST = llvm::cast<llvm::StructType>(T);
  EXPECT_EQ(ST->getName().str(), "Point");
  ASSERT_EQ(ST->getNumElements(), 2u);
  EXPECT_TRUE(ST->getElementType(0)->isIntegerTy(64));
  EXPECT_TRUE(ST->getElementType(1)->isIntegerTy(64));
}

// Alias type passes through to the aliased type.
TEST_F(CGModuleTestBase, ConvertAliasType) {
  AliasTypeDeclaration Alias(nullptr, llvm::SMLoc(), "I", &IntTy);
  llvm::Type *T = CGM->convertType(&Alias);
  ASSERT_NE(T, nullptr);
  EXPECT_TRUE(T->isIntegerTy(64));
}

// Same TypeDeclaration converted twice returns the cached llvm::Type.
TEST_F(CGModuleTestBase, ConvertTypeIsCached) {
  FieldList Fields;
  Fields.emplace_back(llvm::SMLoc(), "X", &IntTy);
  RecordTypeDeclaration Rec(nullptr, llvm::SMLoc(), "R", Fields);

  llvm::Type *T1 = CGM->convertType(&Rec);
  llvm::Type *T2 = CGM->convertType(&Rec);
  EXPECT_EQ(T1, T2);
}

} // namespace
