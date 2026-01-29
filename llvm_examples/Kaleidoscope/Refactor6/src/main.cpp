#include "lexer.h"
#include "parser.h"
#include "codegen_ctx.h"

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

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