#ifndef DRIVER_H
#define DRIVER_H

#include <memory>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"

namespace llvm {
class TargetMachine;
}

namespace toy {

//===----------------------------------------------------------------------===//
// Top-Level parsing and object-file driver
//===----------------------------------------------------------------------===//
// The driver is the composition root: it owns one instance of every stage
// (lexer -> parser -> codegen -> target machine) and runs the read-eval-print
// loop. Where Chapter 4's driver handed each finished module to a JIT, this
// one keeps the session's single module growing and, once stdin is drained,
// runs the backend over it to write an object file. The code generator is
// unchanged: it still just produces modules and takes a data layout.

class Driver {
public:
    Driver();
    ~Driver();

    /// top ::= definition | external | toplevelexpr | ';'
    void mainLoop();

    /// Run the backend over the accumulated module and write native object
    /// code to `filename`. Returns a process exit code (0 on success).
    int emitObjectFile(const char *filename);

private:
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();

    // Declaration order matters: the parser holds a reference to the lexer,
    // and the constructor configures codegen from the target machine.
    Lexer lexer;
    Parser parser;
    CodeGenSession codegen;
    std::unique_ptr<llvm::TargetMachine> theTargetMachine;
};

} // end namespace toy

#endif // DRIVER_H
