#include <cstdio>
#include <cstdlib>

#include "driver.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

using namespace toy;

Driver::Driver() : parser(lexer) {
  // Set up the host target (used by emitObjectFile's object emission).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  // Pick the machine we are running on, and look its backend up in the
  // TargetRegistry.
  auto TargetTriple = llvm::sys::getDefaultTargetTriple();

  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

  // Print an error and exit if we couldn't find the requested target.
  // This generally occurs if we've forgotten to initialise the
  // TargetRegistry or we have a bogus target triple.
  if (!Target) {
    llvm::errs() << Error;
    std::exit(1);
  }

  auto CPU = "generic";
  auto Features = "";

  llvm::TargetOptions opt;
  theTargetMachine.reset(Target->createTargetMachine(
      TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_));

  // Configure the module we are about to fill for this machine, up front, so
  // the optimizer works with the right sizes and alignments.
  codegen.setDataLayout(theTargetMachine->createDataLayout());
  codegen.currentModule().setTargetTriple(TargetTriple);
}

Driver::~Driver() = default;

void Driver::handleDefinition() {
  if (auto FnAST = parser.parseDefinition()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read function definition:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
      // No JIT to hand the module to: the definition stays in the session's
      // one module, which emitObjectFile() writes out at EOF.
    }
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::handleExtern() {
  if (auto ProtoAST = parser.parseExtern()) {
    if (auto *FnIR = codegen.emitPrototype(*ProtoAST)) {
      fprintf(stderr, "Read extern:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
    }
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::handleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = parser.parseTopLevelExpr()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read top-level expression:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      // Remove the anonymous expression. There is no JIT to run it, an object
      // file has no use for it, and leaving it would make the next top-level
      // expression a redefinition of __anon_expr. Through the session, so the
      // analyses cached for it go too.
      codegen.eraseFunction(FnIR);
    }
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::mainLoop() {
    fprintf(stderr, "ready> ");
    parser.getNextToken(); // Bootstrap the first token
    while (true) {
        fprintf(stderr, "ready> ");
        switch (parser.getCurToken()) {
        case tok_eof: return;
        case ';':     parser.getNextToken(); break;  // ignore top-level semicolons.
        case tok_def: handleDefinition(); break;
        case tok_extern: handleExtern(); break;
        default:      handleTopLevelExpression(); break;
        }
    }
}

int Driver::emitObjectFile(const char *Filename) {
  // Emit the object code to a file.
  std::error_code EC;
  llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

  if (EC) {
    llvm::errs() << "Could not open file: " << EC.message();
    return 1;
  }

  // Code generation still runs on the legacy pass manager: addPassesToEmitFile
  // fills it with the whole backend (isel, regalloc, scheduling, emission).
  llvm::legacy::PassManager pass;
  auto FileType = llvm::CodeGenFileType::ObjectFile;

  if (theTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
    llvm::errs() << "TheTargetMachine can't emit a file of this type";
    return 1;
  }

  pass.run(codegen.currentModule());
  dest.flush();

  llvm::outs() << "Wrote " << Filename << "\n";
  return 0;
}
