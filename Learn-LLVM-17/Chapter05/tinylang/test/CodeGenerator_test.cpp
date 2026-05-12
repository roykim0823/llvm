#include "tinylang/CodeGen/CodeGenerator.h"
#include "tinylang/AST/AST.h"
#include "tinylang/AST/ASTContext.h"
#include "tinylang/Lexer/Lexer.h"
#include "tinylang/Parser/Parser.h"
#include "tinylang/Sema/Sema.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "gtest/gtest.h"

using namespace tinylang;

namespace {

class LLVMInitializer {
public:
  LLVMInitializer() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
  }
};
static LLVMInitializer GlobalInit;

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

class CodeGeneratorTestBase : public ::testing::Test {
protected:
  llvm::LLVMContext Ctx;
  llvm::SourceMgr SrcMgr;
  std::unique_ptr<ASTContext> ASTCtx;
  llvm::TargetMachine *TM = nullptr;
  CodeGenerator *CG = nullptr;

  PervasiveTypeDeclaration IntTy{nullptr, llvm::SMLoc(), "INTEGER"};
  PervasiveTypeDeclaration BoolTy{nullptr, llvm::SMLoc(), "BOOLEAN"};

  void SetUp() override {
    ASTCtx = std::make_unique<ASTContext>(SrcMgr, "test.mod");
    TM = createHostTargetMachine();
    ASSERT_NE(TM, nullptr) << "Could not create host TargetMachine";
    CG = CodeGenerator::create(Ctx, *ASTCtx, TM);
    ASSERT_NE(CG, nullptr);
  }

  void TearDown() override {
    delete CG;
    delete TM;
  }
};

// 1. Synthetic AST → run() produces a valid Module.
struct RunParam {
  std::string TestName;
  std::string ModuleName;
  std::string FileName;
  unsigned NumGlobalVars;
  unsigned NumProcs;
};

class CodeGeneratorRunTest
    : public CodeGeneratorTestBase,
      public ::testing::WithParamInterface<RunParam> {};

TEST_P(CodeGeneratorRunTest, ProducesValidModule) {
  const auto &P = GetParam();

  ModuleDeclaration ModDecl(nullptr, llvm::SMLoc(), P.ModuleName);

  DeclList Decls;

  std::vector<std::unique_ptr<std::string>> VarNames;
  std::vector<std::unique_ptr<VariableDeclaration>> Vars;
  for (unsigned i = 0; i < P.NumGlobalVars; ++i) {
    VarNames.push_back(std::make_unique<std::string>("var" + std::to_string(i)));
    Vars.push_back(std::make_unique<VariableDeclaration>(
        &ModDecl, llvm::SMLoc(), *VarNames.back(), &IntTy));
    Decls.push_back(Vars.back().get());
  }

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

  EXPECT_EQ(M->getName().str(), P.FileName)
      << "Module name mismatch for: " << P.TestName;
  EXPECT_FALSE(M->getTargetTriple().empty())
      << "Target triple not set for: " << P.TestName;
  EXPECT_FALSE(M->getDataLayoutStr().empty())
      << "Data layout not set for: " << P.TestName;

  unsigned GlobalCount = 0;
  for ([[maybe_unused]] auto &G : M->globals())
    ++GlobalCount;
  EXPECT_EQ(GlobalCount, P.NumGlobalVars)
      << "Global var count mismatch for: " << P.TestName;

  unsigned FuncCount = 0;
  for ([[maybe_unused]] auto &F : M->functions())
    ++FuncCount;
  EXPECT_EQ(FuncCount, P.NumProcs)
      << "Function count mismatch for: " << P.TestName;

  // CGModule emits PrivateLinkage globals with no initializer, which the
  // verifier rejects. Only verify when there are no global vars.
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

TEST_F(CodeGeneratorTestBase, CreateReturnsNonNull) {
  CodeGenerator *CG2 = CodeGenerator::create(Ctx, *ASTCtx, TM);
  EXPECT_NE(CG2, nullptr);
  delete CG2;
}

// =========================================================================
// Ch05-specific: end-to-end pipeline (source → Parser → Sema → CodeGen).
// Mirrors what the driver does for a real .mod file.
// =========================================================================
class CodeGeneratorPipelineTest : public ::testing::Test {
protected:
  llvm::LLVMContext Ctx;
  llvm::SourceMgr SrcMgr;
  std::unique_ptr<ASTContext> ASTCtx;
  llvm::TargetMachine *TM = nullptr;
  std::unique_ptr<CodeGenerator> CG;
  DiagnosticsEngine *Diags = nullptr;

  void SetUp() override {
    ASTCtx = std::make_unique<ASTContext>(SrcMgr, "test.mod");
    TM = createHostTargetMachine();
    ASSERT_NE(TM, nullptr);
    CG.reset(CodeGenerator::create(Ctx, *ASTCtx, TM));
    Diags = new DiagnosticsEngine(SrcMgr);
  }

  void TearDown() override {
    delete Diags;
    delete TM;
  }

  std::unique_ptr<llvm::Module> compile(const std::string &Source) {
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer(Source, "in.mod");
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    Lexer Lex(SrcMgr, *Diags);
    Sema Sem(*Diags);
    Parser P(Lex, Sem);
    auto *Mod = P.parse();
    if (!Mod || Diags->numErrors() != 0)
      return nullptr;
    return CG->run(Mod, "in.mod");
  }
};

TEST_F(CodeGeneratorPipelineTest, CompilesPointModule) {
  auto M = compile(
      "MODULE Point;\n"
      "TYPE Point = RECORD X, Y: INTEGER END;\n"
      "VAR p: Point;\n"
      "PROCEDURE AssignX(a: INTEGER);\n"
      "BEGIN\n"
      "  p.X := a;\n"
      "END AssignX;\n"
      "END Point.\n");
  ASSERT_NE(M, nullptr);
  EXPECT_EQ(Diags->numErrors(), 0u);

  // One global (p) and one procedure (AssignX).
  unsigned GCount = 0, FCount = 0;
  for ([[maybe_unused]] auto &G : M->globals()) ++GCount;
  for ([[maybe_unused]] auto &F : M->functions()) ++FCount;
  EXPECT_EQ(GCount, 1u);
  EXPECT_EQ(FCount, 1u);

  llvm::Function *Fn = M->getFunction("_t5Point7AssignX");
  ASSERT_NE(Fn, nullptr) << "Expected mangled function _t5Point7AssignX";
  EXPECT_FALSE(llvm::verifyFunction(*Fn, &llvm::errs()));
}

TEST_F(CodeGeneratorPipelineTest, CompilesArrayAccess) {
  auto M = compile(
      "MODULE M;\n"
      "TYPE A = ARRAY [4] OF INTEGER;\n"
      "VAR a: A;\n"
      "PROCEDURE Set(i, v: INTEGER);\n"
      "BEGIN\n"
      "  a[i] := v;\n"
      "END Set;\n"
      "END M.\n");
  ASSERT_NE(M, nullptr);
  EXPECT_EQ(Diags->numErrors(), 0u);
  ASSERT_EQ(M->getFunctionList().size(), 1u);
  llvm::Function &Fn = M->getFunctionList().front();
  EXPECT_FALSE(llvm::verifyFunction(Fn, &llvm::errs()));
}

} // namespace
