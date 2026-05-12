/// \file
/// \brief Unit tests for tinylang::Lexer (token recognition + diagnostics).
///
/// Two suites:
///  - `LexerTokenTest` is parametric over (input string, expected kind,
///    expected text). One row per representative token category.
///  - `LexerTest` covers behavioural cases that don't fit the parametric
///    shape: whitespace skipping, nested comment handling, and the three
///    lexical-error diagnostics.

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

  // Build a Lexer around \p Source. Note: getMemBuffer does *not* copy the
  // bytes; tokens hold pointers into them. The helper accepts a `const char*`
  // so callers naturally pass string literals (static storage, always
  // valid). If you ever need to pass a temporary std::string, switch to
  // getMemBufferCopy to avoid dangling pointers.
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
        // Identifiers are case-sensitive: lowercase "module" is *not* a keyword.
        LexerParam{"module", tok::identifier, "module"},
        LexerParam{"MODULE", tok::kw_MODULE, nullptr},
        LexerParam{"PROCEDURE", tok::kw_PROCEDURE, nullptr},
        LexerParam{"BEGIN", tok::kw_BEGIN, nullptr},
        LexerParam{"END", tok::kw_END, nullptr},
        LexerParam{"IF", tok::kw_IF, nullptr},
        LexerParam{"THEN", tok::kw_THEN, nullptr},
        LexerParam{"ELSE", tok::kw_ELSE, nullptr},
        LexerParam{"WHILE", tok::kw_WHILE, nullptr},
        LexerParam{"DO", tok::kw_DO, nullptr},
        LexerParam{"RETURN", tok::kw_RETURN, nullptr},
        LexerParam{"AND", tok::kw_AND, nullptr},
        LexerParam{"OR", tok::kw_OR, nullptr},
        LexerParam{"NOT", tok::kw_NOT, nullptr},
        LexerParam{"DIV", tok::kw_DIV, nullptr},
        LexerParam{"MOD", tok::kw_MOD, nullptr},
        // Numbers
        LexerParam{"1234", tok::integer_literal, "1234"},
        LexerParam{"0FFH", tok::integer_literal, "0FFH"},
        // Strings
        LexerParam{"\"hello\"", tok::string_literal, "\"hello\""},
        LexerParam{"'world'", tok::string_literal, "'world'"},
        // Punctuators [cite: 9, 10, 15]
        LexerParam{":=", tok::colonequal, nullptr},
        LexerParam{"<=", tok::lessequal, nullptr},
        LexerParam{">=", tok::greaterequal, nullptr},
        // Single-character punctuators that share a prefix with multi-character ones
        // (`:` vs `:=`, `<` vs `<=`, `>` vs `>=`). Catches off-by-one slips in the
        // lookahead branches inside Lexer::next.
        LexerParam{":", tok::colon, nullptr},
        LexerParam{"<", tok::less, nullptr},
        LexerParam{">", tok::greater, nullptr},
        LexerParam{"=", tok::equal, nullptr},
        LexerParam{"#", tok::hash, nullptr},
        LexerParam{"+", tok::plus, nullptr},
        LexerParam{"-", tok::minus, nullptr},
        LexerParam{"*", tok::star, nullptr},
        LexerParam{"/", tok::slash, nullptr},
        LexerParam{",", tok::comma, nullptr},
        LexerParam{";", tok::semi, nullptr},
        LexerParam{"(", tok::l_paren, nullptr},
        LexerParam{")", tok::r_paren, nullptr},
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

// Empty buffer must yield exactly one EOF token. Guards against an off-by-one
// in the leading-whitespace loop that could read past the buffer's NUL.
TEST_F(LexerTest, EmptyInputIsEof) {
  auto Tokens = tokenize("");
  ASSERT_EQ(Tokens.size(), 1u);
  EXPECT_EQ(Tokens[0].getKind(), tok::eof);
}

// An input consisting only of whitespace also collapses to a single EOF token.
TEST_F(LexerTest, WhitespaceOnlyInputIsEof) {
  auto Tokens = tokenize("   \t\n  \r\n  ");
  ASSERT_EQ(Tokens.size(), 1u);
  EXPECT_EQ(Tokens[0].getKind(), tok::eof);
}

// Lexer::next falls through to `tok::unknown` for any character it doesn't
// recognise. The character is *not* consumed by formToken, so iterating to EOF
// in tokenize() relies on the assumption that `next()` advances internally —
// it doesn't for unknown chars, which would loop forever. Therefore we drive
// the lexer manually here instead of going through tokenize().
TEST_F(LexerTest, UnknownCharacterProducesUnknownToken) {
  auto L = makeLexer("@");
  Token Tok;
  L->next(Tok);
  EXPECT_EQ(Tok.getKind(), tok::unknown);
}

// Spot-check Token's source-location plumbing: the second token's location
// should point at the byte immediately after the leading "x ".
TEST_F(LexerTest, TokenLocationsArePreserved) {
  const char *Src = "x y";
  auto L = makeLexer(Src);
  Token First, Second;
  L->next(First);
  L->next(Second);
  EXPECT_EQ(First.getKind(), tok::identifier);
  EXPECT_EQ(Second.getKind(), tok::identifier);
  // The pointer underlying SMLoc is taken from the same buffer we passed in,
  // so we can compare offsets directly.
  EXPECT_EQ(Second.getLocation().getPointer() -
                First.getLocation().getPointer(),
            2);
}

// A representative multi-token sequence: assignment statement
// `x := 42;`. Verifies the lexer's dispatch table threads tokens correctly
// when several disjoint paths (identifier, colonequal, integer, semi) fire in
// sequence.
TEST_F(LexerTest, TokenizesAssignmentSequence) {
  auto Tokens = tokenize("x := 42;");
  ASSERT_EQ(Tokens.size(), 5u); // [x] [:=] [42] [;] [eof]
  EXPECT_EQ(Tokens[0].getKind(), tok::identifier);
  EXPECT_EQ(Tokens[1].getKind(), tok::colonequal);
  EXPECT_EQ(Tokens[2].getKind(), tok::integer_literal);
  EXPECT_EQ(Tokens[3].getKind(), tok::semi);
  EXPECT_EQ(Tokens[4].getKind(), tok::eof);
}

// One TEST_F that bundles three independent lexical-error checks. Kept
// together because each path emits exactly one diagnostic and the assertions
// are one-liners. If any subcase grows beyond a few lines, split it into its
// own TEST_F so gtest reports the failing one in isolation.
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