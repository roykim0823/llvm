/// \file
/// \brief Implementation of \ref tinylang::DiagnosticsEngine helpers.
///
/// The two tables (text and severity) are built by including
/// `Diagnostic.def` twice with different `DIAG` macros, then indexed by the
/// `diag::*` ID at runtime.

#include "tinylang/Basic/Diagnostic.h"

using namespace tinylang;

namespace {
const char *DiagnosticText[] = {
#define DIAG(ID, Level, Msg) Msg,
#include "tinylang/Basic/Diagnostic.def"
};

SourceMgr::DiagKind DiagnosticKind[] = {
#define DIAG(ID, Level, Msg) SourceMgr::DK_##Level,  // ## is the token-pasting operator: DK_+Level
#include "tinylang/Basic/Diagnostic.def"
};
/* After the preprocessing
// Second expansion: Extracting the Levels
// (Note: DK_##Level expands to DK_Error, DK_Warning, etc.)
SourceMgr::DiagKind DiagnosticKind[] = {
  SourceMgr::DK_Error,   // err_unterminated_block_comment
  SourceMgr::DK_Error,   // err_unterminated_char_or_string
  SourceMgr::DK_Error,   // err_hex_digit_in_decimal
  SourceMgr::DK_Error,   // err_expected
  SourceMgr::DK_Error,   // err_module_identifier_not_equal
  SourceMgr::DK_Note,    // note_module_identifier_declaration
  // ... all other levels from Diagnostic.def
};
*/
} // namespace

const char *
DiagnosticsEngine::getDiagnosticText(unsigned DiagID) {
  return DiagnosticText[DiagID];
}

SourceMgr::DiagKind
DiagnosticsEngine::getDiagnosticKind(unsigned DiagID) {
  return DiagnosticKind[DiagID];
}