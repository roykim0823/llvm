//===- lexer_test.cpp - Unit tests for the buffer-based lexer -------------===//
//
// The LexerBuffer reads from a memory range, so tests feed it plain strings:
// no stdin redirection, no temp files, no shared global state.
//
//===----------------------------------------------------------------------===//

#include "toy/Lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace toy;

namespace {

/// Convenience: lex an entire string into a token sequence.
std::vector<Token> lexAll(const std::string &input) {
  LexerBuffer lexer(input.data(), input.data() + input.size(), "test.k");
  std::vector<Token> tokens;
  while (lexer.getNextToken() != tok_eof)
    tokens.push_back(lexer.getCurToken());
  return tokens;
}

struct TokenCase {
  std::string input;
  Token expected;
  std::string expectedIdentifier = "";
  double expectedValue = 0.0;
};

class SingleTokenTest : public ::testing::TestWithParam<TokenCase> {};

TEST_P(SingleTokenTest, LexesFirstToken) {
  const auto &param = GetParam();
  LexerBuffer lexer(param.input.data(), param.input.data() + param.input.size(),
                    "test.k");
  Token tok = lexer.getNextToken();
  EXPECT_EQ(tok, param.expected);
  if (tok == tok_identifier)
    EXPECT_EQ(lexer.getIdentifier(), param.expectedIdentifier);
  if (tok == tok_number)
    EXPECT_DOUBLE_EQ(lexer.getValue(), param.expectedValue);
}

INSTANTIATE_TEST_SUITE_P(
    Tokens, SingleTokenTest,
    ::testing::Values(
        // Keywords
        TokenCase{"", tok_eof}, TokenCase{"def", tok_def},
        TokenCase{"extern", tok_extern}, TokenCase{"if", tok_if},
        TokenCase{"then", tok_then}, TokenCase{"else", tok_else},
        TokenCase{"for", tok_for}, TokenCase{"in", tok_in},
        TokenCase{"binary", tok_binary}, TokenCase{"unary", tok_unary},
        TokenCase{"var", tok_var},

        // Identifiers (underscores now allowed after the first character)
        TokenCase{"myVar", tok_identifier, "myVar"},
        TokenCase{"x123", tok_identifier, "x123"},
        TokenCase{"my_var_123", tok_identifier, "my_var_123"},

        // Numbers
        TokenCase{"42", tok_number, "", 42.0},
        TokenCase{"123.45", tok_number, "", 123.45},
        TokenCase{"0.001", tok_number, "", 0.001},
        TokenCase{".5", tok_number, "", 0.5},
        TokenCase{".0123", tok_number, "", 0.0123},

        // Single characters
        TokenCase{"+", Token('+')}, TokenCase{"(", tok_paren_open},
        TokenCase{"!", Token('!')}, TokenCase{"|", Token('|')},

        // Comments and whitespace skip to the next token
        TokenCase{"# a comment\n42", tok_number, "", 42.0},
        TokenCase{"   \t\n  def", tok_def}));

TEST(LexerTest, TokenSequence) {
  auto tokens = lexAll("def foo(x y) x + foo(y, 4.0);");
  std::vector<Token> expected = {
      tok_def,        tok_identifier, tok_paren_open, tok_identifier,
      tok_identifier, tok_paren_close, tok_identifier, Token('+'),
      tok_identifier, tok_paren_open, tok_identifier, tok_comma,
      tok_number,     tok_paren_close, tok_semicolon};
  EXPECT_EQ(tokens, expected);
}

TEST(LexerTest, SecondDotTerminatesNumber) {
  // "3.14.15" lexes as the number 3.14 followed by the number 0.15 (the
  // second dot terminates the first literal, and ".15" is a leading-dot
  // number just like ".5") -- nothing is silently swallowed, unlike the
  // upstream lexer which consumed the whole string and kept only 3.14.
  std::string input = "3.14.15";
  LexerBuffer lexer(input.data(), input.data() + input.size(), "test.k");
  ASSERT_EQ(lexer.getNextToken(), tok_number);
  EXPECT_DOUBLE_EQ(lexer.getValue(), 3.14);
  ASSERT_EQ(lexer.getNextToken(), tok_number);
  EXPECT_DOUBLE_EQ(lexer.getValue(), 0.15);
  EXPECT_EQ(lexer.getNextToken(), tok_eof);
}

TEST(LexerTest, ExponentIsNotSupported) {
  // The lexer stops at 'e': "1.5e3" is the number 1.5 followed by the
  // identifier "e3".
  std::string input = "1.5e3";
  LexerBuffer lexer(input.data(), input.data() + input.size(), "test.k");
  ASSERT_EQ(lexer.getNextToken(), tok_number);
  EXPECT_DOUBLE_EQ(lexer.getValue(), 1.5);
  ASSERT_EQ(lexer.getNextToken(), tok_identifier);
  EXPECT_EQ(lexer.getIdentifier(), "e3");
}

TEST(LexerTest, TracksLocations) {
  std::string input = "def foo(x)\n  x + 1;\n";
  LexerBuffer lexer(input.data(), input.data() + input.size(), "test.k");

  lexer.getNextToken(); // def
  EXPECT_EQ(lexer.getLastLocation().line, 1);
  EXPECT_EQ(lexer.getLastLocation().col, 1);

  lexer.getNextToken(); // foo
  EXPECT_EQ(lexer.getLastLocation().line, 1);
  EXPECT_EQ(lexer.getLastLocation().col, 5);

  lexer.getNextToken(); // (
  lexer.getNextToken(); // x
  lexer.getNextToken(); // )
  lexer.getNextToken(); // x on line 2
  EXPECT_EQ(lexer.getLastLocation().line, 2);
  EXPECT_EQ(lexer.getLastLocation().col, 3);
  EXPECT_EQ(*lexer.getLastLocation().file, "test.k");
}

TEST(LexerTest, EofIsIdempotent) {
  std::string input = "x";
  LexerBuffer lexer(input.data(), input.data() + input.size(), "test.k");
  EXPECT_EQ(lexer.getNextToken(), tok_identifier);
  EXPECT_EQ(lexer.getNextToken(), tok_eof);
  EXPECT_EQ(lexer.getNextToken(), tok_eof);
  EXPECT_EQ(lexer.getCurToken(), tok_eof);
}

} // namespace
