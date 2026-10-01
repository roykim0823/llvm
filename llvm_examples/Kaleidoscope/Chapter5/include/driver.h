#ifndef DRIVER_H
#define DRIVER_H

#include <memory>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"

#include "../../include/KaleidoscopeJIT.h"

namespace toy {

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT driver
//===----------------------------------------------------------------------===//
// The driver is the composition root: it owns one instance of every stage
// (lexer -> parser -> codegen -> JIT) and runs the read-eval-print loop. It
// is the only component that knows the JIT exists: the code generator hands
// finished modules out through takeModule() and the driver decides what
// happens to them -- kept resident (definitions) or run once and freed
// (top-level expressions).

class Driver {
public:
    Driver();

    /// top ::= definition | external | toplevelexpr | ';'
    void mainLoop();

private:
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();

    // Declaration order matters: the parser holds a reference to the lexer,
    // and the constructor configures codegen from the JIT.
    Lexer lexer;
    Parser parser;
    CodeGenSession codegen;
    llvm::ExitOnError ExitOnErr;
    std::unique_ptr<llvm::orc::KaleidoscopeJIT> theJIT;
};

} // end namespace toy

#endif // DRIVER_H
