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

  // Helper to initialize the Lexer with a source string
  std::unique_ptr<Lexer> makeLexer(const char *Source) {
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer(Source);
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    return std::make_unique<Lexer>(SrcMgr, Diags);
  }

  // Tokenizes the entire input into a vector for verification
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

// --- 1. Parametric Tests for Standard Tokens ---

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

  // Verify content for identifiers and literals
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
        // Identifiers & Keywords [cite: 10, 11, 15]
        LexerParam{"myVar", tok::identifier, "myVar"},
        LexerParam{"MODULE", tok::kw_MODULE, nullptr},
        LexerParam{"PROCEDURE", tok::kw_PROCEDURE, nullptr},
        // Numbers
        LexerParam{"1234", tok::integer_literal, "1234"},
        LexerParam{"0FFH", tok::integer_literal, "0FFH"},
        // Strings
        LexerParam{"\"hello\"", tok::string_literal, "\"hello\""},
        LexerParam{"'world'", tok::string_literal, "'world'"},
        // Punctuators [cite: 9, 10, 15]
        LexerParam{":=", tok::colonequal, nullptr},
        LexerParam{"<=", tok::lessequal, nullptr},
        LexerParam{".", tok::period, nullptr}
    ));

// --- 2. Functional Tests for Complex Logic ---

TEST_F(LexerTest, SkipsWhitespace) {
  // Test both horizontal and vertical whitespace
  auto Tokens = tokenize("  \t\n  VAR \r\n  x  ");
  ASSERT_EQ(Tokens.size(), 3u); // [VAR] [x] [eof]
  EXPECT_EQ(Tokens[0].getKind(), tok::kw_VAR);
  EXPECT_EQ(Tokens[1].getKind(), tok::identifier);
}

TEST_F(LexerTest, HandlesNestedComments) {
  // Lexer should skip entire nested blocks and return the following token
  auto Tokens = tokenize("(* Outer (* Inner *) Outer *) identifier");
  ASSERT_EQ(Tokens.size(), 2u); // [identifier] [eof]
  EXPECT_EQ(Tokens[0].getKind(), tok::identifier);
}

TEST_F(LexerTest, DiagnosticsReporting) {
  // Test Case: Hex digit in decimal number
  {
    tokenize("12A");
    EXPECT_EQ(Diags.numErrors(), 1u);
  }

  // Test Case: Unterminated string literal
  {
    DiagnosticsEngine LocalDiags(SrcMgr); // Reset error count
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer("'unterminated");
    SrcMgr.AddNewSourceBuffer(std::move(MemBuf), llvm::SMLoc());
    Lexer L(SrcMgr, LocalDiags);
    Token Tok;
    L.next(Tok);
    EXPECT_EQ(LocalDiags.numErrors(), 1u);
  }

  // Test Case: Unterminated block comment
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

} // namespace