#include "tinylang/Parser/Parser.h"
#include "tinylang/AST/AST.h"
#include "tinylang/Sema/Sema.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"
#include "gmock/gmock.h"

using namespace tinylang;
using namespace testing;

namespace {

// Expose protected methods for granular testing
class ParserTestee : public Parser {
public:
  using Parser::Parser;
  using Parser::parseCompilationUnit;
  using Parser::parseImport;
};

// Mock Sema to verify specific code paths
class MockSema : public Sema {
public:
  MockSema(DiagnosticsEngine &Diags) : Sema(Diags) {}
  MOCK_METHOD(ModuleDeclaration*, actOnModuleDeclaration, (SMLoc Loc, StringRef Name));
  MOCK_METHOD(void, actOnModuleDeclaration, (ModuleDeclaration* Mod, SMLoc Loc, StringRef Name, DeclList &Decls, StmtList &Stmts));

  MOCK_METHOD(void, actOnImport, (StringRef ModuleName, IdentList &Ids));
};

struct TestParam {
  std::string TestName;
  std::string Source;
  unsigned ExpectedErrors;  // from expect() not comsume()
  bool ExpectSemaCall1;
  bool ExpectSemaCall2;
  bool Expected;
};

class ParserTester{
protected:
  llvm::SourceMgr SrcMgr;
  DiagnosticsEngine Diags;
  NiceMock<MockSema> Actions;

  ParserTester() : Diags(SrcMgr), Actions(Diags) {}

  std::unique_ptr<Lexer> makeLexer(const std::string &Source) {
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer(Source);
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    return std::make_unique<Lexer>(SrcMgr, Diags);
  }
};

class ParserCompilationUnitTest : public ParserTester, public TestWithParam<TestParam> {};

TEST_P(ParserCompilationUnitTest, ValidateBranches) {
  const auto &Param = GetParam();
  auto Lex = makeLexer(Param.Source);
  ParserTestee P(*Lex, Actions);

  // Inside your test case
  ModuleDeclaration DummyModule; // Create a stack-allocated dummy

  if (Param.ExpectSemaCall1) {
    EXPECT_CALL(Actions, actOnModuleDeclaration(_, _))
    .WillOnce(Return(&DummyModule));
    //EXPECT_CALL(Actions, actOnModuleDeclaration(_, _)).Times(AtLeast(1));
  } else {
    EXPECT_CALL(Actions, actOnModuleDeclaration(_, _)).Times(0);
  }

  if (Param.ExpectSemaCall2) {
    EXPECT_CALL(Actions, actOnModuleDeclaration(_, _, _, _, _)).Times(AtLeast(1));
  } else {
    EXPECT_CALL(Actions, actOnModuleDeclaration(_, _, _, _, _)).Times(0);
  }

  ModuleDeclaration *D = nullptr;
  bool result = P.parseCompilationUnit(D);

    // Verify if the semantic action for the module header is reached

  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failure in test case: " << Param.TestName;

  EXPECT_EQ(result, Param.Expected)
      << "Failure in the return value: " << Param.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    LineByLineTests, ParserCompilationUnitTest,
    Values(
        // Line: if (consume(tok::kw_MODULE))
        TestParam{"MissingModuleKw", "M; END M.", 0, false, false, true},

        // Line: if (expect(tok::identifier))
        TestParam{"MissingName", "MODULE ; END M.", 1, false, false, true},

        // Line: if (consume(tok::semi))
        TestParam{"MissingSemicolon", "MODULE M END M.", 0, true, false, true},

        // Line: if (expect(tok::identifier))
        TestParam{"MissingClosingName", "MODULE M; END .", 1, true, false, true},

        // Line: if (consume(tok::period))
        TestParam{"MissingPeriod", "MODULE M; END M", 0, true, true, true},

        // Full valid path
        TestParam{"ValidMinimal", "MODULE M; END M.", 0, true, true, false},

        // Line: while (Tok.isOneOf(tok::kw_FROM, tok::kw_IMPORT))
        TestParam{"ValidWithImport", "MODULE M; IMPORT X; END M.", 0, true, true, false},

        TestParam{"MultipleImports",
          "MODULE Imp; IMPORT A; FROM C IMPORT D; END Imp.",
          0, true, true, false
        },

        TestParam{"DeclarationsOnly",
          "MODULE Decls; CONST c = 100; VAR v: INTEGER; END Decls.",
          0, true, true, false
        },

        TestParam{"WithProcedure",
          "MODULE Proc; PROCEDURE P; BEGIN END P; END Proc.",
          0, true, true, false
        },

        TestParam{"WithBody",
          "MODULE Main; VAR x: INTEGER; BEGIN x := 1; END Main.",
          0, true, true, false
        },

        // 6. Comprehensive Module (Full Grammar Path)
        TestParam{"FullModule",
            "MODULE Gcd;"
            "VAR x: INTEGER;"
            "PROCEDURE GCD(a, b: INTEGER) : INTEGER;"
            "VAR t: INTEGER;"
            "BEGIN"
            "  IF b = 0 THEN"
            "    RETURN a;"
            "  END;"
            "  WHILE b # 0 DO"
            "    t := a MOD b;"
            "    a := b;"
            "    b := t;"
            "  END;"
            "  RETURN a;"
            "END GCD;"
            "END Gcd.",
            0, true, true, false
        }
    ),
    [](const TestParamInfo<TestParam> &info) { return info.param.TestName; }
);


class ParserImportTest : public ParserTester, public TestWithParam<TestParam> {};

TEST_P(ParserImportTest, ValidateImportBranches) {
  const auto &Param = GetParam();
  auto Lex = makeLexer(Param.Source);
  ParserTestee P(*Lex, Actions);

  if (Param.ExpectSemaCall1) {
    EXPECT_CALL(Actions, actOnImport(_, _)).Times(1);
  } else {
    EXPECT_CALL(Actions, actOnImport(_, _)).Times(0);
  }

  bool result = P.parseImport();

  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors) << "Test: " << Param.TestName;
  EXPECT_EQ(result, Param.Expected) << "Test: " << Param.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    ImportTests, ParserImportTest,
    // bool ExpectSemaCall2 is not used. -> all false
    // the last one (Expected value can be returned from skipUntil())
    Values(
        // 1. Success: Basic Import
        TestParam{"BasicImport", "IMPORT x;", 0, true, false, false},

        // 2. Success: Multi-identifier Import
        TestParam{"MultiImport", "IMPORT x, y, z;", 0, true, false, false},

        // 3. Success: FROM ... IMPORT style
        TestParam{"FromImport", "FROM MyMod IMPORT x;", 0, true, false, false},

        // 4. Line 82: Missing IMPORT keyword (consume returns true, no error)
        TestParam{"MissingImportKw", "FROM MyMod x;", 0, false, false, true},

        // 5. Line 79: Missing identifier after FROM (expect reports error)
        TestParam{"MissingFromIdent", "FROM ; IMPORT x;", 1, false, false, false},

        // 6. Line 86: Missing semicolon (expect reports error)
        TestParam{"MissingSemicolon", "IMPORT x", 1, false, false, true},

        // 7. Malformed identifier list
        TestParam{"BadIdentList", "IMPORT x, ;", 1, true, false, false}
    ),
    [](const TestParamInfo<TestParam> &info) { return info.param.TestName; }
);

} // namespace