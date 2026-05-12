/// \file
/// \brief The `tinylang` Chapter-3 driver — a frontend-only entry point.
///
/// For each `.mod` file on the command line it
///   1. loads the file into a fresh `llvm::SourceMgr`,
///   2. spins up a \ref tinylang::DiagnosticsEngine,
///       \ref tinylang::Lexer, \ref tinylang::Sema, \ref tinylang::Parser,
///   3. calls \ref tinylang::Parser::parse() and discards the AST.
///
/// No code is emitted in Ch03; later chapters add an output stage.

#include "tinylang/Basic/Diagnostic.h"
#include "tinylang/Basic/Version.h"
#include "tinylang/Parser/Parser.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

using namespace tinylang;

int main(int argc_, const char **argv_) {
  llvm::InitLLVM X(argc_, argv_);
  llvm::SmallVector<const char *, 256> argv(argv_ + 1,
                                            argv_ + argc_);

  llvm::outs() << "Tinylang "
               << tinylang::getTinylangVersion() << "\n";

  for (const char *F : argv) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
        FileOrErr = llvm::MemoryBuffer::getFile(F);
    if (std::error_code BufferError =
            FileOrErr.getError()) {
      llvm::errs() << "Error reading " << F << ": "
                   << BufferError.message() << "\n";
      continue;
    }

    llvm::SourceMgr SrcMgr;
    DiagnosticsEngine Diags(SrcMgr);

    // Tell SrcMgr about this buffer, which is what the
    // parser will pick up.
    SrcMgr.AddNewSourceBuffer(std::move(*FileOrErr),
                              llvm::SMLoc());

    auto TheLexer = Lexer(SrcMgr, Diags);
    auto TheSema = Sema(Diags);
    auto TheParser = Parser(TheLexer, TheSema);
    TheParser.parse();
  }
}