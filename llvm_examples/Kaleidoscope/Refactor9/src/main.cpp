#include "lexer.h"
#include "parser.h"
#include "ir_gen_ctx.h"
#include "debug.h"

// for obj file generation
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

// Debug Info
extern std::unique_ptr<llvm::DIBuilder> DBuilder;
extern toy::DebugInfo KSDbgInfo;

int main() {
    toy::IRGenContext ctx;
    toy::DebugInfoManager debugMgr(ctx);
    toy::Lexer lexer(debugMgr);
    toy::Parser parser(lexer, ctx);

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

    // Run the main "interpreter loop" now.
    parser.mainLoop();

    // Finalize the debug info.
    DBuilder->finalize();

    // Print out all of the generated code.
    ctx.theModule->print(llvm::errs(), nullptr);
    return 0;
}