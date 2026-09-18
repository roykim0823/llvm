#ifndef DRIVER_H
#define DRIVER_H

#include "lexer.h"
#include "parser.h"

namespace toy {

//===----------------------------------------------------------------------===//
// Top-Level parsing and driver
//===----------------------------------------------------------------------===//
// The driver is the composition root: it owns one instance of every stage
// (lexer -> parser, for now) and runs the read-eval-print loop that feeds
// each top-level construct to the next stage. At this point the "next stage"
// is only a report of what was parsed; Chapter 3 hands the AST to a code
// generator instead. The parser knows nothing about any of this -- it only
// produces AST.

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
};

} // end namespace toy

#endif // DRIVER_H
