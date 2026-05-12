/// \file
/// \brief Diagnostic IDs and the engine that formats and reports them.
///
/// Every distinct error/warning/note message is declared once in
/// `Diagnostic.def`. This header X-macro-expands those entries into the
/// `tinylang::diag` enum and defines \ref tinylang::DiagnosticsEngine.

#ifndef TINYLANG_BASIC_DIAGNOSTIC_H
#define TINYLANG_BASIC_DIAGNOSTIC_H

#include "tinylang/Basic/LLVM.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <utility>

namespace tinylang {

/// Diagnostic identifiers, generated from `Diagnostic.def` via X-macros.
///
/// Each entry has an ID (`err_*`, `warn_*`, `note_*`), a severity level, and
/// a `formatv`-style message string. The numeric values are unstable across
/// edits to `Diagnostic.def` — always reference IDs by name.
namespace diag {
enum {
#define DIAG(ID, Level, Msg) ID,
#include "tinylang/Basic/Diagnostic.def"
};
} // namespace diag

/// Formats and reports diagnostics, tracking the number of errors seen.
///
/// Wraps `llvm::SourceMgr::PrintMessage`: callers pass an `SMLoc`, a
/// `diag::*` ID, and any arguments referenced by the message's `{0}`/`{1}`
/// placeholders. The engine looks up the level (`Error`, `Warning`, `Note`),
/// `llvm::formatv`-formats the message, and prints it with source-line
/// context. Error count is exposed via \ref numErrors so the parser can
/// decide whether to keep going.
class DiagnosticsEngine {
  static const char *getDiagnosticText(unsigned DiagID);
  static SourceMgr::DiagKind getDiagnosticKind(unsigned DiagID);

  SourceMgr &SrcMgr;
  unsigned NumErrors;

public:
  /// \param SrcMgr the source manager that owns the buffers being diagnosed.
  DiagnosticsEngine(SourceMgr &SrcMgr)
      : SrcMgr(SrcMgr), NumErrors(0) {}

  /// Number of `Error`-level diagnostics emitted so far.
  unsigned numErrors() { return NumErrors; }

  /// Emits a diagnostic at \p Loc.
  ///
  /// \param Loc       Source location to anchor the message; pass `SMLoc()`
  ///                  when none is appropriate.
  /// \param DiagID    A `diag::*` identifier.
  /// \param Arguments Values for any `{N}` placeholders in the message.
  template <typename... Args>
  void report(SMLoc Loc, unsigned DiagID,
              Args &&... Arguments) {
    std::string Msg =
        llvm::formatv(getDiagnosticText(DiagID),
                      std::forward<Args>(Arguments)...)
            .str();
    SourceMgr::DiagKind Kind = getDiagnosticKind(DiagID);
    SrcMgr.PrintMessage(Loc, Kind, Msg);
    NumErrors += (Kind == SourceMgr::DK_Error);
  }
};

} // namespace tinylang

#endif
