/// \file
/// \brief Unit tests for the `tinylang::tok::get*` spelling helpers.
///
/// Each token kind in `TokenKinds.def` is expanded into one fixture row
/// (kind, stringified name, spelling, is-punctuator flag, is-keyword flag).
/// Three `TEST_P` checks per row verify:
///  - `getTokenName`  returns the stringified enumerator,
///  - `getPunctuatorSpelling` returns the spelling for punctuators and
///    `nullptr` otherwise,
///  - `getKeywordSpelling`    returns the spelling for keywords and
///    `nullptr` otherwise.

#include "tinylang/Basic/TokenKinds.h"
#include "llvm/ADT/StringRef.h"
#include "gtest/gtest.h"
#include <vector>

using namespace tinylang;

namespace {

struct TokenTestData {
  tok::TokenKind Kind;
  const char *Name;
  const char *ExpectedSpelling;
  bool IsPunctuator;
  bool IsKeyword;
};

// Helper to build the data set using the .def file
std::vector<TokenTestData> GetTokenTestData() {
  return {
#define TOK(ID) {tok::ID, #ID, nullptr, false, false},
#define PUNCTUATOR(ID, SP) {tok::ID, #ID, SP, true, false},
#define KEYWORD(ID, FLAG) {tok::kw_##ID, #ID, #ID, false, true},
#include "tinylang/Basic/TokenKinds.def"
#undef TOK
#undef PUNCTUATOR
#undef KEYWORD
  };
}

class TokenKindsTest : public ::testing::TestWithParam<TokenTestData> {};

// 1. Test that getTokenName returns the correct stringified ID
TEST_P(TokenKindsTest, TokenNameMatches) {
  const auto &Data = GetParam();
  EXPECT_STREQ(Data.Name, tok::getTokenName(Data.Kind));
}

// 2. Test Punctuator spellings
TEST_P(TokenKindsTest, PunctuatorSpellingMatches) {
  const auto &Data = GetParam();
  const char *Spelling = tok::getPunctuatorSpelling(Data.Kind);

  if (Data.IsPunctuator) {
    ASSERT_NE(nullptr, Spelling);
    EXPECT_STREQ(Data.ExpectedSpelling, Spelling);
  } else {
    EXPECT_EQ(nullptr, Spelling);
  }
}

// 3. Test Keyword spellings
TEST_P(TokenKindsTest, KeywordSpellingMatches) {
  const auto &Data = GetParam();
  const char *Spelling = tok::getKeywordSpelling(Data.Kind);

  if (Data.IsKeyword) {
    ASSERT_NE(nullptr, Spelling);
    EXPECT_STREQ(Data.ExpectedSpelling, Spelling);
  } else {
    EXPECT_EQ(nullptr, Spelling);
  }
}

INSTANTIATE_TEST_SUITE_P(
    AllTokens,
    TokenKindsTest,
    ::testing::ValuesIn(GetTokenTestData()),
    [](const ::testing::TestParamInfo<TokenTestData> &info) {
      return info.param.Name;
    }
);

} // namespace