#include "tinylang/Basic/Diagnostic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <vector>

using namespace tinylang;

namespace {

struct DiagTestData {
  unsigned ID;
  const char *Name;
  llvm::SourceMgr::DiagKind ExpectedKind;
  bool IsError;
};

std::vector<DiagTestData> GetDiagnosticTestData() {
  return {
#define DIAG(ID, Level, Msg)                                                   \
  {diag::ID, #ID, llvm::SourceMgr::DK_##Level,                                 \
   (llvm::SourceMgr::DK_##Level == llvm::SourceMgr::DK_Error)},
#include "tinylang/Basic/Diagnostic.def"
#undef DIAG
  };
}

class DiagnosticEngineTest : public ::testing::TestWithParam<DiagTestData> {
protected:
  llvm::SourceMgr SrcMgr;
  DiagnosticsEngine Diags;

  DiagnosticEngineTest() : Diags(SrcMgr) {}

  llvm::SMLoc getLoc() {
    auto MemBuffer = llvm::MemoryBuffer::getMemBuffer("test content", "test.mod");
    unsigned BufID = SrcMgr.AddNewSourceBuffer(std::move(MemBuffer), llvm::SMLoc());
    return llvm::SMLoc::getFromPointer(
        SrcMgr.getMemoryBuffer(BufID)->getBufferStart());
  }
};

TEST_P(DiagnosticEngineTest, AllDiagnostics) {
  const DiagTestData &Params = GetParam();
  unsigned InitialErrors = Diags.numErrors();

  Diags.report(getLoc(), Params.ID, "arg1", "arg2");

  if (Params.IsError) {
    EXPECT_EQ(Diags.numErrors(), InitialErrors + 1)
        << "Diagnostic " << Params.Name << " failed to increment error count.";
  } else {
    EXPECT_EQ(Diags.numErrors(), InitialErrors)
        << "Diagnostic " << Params.Name << " incremented error count incorrectly.";
  }
}

INSTANTIATE_TEST_SUITE_P(
    DiagnosticDefinitions, DiagnosticEngineTest,
    ::testing::ValuesIn(GetDiagnosticTestData()),
    [](const ::testing::TestParamInfo<DiagTestData> &info) {
      return info.param.Name;
    });

} // namespace
