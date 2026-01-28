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
    // Initialize the JIT in main
    ctx.TheJIT = ctx.ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
    toy::Parser parser(lexer, ctx);

    ctx.InitializeModuleAndPassManager();
    // Run the main "interpreter loop" now.
    parser.mainLoop();

    return 0;
}