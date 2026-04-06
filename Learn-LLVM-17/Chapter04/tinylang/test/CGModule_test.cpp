#include "tinylang/CodeGen/CGModule.h"
#include "tinylang/AST/AST.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

// Base fixture: creates an LLVMContext, Module, and CGModule
class CGModuleTestBase : public ::testing::Test {
protected:
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::Module> LLVMMod;
  std::unique_ptr<CGModule> CGM;

  // Persistent AST nodes (owned by the test)
  ModuleDeclaration ModDecl;
  TypeDeclaration IntTy{nullptr, llvm::SMLoc(), "INTEGER"};
  TypeDeclaration BoolTy{nullptr, llvm::SMLoc(), "BOOLEAN"};

  void SetUp() override {
    LLVMMod = std::make_unique<llvm::Module>("test", Ctx);
    CGM = std::make_unique<CGModule>(LLVMMod.get());
  }
};

// ---------------------------------------------------------------
// 1. Tests for initialize() -- type and constant setup
// ---------------------------------------------------------------
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

// ---------------------------------------------------------------
// 2. Parametric tests for convertType()
// ---------------------------------------------------------------
struct ConvertTypeParam {
  std::string TestName;
  std::string TypeName;  // "INTEGER" or "BOOLEAN"
  unsigned ExpectedBitWidth;
};

class CGModuleConvertTypeTest
    : public CGModuleTestBase,
      public ::testing::WithParamInterface<ConvertTypeParam> {};

TEST_P(CGModuleConvertTypeTest, ConvertsToCorrectLLVMType) {
  const auto &P = GetParam();
  TypeDeclaration Ty(nullptr, llvm::SMLoc(), P.TypeName);
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

// ---------------------------------------------------------------
// 3. Parametric tests for mangleName()
// ---------------------------------------------------------------
struct MangleNameParam {
  std::string TestName;
  std::string DeclName;
  std::string EnclosingName;  // empty means no enclosing decl
  std::string Expected;
};

class CGModuleMangleNameTest
    : public CGModuleTestBase,
      public ::testing::WithParamInterface<MangleNameParam> {};

TEST_P(CGModuleMangleNameTest, ProducesExpectedMangling) {
  const auto &P = GetParam();

  // Build a Decl chain: optionally with an enclosing module decl
  ModuleDeclaration EnclosingMod(nullptr, llvm::SMLoc(), P.EnclosingName);
  Decl *Enclosing = P.EnclosingName.empty() ? nullptr : &EnclosingMod;

  // Use a ProcedureDeclaration as the inner decl (any Decl subclass works)
  ProcedureDeclaration Inner(Enclosing, llvm::SMLoc(), P.DeclName);

  std::string Result = CGM->mangleName(&Inner);
  EXPECT_EQ(Result, P.Expected)
      << "Failed for: " << P.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    NameMangling, CGModuleMangleNameTest,
    ::testing::Values(
        // _t + len(Name) + Name for each part bottom-up
        MangleNameParam{"SimpleProc", "GCD", "", "_t3GCD"},
        MangleNameParam{"WithModule", "GCD", "Gcd", "_t3Gcd3GCD"},
        MangleNameParam{"SingleChar", "X", "M", "_t1M1X"}
    ),
    [](const ::testing::TestParamInfo<MangleNameParam> &info) {
      return info.param.TestName;
    });

// ---------------------------------------------------------------
// 4. Parametric tests for run() -- global variable creation
// ---------------------------------------------------------------
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

  // Create VariableDeclarations owned by the module
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

  // Count globals in the LLVM module
  unsigned GlobalCount = 0;
  for (auto &G : LLVMMod->globals())
    ++GlobalCount;

  EXPECT_EQ(GlobalCount, P.ExpectedGlobalCount)
      << "Failed for: " << P.TestName;

  // Each variable should be retrievable via getGlobal
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

} // namespace
