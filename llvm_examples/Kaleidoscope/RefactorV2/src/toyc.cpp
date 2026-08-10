//===- toyc.cpp - The Kaleidoscope Compiler Driver ------------------------===//
//
// Staged driver in the style of the MLIR Toy tutorial's toyc: one binary,
// with the pipeline stage selected by -emit. The enum ordering is
// load-bearing: "emit stage N" means "run everything up to N".
//
//   toyc file.k -emit=ast        dump the AST and stop
//   toyc file.k -emit=ir [-opt]  print LLVM IR (optionally optimized)
//   toyc file.k -emit=obj -o f.o compile to an object file
//   toyc file.k -emit=jit        execute top-level expressions
//
// The input defaults to stdin ("-"), so `toyc < file.k -emit=ir` also works.
//
//===----------------------------------------------------------------------===//

#include "toy/AST.h"
#include "toy/CodeGen.h"
#include "toy/Lexer.h"
#include "toy/Parser.h"

#include "../../include/KaleidoscopeJIT.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

using namespace toy;
namespace cl = llvm::cl;

static cl::opt<std::string> inputFilename(cl::Positional,
                                          cl::desc("<input kaleidoscope file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

namespace {
enum Action { None, DumpAST, DumpIR, EmitObj, RunJIT };
} // namespace

static cl::opt<enum Action> emitAction(
    "emit", cl::desc("Select the kind of output desired"),
    cl::values(clEnumValN(DumpAST, "ast", "output the AST dump"),
               clEnumValN(DumpIR, "ir", "output the LLVM IR"),
               clEnumValN(EmitObj, "obj", "compile to an object file"),
               clEnumValN(RunJIT, "jit",
                          "JIT-execute the top-level expressions")));

static cl::opt<bool> enableOpt("opt", cl::desc("Enable optimizations"));

static cl::opt<bool> emitDebugInfo("g", cl::desc("Emit debug information"));

static cl::opt<std::string> outputFilename("o", cl::desc("Output object file"),
                                           cl::init("output.o"),
                                           cl::value_desc("filename"));

/// Read the input file (or stdin) and parse it into a ModuleAST.
/// The MemoryBuffer must outlive the lexer, which must outlive the parser --
/// all handled by declaration order in this frame; the returned AST is
/// self-contained.
static std::unique_ptr<ModuleAST> parseInputFile(llvm::StringRef filename) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileOrErr =
      llvm::MemoryBuffer::getFileOrSTDIN(filename);
  if (std::error_code ec = fileOrErr.getError()) {
    llvm::errs() << "Could not open input file: " << ec.message() << "\n";
    return nullptr;
  }
  auto buffer = fileOrErr.get()->getBuffer();
  LexerBuffer lexer(buffer.begin(), buffer.end(), std::string(filename));
  Parser parser(lexer);
  return parser.parseModule();
}

/// Generate all records into one module and hand back the session,
/// finalized and verified. Returns false on any codegen error.
static bool emitWholeModule(CodeGenSession &session, ModuleAST &moduleAST) {
  for (auto &record : moduleAST)
    if (!session.emitRecord(*record))
      return false;
  return session.finalize();
}

static int dumpLLVMIR(ModuleAST &moduleAST) {
  CodeGenSession session(
      {enableOpt, emitDebugInfo, std::string(inputFilename)});
  if (!emitWholeModule(session, moduleAST))
    return 1;
  session.currentModule().print(llvm::outs(), nullptr);
  return 0;
}

static int emitObjectFile(ModuleAST &moduleAST) {
  auto targetTriple = llvm::sys::getDefaultTargetTriple();

  std::string error;
  const llvm::Target *target =
      llvm::TargetRegistry::lookupTarget(targetTriple, error);
  if (!target) {
    llvm::errs() << error << "\n";
    return 1;
  }

  llvm::TargetOptions opt;
  std::unique_ptr<llvm::TargetMachine> targetMachine(
      target->createTargetMachine(targetTriple, /*CPU=*/"generic",
                                  /*Features=*/"", opt, llvm::Reloc::PIC_));

  CodeGenSession session(
      {enableOpt, emitDebugInfo, std::string(inputFilename)});
  session.setDataLayout(targetMachine->createDataLayout());
  if (!emitWholeModule(session, moduleAST))
    return 1;

  llvm::Module &module = session.currentModule();
  module.setTargetTriple(targetTriple);

  std::error_code ec;
  llvm::raw_fd_ostream dest(outputFilename, ec, llvm::sys::fs::OF_None);
  if (ec) {
    llvm::errs() << "Could not open file: " << ec.message() << "\n";
    return 1;
  }

  llvm::legacy::PassManager pass;
  if (targetMachine->addPassesToEmitFile(pass, dest, nullptr,
                                         llvm::CodeGenFileType::ObjectFile)) {
    llvm::errs() << "The target machine can't emit an object file\n";
    return 1;
  }
  pass.run(module);
  dest.flush();

  llvm::outs() << "Wrote " << outputFilename << "\n";
  return 0;
}

/// JIT mode: walk the records in source order. Function definitions and
/// top-level expressions each get their own module (so they can be replaced
/// and freed independently); top-level expressions are executed immediately
/// and print their value.
static int runJit(ModuleAST &moduleAST) {
  auto jitOrErr = llvm::orc::KaleidoscopeJIT::Create();
  if (!jitOrErr) {
    llvm::errs() << "Failed to create JIT: "
                 << llvm::toString(jitOrErr.takeError()) << "\n";
    return 1;
  }
  auto jit = std::move(*jitOrErr);

  CodeGenSession session(
      {enableOpt, /*emitDebugInfo=*/false, std::string(inputFilename)});
  session.setDataLayout(jit->getDataLayout());

  auto reportErr = [](llvm::Error err) {
    llvm::errs() << "JIT error: " << llvm::toString(std::move(err)) << "\n";
    return 1;
  };

  for (auto &record : moduleAST) {
    llvm::Function *ir = session.emitRecord(*record);
    if (!ir)
      return 1;

    // Externs only register a prototype; nothing to add to the JIT.
    auto *func = llvm::dyn_cast<FunctionAST>(record.get());
    if (!func)
      continue;

    std::string name = ir->getName().str();
    if (!func->isTopLevelExpr()) {
      // A definition: move its module into the JIT and start a fresh one.
      if (auto err = jit->addModule(session.takeModule()))
        return reportErr(std::move(err));
      continue;
    }

    // A top-level expression: add, run, print, and free its module.
    auto rt = jit->getMainJITDylib().createResourceTracker();
    if (auto err = jit->addModule(session.takeModule(), rt))
      return reportErr(std::move(err));

    auto symOrErr = jit->lookup(name);
    if (!symOrErr)
      return reportErr(symOrErr.takeError());

    double (*fp)() = symOrErr->getAddress().toPtr<double (*)()>();
    llvm::outs() << "Evaluated to " << llvm::format("%f", fp()) << "\n";

    if (auto err = rt->remove())
      return reportErr(std::move(err));
  }
  return 0;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "kaleidoscope compiler\n");

  auto moduleAST = parseInputFile(inputFilename);
  if (!moduleAST)
    return 1;

  if (emitAction == Action::DumpAST) {
    dump(*moduleAST);
    return 0;
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  switch (emitAction) {
  case Action::DumpIR:
    return dumpLLVMIR(*moduleAST);
  case Action::EmitObj:
    return emitObjectFile(*moduleAST);
  case Action::RunJIT:
    return runJit(*moduleAST);
  default:
    llvm::errs() << "No action specified (parsing only?), use -emit=<action>\n";
    return 1;
  }
}
