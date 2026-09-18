#include <cstdio>

#include "driver.h"

using namespace toy;

void Driver::handleDefinition() {
  if (parser.parseDefinition()) {
    fprintf(stderr, "Parsed a function definition.\n");
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::handleExtern() {
  if (parser.parseExtern()) {
    fprintf(stderr, "Parsed an extern\n");
  } else {
    // Skip token for error recovery.
    parser.getNextToken();
  }
}

void Driver::handleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (parser.parseTopLevelExpr()) {
    fprintf(stderr, "Parsed a top-level expr\n");
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
}
