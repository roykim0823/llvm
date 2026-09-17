//===- Diagnostics.h - Diagnostic engine for the toy compiler -------------===//
//
// One diagnostic channel for every stage (parser, sema, IR generation,
// driver), in the spirit of clang's DiagnosticsEngine and MLIR's
// DiagnosticEngine: a diagnostic is *data* (severity + location + message);
// rendering is separate; and an embedder can install its own handler (the
// unit tests install a silent one and inspect the stored list). Rendering
// follows the clang convention:
//
//   file:line:col: error: message
//
// Because reporting no longer aborts anything, stages can keep going after
// an error and a single run reports every problem it finds.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_DIAGNOSTICS_H
#define TOY_DIAGNOSTICS_H

#include "toy/Lexer.h" // for Location

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace toy {

/// A single diagnostic: pure data, renderable anywhere.
struct Diagnostic {
  enum Severity { Error, Warning, Note };

  Severity severity;
  Location loc;
  std::string message;

  void print(llvm::raw_ostream &os) const {
    if (loc.file)
      os << *loc.file << ":" << loc.line << ":" << loc.col << ": ";
    switch (severity) {
    case Error:
      os << "error: ";
      break;
    case Warning:
      os << "warning: ";
      break;
    case Note:
      os << "note: ";
      break;
    }
    os << message << "\n";
  }
};

/// Collects diagnostics from every stage and forwards each one to a handler
/// as it arrives. The default handler prints to llvm::errs(); tests install
/// a no-op handler and inspect diagnostics() instead. Only Error severity
/// counts toward hadError() -- warnings and notes never fail a compile.
class DiagnosticEngine {
public:
  using Handler = std::function<void(const Diagnostic &)>;

  DiagnosticEngine()
      : handler([](const Diagnostic &d) { d.print(llvm::errs()); }) {}
  explicit DiagnosticEngine(Handler handler) : handler(std::move(handler)) {}

  void report(Diagnostic::Severity severity, Location loc,
              const llvm::Twine &message) {
    diags.push_back({severity, std::move(loc), message.str()});
    if (severity == Diagnostic::Error)
      ++numErrors;
    if (handler)
      handler(diags.back());
  }

  void error(Location loc, const llvm::Twine &message) {
    report(Diagnostic::Error, std::move(loc), message);
  }
  void warning(Location loc, const llvm::Twine &message) {
    report(Diagnostic::Warning, std::move(loc), message);
  }
  /// A note attaches secondary information (e.g. "previously defined here")
  /// to the diagnostic reported immediately before it.
  void note(Location loc, const llvm::Twine &message) {
    report(Diagnostic::Note, std::move(loc), message);
  }

  bool hadError() const { return numErrors != 0; }
  unsigned errorCount() const { return numErrors; }
  llvm::ArrayRef<Diagnostic> diagnostics() const { return diags; }

private:
  Handler handler;
  std::vector<Diagnostic> diags;
  unsigned numErrors = 0;
};

} // namespace toy

#endif // TOY_DIAGNOSTICS_H
