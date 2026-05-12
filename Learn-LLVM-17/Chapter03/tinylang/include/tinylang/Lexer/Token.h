/// \file
/// \brief The \ref tinylang::Token type produced by the lexer.

#ifndef TINYLANG_LEXER_TOKEN_H
#define TINYLANG_LEXER_TOKEN_H

#include "tinylang/Basic/LLVM.h"
#include "tinylang/Basic/TokenKinds.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"

namespace tinylang {

class Lexer;

/// A lexer token: a pointer + length view into the source buffer plus a
/// `tok::TokenKind`.
///
/// Tokens are *not* self-contained — they only live as long as the
/// underlying `SourceMgr` buffer. The only producer is \ref Lexer (declared a
/// friend so the fields can stay private to outside code).
class Token {
  friend class Lexer;

  const char *Ptr;     ///< Start of the token spelling in the source buffer.
  size_t Length;       ///< Length of the spelling in bytes.
  tok::TokenKind Kind; ///< The flavour of this token.

public:
  /// Returns the token kind.
  tok::TokenKind getKind() const { return Kind; }
  /// Overrides the kind (used during keyword reclassification in the lexer).
  void setKind(tok::TokenKind K) { Kind = K; }

  /// True if this token's kind matches \p K.
  bool is(tok::TokenKind K) const { return Kind == K; }
  /// True if this token's kind does *not* match \p K.
  bool isNot(tok::TokenKind K) const { return Kind != K; }
  /// True if this token matches any of the supplied kinds.
  template <typename... Tokens>
  bool isOneOf(Tokens &&... Toks) const {
    return (... || is(Toks));
  }

  /// Stringified kind name (`"plus"`, `"kw_MODULE"`, …) — useful in diagnostics.
  const char *getName() const {
    return tok::getTokenName(Kind);
  }

  /// Source location of the first byte of the token's spelling.
  SMLoc getLocation() const {
    return SMLoc::getFromPointer(Ptr);
  }
  /// Length of the spelling in bytes.
  size_t getLength() const { return Length; }

  /// Spelling for an identifier token. Asserts the kind is `tok::identifier`.
  StringRef getIdentifier() {
    assert(is(tok::identifier) &&
           "Cannot get identfier of non-identifier");
    return StringRef(Ptr, Length);
  }

  /// Spelling for a literal token (integer or string).
  /// Asserts the kind is `tok::integer_literal` or `tok::string_literal`.
  StringRef getLiteralData() {
    assert(isOneOf(tok::integer_literal,
                   tok::string_literal) &&
           "Cannot get literal data of non-literal");
    return StringRef(Ptr, Length);
  }
};

} // namespace tinylang
#endif
