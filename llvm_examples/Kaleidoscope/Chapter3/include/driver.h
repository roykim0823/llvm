#ifndef DRIVER_H
#define DRIVER_H

#include "codegen.h"
#include "lexer.h"
#include "parser.h"

namespace toy {

//===----------------------------------------------------------------------===//
// Top-Level parsing and driver
//===----------------------------------------------------------------------===//
// The driver is the composition root: it owns one instance of every stage
// (lexer -> parser -> codegen) and runs the read-eval-print loop that feeds
// each parsed top-level construct to code generation. Neither the parser nor
// the code generator knows about the other.

class Driver {
public:
    Driver() : parser(lexer) {}

    /// top ::= definition | external | toplevelexpr | ';'
    void mainLoop();

private:
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();

    // Declaration order matters: the parser holds a reference to the lexer.
    Lexer lexer;
    Parser parser;
    CodeGenSession codegen;
};

} // end namespace toy

#endif // DRIVER_H
