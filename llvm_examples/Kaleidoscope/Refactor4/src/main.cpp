#include "lexer.h"
#include "parser.h"
#include "codegen_ctx.h"

int main() {
    // To support JIT
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    toy::Lexer lexer;
    toy::CodegenContext ctx;
    toy::Parser parser(lexer, ctx);
    
    // Run the main "interpreter loop" now.
    parser.mainLoop();

    return 0;
}