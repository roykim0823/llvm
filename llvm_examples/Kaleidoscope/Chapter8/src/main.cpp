#include "lexer.h"
#include "parser.h"
#include "ir_gen_ctx.h"

// for obj file generation
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//
int compile_obj(toy::IRGenContext& ctx) {
  auto TargetTriple = llvm::sys::getDefaultTargetTriple();
  ctx.theModule->setTargetTriple(TargetTriple);

  std::string Error;
  auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

  // Print an error and exit if we couldn't find the requested target.
  // This generally occurs if we've forgotten to initialise the
  // TargetRegistry or we have a bogus target triple.
  if (!Target) {
    llvm::errs() << Error;
    return 1;
  }

  auto CPU = "generic";
  auto Features = "";

  llvm::TargetOptions opt;
  auto TheTargetMachine = Target->createTargetMachine(
      TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_);


  ctx.theModule->setDataLayout(TheTargetMachine->createDataLayout());

  // Emit the object code to a file.
  auto Filename = "output.o";
  std::error_code EC;
  llvm::raw_fd_ostream dest(Filename, EC, llvm::sys::fs::OF_None);

  if (EC) {
    llvm::errs() << "Could not open file: " << EC.message();
    return 1;
  }

  llvm::legacy::PassManager pass;
  auto FileType = llvm::CodeGenFileType::ObjectFile;

  if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
    llvm::errs() << "TheTargetMachine can't emit a file of this type";
    return 1;
  }

  pass.run(*ctx.theModule);
  dest.flush();

  llvm::outs() << "Wrote " << Filename << "\n";
  return 0;
}

int main() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    toy::Lexer lexer;
    toy::IRGenContext ctx;
    toy::Parser parser(lexer, ctx);

    // Run the main "interpreter loop" now.
    parser.mainLoop();

    return compile_obj(ctx);
}
