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
// Top-Level parsing and batch-compiler driver
//===----------------------------------------------------------------------===//
// The driver is the composition root: it owns one instance of every stage and
// runs the loop over the input. Chapter 9 turns it into a plain ahead-of-time
// batch compiler: no prompts, no per-definition printing, no JIT; the whole
// program accumulates in the session's one module -- generated unoptimized
// and with debug info -- and is printed once at EOF as LLVM assembly that
// clang can turn into a debuggable executable.

class Driver {
public:
    Driver();
    ~Driver();

    /// top ::= definition | external | toplevelexpr | ';'
    void mainLoop();

    /// Complete the module (debug metadata) and print it to stderr as LLVM
    /// assembly. Returns a process exit code.
    int dumpModule();

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
