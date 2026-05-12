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

class ParserTestee : public Parser {
public:
  using Parser::Parser;
  using Parser::parseCompilationUnit;
  using Parser::parseImport;
};

// Mocks Sema's virtual entry points to verify which actions the parser invokes.
class MockSema : public Sema {
public:
  MockSema(DiagnosticsEngine &Diags) : Sema(Diags) {}
  MOCK_METHOD(ModuleDeclaration*, actOnModuleDeclaration, (SMLoc Loc, StringRef Name), (override));
  MOCK_METHOD(void, actOnModuleDeclaration,
              (ModuleDeclaration* Mod, SMLoc Loc, StringRef Name, DeclList &Decls, StmtList &Stmts),
              (override));
  MOCK_METHOD(void, actOnImport, (StringRef ModuleName, IdentList &Ids), (override));
};

struct TestParam {
  std::string TestName;
  std::string Source;
  unsigned ExpectedErrors;
  bool ExpectSemaCall1;
  bool ExpectSemaCall2;
  bool Expected;
};

class ParserTester {
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

  ModuleDeclaration DummyModule(nullptr, llvm::SMLoc(), "DUMMY");

  if (Param.ExpectSemaCall1) {
    EXPECT_CALL(Actions, actOnModuleDeclaration(_, _))
      .WillOnce(Return(&DummyModule));
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

  EXPECT_EQ(Diags.numErrors(), Param.ExpectedErrors)
      << "Failure in test case: " << Param.TestName;
  EXPECT_EQ(result, Param.Expected)
      << "Failure in the return value: " << Param.TestName;
}

INSTANTIATE_TEST_SUITE_P(
    LineByLineTests, ParserCompilationUnitTest,
    Values(
        TestParam{"MissingModuleKw", "M; END M.", 0, false, false, true},
        TestParam{"MissingName", "MODULE ; END M.", 1, false, false, true},
        TestParam{"MissingSemicolon", "MODULE M END M.", 0, true, false, true},
        TestParam{"MissingClosingName", "MODULE M; END .", 1, true, false, true},
        TestParam{"MissingPeriod", "MODULE M; END M", 0, true, true, true},
        TestParam{"ValidMinimal", "MODULE M; END M.", 0, true, true, false},
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
        },

        // Ch05-specific: TYPE block must parse.
        TestParam{"WithRecordType",
          "MODULE M; TYPE P = RECORD X, Y: INTEGER END; END M.",
          0, true, true, false
        },
        TestParam{"WithArrayType",
          "MODULE M; TYPE A = ARRAY [10] OF INTEGER; END M.",
          0, true, true, false
        },
        TestParam{"WithPointerType",
          "MODULE M; TYPE PI = POINTER TO INTEGER; END M.",
          0, true, true, false
        },
        TestParam{"WithAliasType",
          "MODULE M; TYPE I = INTEGER; END M.",
          0, true, true, false
        },
        TestParam{"PointMod",
          "MODULE Point;"
          "TYPE Point = RECORD X, Y: INTEGER END;"
          "VAR p: Point;"
          "PROCEDURE AssignX(a: INTEGER);"
          "BEGIN"
          "  p.X := a;"
          "END AssignX;"
          "END Point.",
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
    Values(
        TestParam{"BasicImport", "IMPORT x;", 0, true, false, false},
        TestParam{"MultiImport", "IMPORT x, y, z;", 0, true, false, false},
        TestParam{"FromImport", "FROM MyMod IMPORT x;", 0, true, false, false},
        TestParam{"MissingImportKw", "FROM MyMod x;", 0, false, false, true},
        TestParam{"MissingFromIdent", "FROM ; IMPORT x;", 1, false, false, false},
        TestParam{"MissingSemicolon", "IMPORT x", 1, false, false, true},
        TestParam{"BadIdentList", "IMPORT x, ;", 1, true, false, false}
    ),
    [](const TestParamInfo<TestParam> &info) { return info.param.TestName; }
);

// =========================================================================
// Ch05-specific: end-to-end parse of TYPE declarations with real Sema,
// verifying the produced AST contains the expected declarations.
// =========================================================================
class ParserTypeDeclTest : public ::testing::Test {
protected:
  llvm::SourceMgr SrcMgr;
  DiagnosticsEngine Diags;
  Sema Semantics;

  ParserTypeDeclTest() : Diags(SrcMgr), Semantics(Diags) {}

  ModuleDeclaration *parse(const std::string &Source) {
    // Must copy: AST nodes hold StringRefs into the buffer, and the source
    // string passed in may be a temporary.
    auto MemBuf = llvm::MemoryBuffer::getMemBufferCopy(Source, "in.mod");
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    auto L = std::make_unique<Lexer>(SrcMgr, Diags);
    Parser P(*L, Semantics);
    return P.parse();
  }

  template <typename DeclT>
  DeclT *findDecl(ModuleDeclaration *Mod, llvm::StringRef Name) {
    for (Decl *D : Mod->getDecls()) {
      if (auto *T = dyn_cast<DeclT>(D))
        if (T->getName() == Name)
          return T;
    }
    return nullptr;
  }
};

TEST_F(ParserTypeDeclTest, RecordWithMultipleFields) {
  auto *Mod = parse("MODULE M; TYPE P = RECORD X, Y: INTEGER END; END M.");
  ASSERT_NE(Mod, nullptr);
  EXPECT_EQ(Diags.numErrors(), 0u);
  auto *Rec = findDecl<RecordTypeDeclaration>(Mod, "P");
  ASSERT_NE(Rec, nullptr);
  ASSERT_EQ(Rec->getFields().size(), 2u);
  EXPECT_EQ(Rec->getFields()[0].getName(), "X");
  EXPECT_EQ(Rec->getFields()[1].getName(), "Y");
}

TEST_F(ParserTypeDeclTest, ArrayWithConstantSize) {
  auto *Mod = parse("MODULE M; TYPE A = ARRAY [10] OF INTEGER; END M.");
  ASSERT_NE(Mod, nullptr);
  EXPECT_EQ(Diags.numErrors(), 0u);
  auto *Arr = findDecl<ArrayTypeDeclaration>(Mod, "A");
  ASSERT_NE(Arr, nullptr);
  EXPECT_NE(Arr->getNums(), nullptr);
  EXPECT_NE(Arr->getType(), nullptr);
}

TEST_F(ParserTypeDeclTest, PointerToInteger) {
  auto *Mod = parse("MODULE M; TYPE PI = POINTER TO INTEGER; END M.");
  ASSERT_NE(Mod, nullptr);
  EXPECT_EQ(Diags.numErrors(), 0u);
  auto *Ptr = findDecl<PointerTypeDeclaration>(Mod, "PI");
  ASSERT_NE(Ptr, nullptr);
  EXPECT_NE(Ptr->getType(), nullptr);
}

TEST_F(ParserTypeDeclTest, AliasOfInteger) {
  auto *Mod = parse("MODULE M; TYPE I = INTEGER; END M.");
  ASSERT_NE(Mod, nullptr);
  EXPECT_EQ(Diags.numErrors(), 0u);
  auto *A = findDecl<AliasTypeDeclaration>(Mod, "I");
  ASSERT_NE(A, nullptr);
  EXPECT_NE(A->getType(), nullptr);
}

} // namespace
