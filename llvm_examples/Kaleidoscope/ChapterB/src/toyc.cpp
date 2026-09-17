//===- toyc.cpp - The Kaleidoscope Compiler Driver ------------------------===//
//
// Staged driver in the style of the MLIR Toy tutorial's toyc: one binary,
// with the pipeline stage selected by -emit. The enum ordering is
// load-bearing: "emit stage N" means "run everything up to N".
//
//   toyc file.k -emit=ast        dump the AST and stop (parse only, no sema)
//   toyc file.k -emit=ir [-opt]  print LLVM IR (optionally optimized)
//   toyc file.k -emit=obj -o f.o compile to an object file
//   toyc file.k -emit=jit        execute top-level expressions
//
// The input defaults to stdin ("-"), so `toyc < file.k -emit=ir` also works.
//
// Changes from ChapterA: one DiagnosticEngine is threaded through every
// stage; the parser recovers and reports all parse errors; sema runs as its
// own gate between parsing and the backend, reports all semantic errors,
// and produces the name Resolutions the backend consumes; any failing stage
// ends with a clang-style "N errors generated." summary.
//
//===----------------------------------------------------------------------===//

#include "toy/AST.h"
#include "toy/CodeGen.h"
#include "toy/Diagnostics.h"
#include "toy/Lexer.h"
#include "toy/Parser.h"
#include "toy/Sema.h"

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

/// Print the clang-style failure summary and return the failing exit code:
/// callers `return reportErrors(diags);`.
static int reportErrors(const DiagnosticEngine &diags) {
  unsigned n = diags.errorCount();
  llvm::errs() << n << (n == 1 ? " error" : " errors") << " generated.\n";
  return 1;
}

/// Read the input file (or stdin) and parse it into a ModuleAST.
/// The MemoryBuffer must outlive the lexer, which must outlive the parser --
/// all handled by declaration order in this frame; the returned AST is
/// self-contained. Returns null only if the input cannot be read; parse
/// errors are reported through `diags` and yield a partial module.
static std::unique_ptr<ModuleAST> parseInputFile(llvm::StringRef filename,
                                                 DiagnosticEngine &diags) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileOrErr =
      llvm::MemoryBuffer::getFileOrSTDIN(filename);
  if (std::error_code ec = fileOrErr.getError()) {
    llvm::errs() << "Could not open input file: " << ec.message() << "\n";
    return nullptr;
  }
  auto buffer = fileOrErr.get()->getBuffer();
  LexerBuffer lexer(buffer.begin(), buffer.end(), std::string(filename));
  Parser parser(lexer, diags);
  return parser.parseModule();
}

/// Generate all records into one module and finalize (debug info +
/// verifier). Returns false on any codegen error.
static bool emitWholeModule(CodeGenSession &session, ModuleAST &moduleAST) {
  for (auto &record : moduleAST)
    if (!session.emitRecord(*record))
      return false;
  return session.finalize();
}

static int dumpLLVMIR(ModuleAST &moduleAST, const Resolutions &resolutions,
                      DiagnosticEngine &diags) {
  CodeGenSession session({enableOpt, emitDebugInfo, std::string(inputFilename)},
                         resolutions, diags);
  if (!emitWholeModule(session, moduleAST))
    return reportErrors(diags);
  session.currentModule().print(llvm::outs(), nullptr);
  return 0;
}

static int emitObjectFile(ModuleAST &moduleAST, const Resolutions &resolutions,
                          DiagnosticEngine &diags) {
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

  CodeGenSession session({enableOpt, emitDebugInfo, std::string(inputFilename)},
                         resolutions, diags);
  session.setDataLayout(targetMachine->createDataLayout());
  if (!emitWholeModule(session, moduleAST))
    return reportErrors(diags);

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
static int runJit(ModuleAST &moduleAST, const Resolutions &resolutions,
                  DiagnosticEngine &diags) {
  auto jitOrErr = llvm::orc::KaleidoscopeJIT::Create();
  if (!jitOrErr) {
    llvm::errs() << "Failed to create JIT: "
                 << llvm::toString(jitOrErr.takeError()) << "\n";
    return 1;
  }
  auto jit = std::move(*jitOrErr);

  if (emitDebugInfo)
    llvm::errs() << "warning: -g is ignored with -emit=jit\n";

  CodeGenSession session(
      {enableOpt, /*emitDebugInfo=*/false, std::string(inputFilename)},
      resolutions, diags);
  session.setDataLayout(jit->getDataLayout());

  auto reportErr = [](llvm::Error err) {
    llvm::errs() << "JIT error: " << llvm::toString(std::move(err)) << "\n";
    return 1;
  };

  for (auto &record : moduleAST) {
    llvm::Function *ir = session.emitRecord(*record);
    if (!ir)
      return reportErrors(diags);

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

  DiagnosticEngine diags;

  auto moduleAST = parseInputFile(inputFilename, diags);
  if (!moduleAST)
    return 1;
  if (diags.hadError())
    return reportErrors(diags);

  // The AST dump shows the parse tree, so it runs before sema (undeclared
  // names dump fine) -- the frontend still stops here with no LLVM target
  // machinery touched.
  if (emitAction == Action::DumpAST) {
    dump(*moduleAST);
    return 0;
  }

  // Semantic analysis gates every backend stage (all errors in one run)
  // and produces the name resolution the backend consumes.
  Resolutions resolutions = resolveModule(*moduleAST, diags);
  if (diags.hadError())
    return reportErrors(diags);

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  switch (emitAction) {
  case Action::DumpIR:
    return dumpLLVMIR(*moduleAST, resolutions, diags);
  case Action::EmitObj:
    return emitObjectFile(*moduleAST, resolutions, diags);
  case Action::RunJIT:
    return runJit(*moduleAST, resolutions, diags);
  default:
    llvm::errs() << "No action specified (parsing only?), use -emit=<action>\n";
    return 1;
  }
}
