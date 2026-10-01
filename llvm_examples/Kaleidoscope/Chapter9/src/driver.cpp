#include <cstdio>
#include <cstdlib>

#include "driver.h"

#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

using namespace toy;

Driver::Driver()
    : parser(lexer),
      // Chapter 9: no optimization (it scrambles the line mapping), debug info
      // on. The source arrives on stdin, so the compile unit is named after the
      // chapter's example, as upstream hardcodes it.
      codegen(CodeGenOptions{/*optimize=*/false, /*emitDebugInfo=*/true, "fib.ks"}) {
  // Set up the host target -- it exists here only to supply a data layout.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto TargetTriple = llvm::sys::getDefaultTargetTriple();
  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
  if (!Target) {
    llvm::errs() << Error;
    std::exit(1);
  }
  llvm::TargetOptions opt;
  theTargetMachine.reset(Target->createTargetMachine(
      TargetTriple, "generic", "", opt, llvm::Reloc::PIC_));

  codegen.setDataLayout(theTargetMachine->createDataLayout());
}

Driver::~Driver() = default;

void Driver::handleDefinition() {
  if (auto FnAST = parser.parseDefinition()) {
    if (!codegen.emitFunction(*FnAST))
      fprintf(stderr, "Error reading function definition:");
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::handleExtern() {
  if (auto ProtoAST = parser.parseExtern()) {
    if (!codegen.emitPrototype(*ProtoAST))
      fprintf(stderr, "Error reading extern");
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::handleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function -- named
  // `main` since Chapter 9, so the dumped module is a runnable program. It
  // stays in the module (nothing erases it), which is why only one top-level
  // expression per program is supported: a second one is a redefinition.
  if (auto FnAST = parser.parseTopLevelExpr()) {
    if (!codegen.emitFunction(*FnAST))
      fprintf(stderr, "Error generating code for top level expr");
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::mainLoop() {
    // Chapter 9: a batch compiler -- no `ready>` prompts.
    parser.getNextToken(); // Bootstrap the first token
    while (true) {
        switch (parser.getCurToken()) {
        case tok_eof: return;
        case ';':     parser.getNextToken(); break;  // ignore top-level semicolons.
        case tok_def: handleDefinition(); break;
        case tok_extern: handleExtern(); break;
        default:      handleTopLevelExpression(); break;
        }
    }
}

int Driver::dumpModule() {
  // Finalize the debug info.
  codegen.finalize();

  // Print out all of the generated code.
  codegen.currentModule().print(llvm::errs(), nullptr);
  return 0;
}
