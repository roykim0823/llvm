#include <cstdio>

#include "driver.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

using namespace toy;

void Driver::handleDefinition() {
  if (auto FnAST = parser.parseDefinition()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read function definition:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
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

      // Remove the anonymous expression.
      codegen.eraseFunction(FnIR);
    }
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::mainLoop() {
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
    // NOTE: unreachable module dump removed -- the loop above only exits via
    // 'return' on tok_eof, so code after it never ran.
}
