
#include "llvm/TargetParser/Host.h"
#include "lexer.h"
#include "parser.h"
#include "codegen_ctx.h"
#include "debug.h"

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

extern std::unique_ptr<llvm::DIBuilder> DBuilder;
extern toy::DebugInfo KSDbgInfo;

int main() {
    // To support JIT
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    toy::CodegenContext ctx;
    toy::DebugInfoManager debugMgr(ctx);
    toy::Lexer lexer(debugMgr);
    // Initialize the JIT in main
    ctx.TheJIT = ctx.ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
    toy::Parser parser(lexer, ctx);

    parser.getNextToken(); // Bootstrap the first token

    ctx.InitializeModuleAndPassManager();
    // Run the main "interpreter loop" now.

    // Add the current debug info version into the module.
  ctx.theModule->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);

  // Darwin only supports dwarf2.
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
    ctx.theModule->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

  // Construct the DIBuilder, we do this here because we need the module.
  DBuilder = std::make_unique<llvm::DIBuilder>(*ctx.theModule);

  // Create the compile unit for the module.
  // Currently down as "fib.ks" as a filename since we're redirecting stdin
  // but we'd like actual source locations.
  KSDbgInfo.TheCU = DBuilder->createCompileUnit(
      llvm::dwarf::DW_LANG_C, DBuilder->createFile("fib.ks", "."),
      "Kaleidoscope Compiler", false, "", 0);

    parser.mainLoop();

    return 0;
}