#include "tinylang/Lexer/Lexer.h"
#include "tinylang/Basic/Diagnostic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <vector>

using namespace tinylang;

namespace {

class LexerTest : public ::testing::Test {
protected:
  llvm::SourceMgr SrcMgr;
  DiagnosticsEngine Diags;

  LexerTest() : Diags(SrcMgr) {}

  std::unique_ptr<Lexer> makeLexer(const char *Source) {
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer(Source);
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    return std::make_unique<Lexer>(SrcMgr, Diags);
  }

  std::vector<Token> tokenize(const char *Source) {
    auto L = makeLexer(Source);
    std::vector<Token> Tokens;
    Token Tok;
    do {
      L->next(Tok);
      Tokens.push_back(Tok);
    } while (Tok.isNot(tok::eof));
    return Tokens;
  }
};

struct LexerParam {
  const char *Input;
  tok::TokenKind ExpectedKind;
  const char *ExpectedText;
};

class LexerTokenTest : public LexerTest, public ::testing::WithParamInterface<LexerParam> {};

TEST_P(LexerTokenTest, CorrectlyIdentifiesToken) {
  auto Params = GetParam();
  auto Tokens = tokenize(Params.Input);

  ASSERT_FALSE(Tokens.empty());
  EXPECT_EQ(Tokens[0].getKind(), Params.ExpectedKind);

  if (Params.ExpectedText) {
    if (Tokens[0].is(tok::identifier))
      EXPECT_EQ(Tokens[0].getIdentifier(), Params.ExpectedText);
    else if (Tokens[0].isOneOf(tok::integer_literal, tok::string_literal))
      EXPECT_EQ(Tokens[0].getLiteralData(), Params.ExpectedText);
  }
}

INSTANTIATE_TEST_SUITE_P(
    CommonTokens, LexerTokenTest,
    ::testing::Values(
        // Carried over from Ch04
        LexerParam{"myVar", tok::identifier, "myVar"},
        LexerParam{"MODULE", tok::kw_MODULE, nullptr},
        LexerParam{"PROCEDURE", tok::kw_PROCEDURE, nullptr},
        LexerParam{"1234", tok::integer_literal, "1234"},
        LexerParam{"0FFH", tok::integer_literal, "0FFH"},
        LexerParam{"\"hello\"", tok::string_literal, "\"hello\""},
        LexerParam{"'world'", tok::string_literal, "'world'"},
        LexerParam{":=", tok::colonequal, nullptr},
        LexerParam{"<=", tok::lessequal, nullptr},
        LexerParam{".", tok::period, nullptr},

        // Ch05 additions
        LexerParam{"^", tok::caret, nullptr},
        LexerParam{"[", tok::l_square, nullptr},
        LexerParam{"]", tok::r_square, nullptr},
        LexerParam{"ARRAY", tok::kw_ARRAY, nullptr},
        LexerParam{"OF", tok::kw_OF, nullptr},
        LexerParam{"POINTER", tok::kw_POINTER, nullptr},
        LexerParam{"RECORD", tok::kw_RECORD, nullptr},
        LexerParam{"TO", tok::kw_TO, nullptr},
        LexerParam{"TYPE", tok::kw_TYPE, nullptr}
    ));

TEST_F(LexerTest, SkipsWhitespace) {
  auto Tokens = tokenize("  \t\n  VAR \r\n  x  ");
  ASSERT_EQ(Tokens.size(), 3u);
  EXPECT_EQ(Tokens[0].getKind(), tok::kw_VAR);
  EXPECT_EQ(Tokens[1].getKind(), tok::identifier);
}

TEST_F(LexerTest, HandlesNestedComments) {
  auto Tokens = tokenize("(* Outer (* Inner *) Outer *) identifier");
  ASSERT_EQ(Tokens.size(), 2u);
  EXPECT_EQ(Tokens[0].getKind(), tok::identifier);
}

TEST_F(LexerTest, DiagnosticsReporting) {
  {
    tokenize("12A");
    EXPECT_EQ(Diags.numErrors(), 1u);
  }
  {
    DiagnosticsEngine LocalDiags(SrcMgr);
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer("'unterminated");
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    Lexer L(SrcMgr, LocalDiags);
    Token Tok;
    L.next(Tok);
    EXPECT_EQ(LocalDiags.numErrors(), 1u);
  }
  {
    DiagnosticsEngine LocalDiags(SrcMgr);
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer("(* no end");
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    Lexer L(SrcMgr, LocalDiags);
    Token Tok;
    L.next(Tok);
    EXPECT_EQ(LocalDiags.numErrors(), 1u);
  }
}

// Ch05: Sequence that exercises the new punctuators in a realistic context.
TEST_F(LexerTest, RecordAndArrayTokenStream) {
  auto Tokens = tokenize("TYPE T = ARRAY [10] OF INTEGER;");
  ASSERT_GE(Tokens.size(), 9u);
  EXPECT_EQ(Tokens[0].getKind(), tok::kw_TYPE);
  EXPECT_EQ(Tokens[1].getKind(), tok::identifier);
  EXPECT_EQ(Tokens[2].getKind(), tok::equal);
  EXPECT_EQ(Tokens[3].getKind(), tok::kw_ARRAY);
  EXPECT_EQ(Tokens[4].getKind(), tok::l_square);
  EXPECT_EQ(Tokens[5].getKind(), tok::integer_literal);
  EXPECT_EQ(Tokens[6].getKind(), tok::r_square);
  EXPECT_EQ(Tokens[7].getKind(), tok::kw_OF);
  EXPECT_EQ(Tokens[8].getKind(), tok::identifier);
}

TEST_F(LexerTest, PointerDereferenceTokens) {
  auto Tokens = tokenize("p^.X");
  ASSERT_GE(Tokens.size(), 4u);
  EXPECT_EQ(Tokens[0].getKind(), tok::identifier);
  EXPECT_EQ(Tokens[1].getKind(), tok::caret);
  EXPECT_EQ(Tokens[2].getKind(), tok::period);
  EXPECT_EQ(Tokens[3].getKind(), tok::identifier);
}

} // namespace
