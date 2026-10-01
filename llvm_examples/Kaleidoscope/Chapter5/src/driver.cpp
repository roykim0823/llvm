#include <cstdio>

#include "driver.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;

Driver::Driver() : parser(lexer) {
  // Set up the host target so the JIT can emit native code.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  // Initialize the JIT.  This takes ownership of the process control object, and will clean it up on destruction.
  theJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());

  // Every module the session opens must carry the JIT's data layout.
  codegen.setDataLayout(theJIT->getDataLayout());
}

void Driver::handleDefinition() {
  if (auto FnAST = parser.parseDefinition()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read function definition:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      // To Support JIT: hand the finished module to the JIT (it stays resident so
      // later expressions can call the function); the session opens a fresh one.
      ExitOnErr(theJIT->addModule(codegen.takeModule()));
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

      // JIT implementation
      //---------------------------------------------------------------------
      // Create a ResourceTracker to track JIT'd memory allocated to our
      // anonymous expression -- that way we can free it after executing.
      auto RT = theJIT->getMainJITDylib().createResourceTracker();

      // Hand the module to the JIT under the tracker; the session opens a fresh one.
      ExitOnErr(theJIT->addModule(codegen.takeModule(), RT));

      // Search the JIT for the __anon_expr symbol.
      auto ExprSymbol = ExitOnErr(theJIT->lookup("__anon_expr"));

      // Get the symbol's address and cast it to the right type (takes no
      // arguments, returns a double) so we can call it as a native function.
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
      fprintf(stderr, "Evaluated to %f\n", FP());

      // Delete the anonymous expression module from the JIT.
      ExitOnErr(RT->remove());
      //---------------------------------------------------------------------
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
