/// \file
/// \brief The token kind enumeration and spelling lookups.
///
/// `tok::TokenKind` is generated from `TokenKinds.def` via X-macros, so
/// adding a new token or keyword is a single-line change in the `.def` file.

#ifndef TINYLANG_BASIC_TOKENKINDS_H
#define TINYLANG_BASIC_TOKENKINDS_H

#include "llvm/Support/Compiler.h"

namespace tinylang {

namespace tok {

/// Every token kind tinylang recognises.
///
/// The enumerators are produced by including `TokenKinds.def`, which lists:
///   - `TOK(name)`            — synthetic kinds (`unknown`, `eof`, `identifier`, literals)
///   - `PUNCTUATOR(name, sp)` — punctuators (their spelling is in `sp`)
///   - `KEYWORD(name, flags)` — language keywords (enumerator is `kw_NAME`)
///
/// `NUM_TOKENS` is a sentinel one past the last real kind.
enum TokenKind : unsigned short {
#define TOK(ID) ID,
#include "TokenKinds.def"
  NUM_TOKENS
};

/// Returns the enumerator name as a C string (e.g. `tok::plus` → `"plus"`).
/// Used in diagnostics and the gtest pretty-printer.
const char *getTokenName(TokenKind Kind) LLVM_READNONE;

/// Returns the source spelling of a punctuator token, or `nullptr` if
/// \p Kind is not a punctuator (e.g. for identifiers and literals).
const char *getPunctuatorSpelling(TokenKind Kind) LLVM_READNONE;

/// Returns the source spelling of a keyword token (e.g. `tok::kw_MODULE` →
/// `"MODULE"`), or `nullptr` if \p Kind is not a keyword.
const char *getKeywordSpelling(TokenKind Kind) LLVM_READNONE;

} // namespace tok
} // namespace tinylang

#endif
