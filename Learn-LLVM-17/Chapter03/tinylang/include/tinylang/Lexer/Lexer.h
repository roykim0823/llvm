/// \file
/// \brief The lexer: turns the source buffer into a stream of \ref tinylang::Token "Token"s.

#ifndef TINYLANG_LEXER_LEXER_H
#define TINYLANG_LEXER_LEXER_H

#include "tinylang/Basic/Diagnostic.h"
#include "tinylang/Basic/LLVM.h"
#include "tinylang/Lexer/Token.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

namespace tinylang {

/// Hash-table of language keywords used to reclassify identifier tokens.
///
/// Populated from `TokenKinds.def` by \ref addKeywords. The \ref Lexer
/// scans an identifier first, then asks this filter whether the spelling is
/// really a keyword.
class KeywordFilter {
  llvm::StringMap<tok::TokenKind> HashTable;

  void addKeyword(StringRef Keyword,
                  tok::TokenKind TokenCode);

public:
  /// Loads every `KEYWORD(...)` entry from `TokenKinds.def` into the table.
  /// Must be called once before \ref getKeyword.
  void addKeywords();

  /// Returns the token kind for \p Name if it is a keyword, otherwise
  /// \p DefaultTokenCode (typically `tok::unknown` — the caller then keeps
  /// the token as an identifier).
  tok::TokenKind getKeyword(
      StringRef Name,
      tok::TokenKind DefaultTokenCode = tok::unknown) {
    auto Result = HashTable.find(Name);
    if (Result != HashTable.end())
      return Result->second;
    return DefaultTokenCode;
  }
};

/// Stateful lexer over an LLVM `SourceMgr` buffer.
///
/// On construction the main file ID is pinned and the scan pointer is set to
/// the buffer start. \ref next is the only entry point the parser uses: it
/// dispatches on the current character to the appropriate helper
/// (`identifier`, `number`, `string`, `comment`) or builds a punctuator
/// directly with \ref formToken. Lexical errors (unterminated comments,
/// unterminated strings, hex digits in a decimal number) are reported via
/// the held \ref DiagnosticsEngine.
class Lexer {
  SourceMgr &SrcMgr;
  DiagnosticsEngine &Diags;

  const char *CurPtr;   ///< Next byte to read.
  StringRef CurBuf;     ///< The full source buffer.

  unsigned CurBuffer = 0; ///< Index of the current buffer in \p SrcMgr.

  KeywordFilter Keywords;

public:
  /// Binds the lexer to \p SrcMgr's main buffer. \p Diags receives any
  /// lexical errors. The keyword table is initialised here.
  Lexer(SourceMgr &SrcMgr, DiagnosticsEngine &Diags)
      : SrcMgr(SrcMgr), Diags(Diags) {
    CurBuffer = SrcMgr.getMainFileID();
    CurBuf = SrcMgr.getMemoryBuffer(CurBuffer)->getBuffer();
    CurPtr = CurBuf.begin();
    Keywords.addKeywords();
  }

  /// Access to the engine receiving lexical diagnostics.
  DiagnosticsEngine &getDiagnostics() const {
    return Diags;
  }

  /// Reads the next token from the input, writing into \p Result.
  ///
  /// On EOF, \p Result has kind `tok::eof`. The function never throws —
  /// errors are reported via the diagnostics engine and the lexer attempts
  /// to resync.
  void next(Token &Result);

  /// The full source buffer this lexer is reading from.
  StringRef getBuffer() const { return CurBuf; }

private:
  void identifier(Token &Result);   ///< Lex an identifier (or keyword).
  void number(Token &Result);       ///< Lex an integer literal (decimal/hex).
  void string(Token &Result);       ///< Lex a `"…"` or `'…'` string literal.
  void comment();                   ///< Skip a possibly-nested `(* … *)` comment.

  /// Current scan position as an `SMLoc` (for diagnostics).
  SMLoc getLoc() { return SMLoc::getFromPointer(CurPtr); }

  /// Finalises \p Result with the given kind, recording the span
  /// `[CurPtr, TokEnd)`. Advances `CurPtr` to \p TokEnd.
  void formToken(Token &Result, const char *TokEnd,
                 tok::TokenKind Kind);
};
} // namespace tinylang
#endif
