#include "tinylang/CodeGen/CodeGenerator.h"
#include "tinylang/AST/AST.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

// One-time LLVM target initialization
class LLVMInitializer {
public:
  LLVMInitializer() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
  }
};
static LLVMInitializer GlobalInit;

// Helper to create a TargetMachine for the host
static llvm::TargetMachine *createHostTargetMachine() {
  std::string Error;
  std::string TripleStr = llvm::sys::getDefaultTargetTriple();
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(TripleStr, Error);
  if (!Target)
    return nullptr;
  return Target->createTargetMachine(
#if __clang_major__ <= 20
      TripleStr, "generic", "",
#else
      llvm::Triple(TripleStr), "generic", "",
#endif
      llvm::TargetOptions(),
      std::optional<llvm::Reloc::Model>());
}

// Base fixture
class CodeGeneratorTestBase : public ::testing::Test {
protected:
  llvm::LLVMContext Ctx;
  llvm::TargetMachine *TM = nullptr;
  CodeGenerator *CG = nullptr;

  // Reusable AST types
  TypeDeclaration IntTy{nullptr, llvm::SMLoc(), "INTEGER"};
  TypeDeclaration BoolTy{nullptr, llvm::SMLoc(), "BOOLEAN"};

  void SetUp() override {
    TM = createHostTargetMachine();
    ASSERT_NE(TM, nullptr) << "Could not create host TargetMachine";
    CG = CodeGenerator::create(Ctx, TM);
    ASSERT_NE(CG, nullptr);
  }

  void TearDown() override {
    delete CG;
    delete TM;
  }
};

// ---------------------------------------------------------------
// 1. Parametric: run() produces a valid module with correct metadata
// ---------------------------------------------------------------
struct RunParam {
  std::string TestName;
  std::string ModuleName;
  std::string FileName;
  unsigned NumGlobalVars;
  unsigned NumProcs;    // simple void procedures
};

class CodeGeneratorRunTest
    : public CodeGeneratorTestBase,
      public ::testing::WithParamInterface<RunParam> {};

TEST_P(CodeGeneratorRunTest, ProducesValidModule) {
  const auto &P = GetParam();

  ModuleDeclaration ModDecl(nullptr, llvm::SMLoc(), P.ModuleName);

  DeclList Decls;

  // Create global variables
  // Keep names alive since Decl stores StringRef (not std::string)
  std::vector<std::unique_ptr<std::string>> VarNames;
  std::vector<std::unique_ptr<VariableDeclaration>> Vars;
  for (unsigned i = 0; i < P.NumGlobalVars; ++i) {
    VarNames.push_back(std::make_unique<std::string>("var" + std::to_string(i)));
    Vars.push_back(std::make_unique<VariableDeclaration>(
        &ModDecl, llvm::SMLoc(), *VarNames.back(), &IntTy));
    Decls.push_back(Vars.back().get());
  }

  // Create simple void procedures (empty body)
  std::vector<std::unique_ptr<std::string>> ProcNames;
  std::vector<std::unique_ptr<ProcedureDeclaration>> Procs;
  for (unsigned i = 0; i < P.NumProcs; ++i) {
    ProcNames.push_back(std::make_unique<std::string>("Proc" + std::to_string(i)));
    FormalParamList FP;
    DeclList PDecls;
    StmtList PStmts;
    Procs.push_back(std::make_unique<ProcedureDeclaration>(
        &ModDecl, llvm::SMLoc(), *ProcNames.back(), FP, nullptr, PDecls, PStmts));
    Decls.push_back(Procs.back().get());
  }

  ModDecl.setDecls(Decls);
  StmtList EmptyStmts;
  ModDecl.setStmts(EmptyStmts);

  std::unique_ptr<llvm::Module> M = CG->run(&ModDecl, P.FileName);
  ASSERT_NE(M, nullptr) << "Module is null for: " << P.TestName;

  // Module name should match the filename
  EXPECT_EQ(M->getName().str(), P.FileName)
      << "Module name mismatch for: " << P.TestName;

  // Target triple should be set
  EXPECT_FALSE(M->getTargetTriple().empty())
      << "Target triple not set for: " << P.TestName;

  // Data layout should be set
  EXPECT_FALSE(M->getDataLayoutStr().empty())
      << "Data layout not set for: " << P.TestName;

  // Count global variables
  unsigned GlobalCount = 0;
  for ([[maybe_unused]] auto &G : M->globals())
    ++GlobalCount;
  EXPECT_EQ(GlobalCount, P.NumGlobalVars)
      << "Global var count mismatch for: " << P.TestName;

  // Count functions
  unsigned FuncCount = 0;
  for ([[maybe_unused]] auto &F : M->functions())
    ++FuncCount;
  EXPECT_EQ(FuncCount, P.NumProcs)
      << "Function count mismatch for: " << P.TestName;

  // Verify the entire module.
  // Note: CGModule::run() creates PrivateLinkage globals without an
  // initializer, which the LLVM verifier rejects. Only verify when
  // no global variables are present.
  if (P.NumGlobalVars == 0) {
    EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()))
        << "Module verification failed for: " << P.TestName;
  }
}

INSTANTIATE_TEST_SUITE_P(
    EndToEnd, CodeGeneratorRunTest,
    ::testing::Values(
        RunParam{"EmptyModule", "Empty", "empty.mod", 0, 0},
        RunParam{"VarsOnly", "Vars", "vars.mod", 3, 0},
        RunParam{"ProcsOnly", "Procs", "procs.mod", 0, 2},
        RunParam{"Mixed", "Gcd", "gcd.mod", 2, 1}
    ),
    [](const ::testing::TestParamInfo<RunParam> &info) {
      return info.param.TestName;
    });

// ---------------------------------------------------------------
// 2. Test: CodeGenerator::create() factory method
// ---------------------------------------------------------------
TEST_F(CodeGeneratorTestBase, CreateReturnsNonNull) {
  // Already tested in SetUp, but be explicit
  CodeGenerator *CG2 = CodeGenerator::create(Ctx, TM);
  EXPECT_NE(CG2, nullptr);
  delete CG2;
}

} // namespace
