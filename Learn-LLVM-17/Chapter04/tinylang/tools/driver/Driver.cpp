#include "tinylang/Basic/Diagnostic.h"
#include "tinylang/Basic/Version.h"
#include "tinylang/CodeGen/CodeGenerator.h"
#include "tinylang/Parser/Parser.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/TargetParser/Host.h"

#if __clang_major__ > 17
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#endif

using namespace llvm;
using namespace tinylang;

static codegen::RegisterCodeGenFlags CGF;

static llvm::cl::opt<std::string>
    InputFile(llvm::cl::Positional,
              llvm::cl::desc("<input-files>"),
              cl::init("-"));

static llvm::cl::opt<std::string>
    OutputFilename("o",
                   llvm::cl::desc("Output filename"),
                   llvm::cl::value_desc("filename"));

static llvm::cl::opt<std::string> MTriple(
    "mtriple",
    llvm::cl::desc("Override target triple for module"));

static llvm::cl::opt<bool> EmitLLVM(
    "emit-llvm",
    llvm::cl::desc("Emit IR code instead of assembler"),
    llvm::cl::init(false));

static const char *Head = "tinylang - Tinylang compiler";

void printVersion(llvm::raw_ostream &OS) {
  OS << Head << " " << getTinylangVersion() << "\n";
  OS << "  Default target: "
     << llvm::sys::getDefaultTargetTriple() << "\n";
  std::string CPU(llvm::sys::getHostCPUName());
  OS << "  Host CPU: " << CPU << "\n";
  OS << "\n";
  OS.flush();
  llvm::TargetRegistry::printRegisteredTargetsForVersion(
      OS);
  exit(EXIT_SUCCESS);
}

llvm::TargetMachine *
createTargetMachine(const char *Argv0) {
  llvm::Triple Triple = llvm::Triple(
      !MTriple.empty()
          ? llvm::Triple::normalize(MTriple)
          : llvm::sys::getDefaultTargetTriple());

  // collect all the target info from the cmd line.
  llvm::TargetOptions TargetOptions =
      codegen::InitTargetOptionsFromCodeGenFlags(Triple);
  std::string CPUStr = codegen::getCPUStr();
  std::string FeatureStr = codegen::getFeaturesStr();

  std::string Error;
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(
          codegen::getMArch(), Triple, Error);

  if (!Target) {
    llvm::WithColor::error(llvm::errs(), Argv0) << Error;
    return nullptr;
  }

  llvm::TargetMachine *TM = Target->createTargetMachine(
      Triple.getTriple(), CPUStr, FeatureStr, TargetOptions,
      std::optional<llvm::Reloc::Model>(
          codegen::getRelocModel()));
  return TM;
}

bool emit(StringRef Argv0, llvm::Module *M,
          llvm::TargetMachine *TM,
          StringRef InputFilename) {
  CodeGenFileType FileType = codegen::getFileType();
  if (OutputFilename.empty()) {
    if (InputFilename == "-") {
      OutputFilename = "-";
    } else {
#if __clang_major__ <= 17
      if (InputFilename.endswith(".mod"))
#else
      if (InputFilename.ends_with(".mod"))
#endif
        OutputFilename =
            InputFilename.drop_back(4).str();
      else
        OutputFilename = InputFilename.str();
      switch (FileType) {
#if __clang_major__ <= 17
      case CGFT_AssemblyFile:
#else
      case llvm::CodeGenFileType::AssemblyFile:
#endif
        OutputFilename.append(EmitLLVM ? ".ll" : ".s");
        break;
#if __clang_major__ <= 17
      case CGFT_ObjectFile:
#else
      case llvm::CodeGenFileType::ObjectFile:
#endif
        OutputFilename.append(".o");
        break;
#if __clang_major__ <= 17
      case CGFT_Null:
#else
      case llvm::CodeGenFileType::Null:
#endif
        OutputFilename.append(".null");
        break;
      }
    }
  }

  // Open the file.
  std::error_code EC;
  sys::fs::OpenFlags OpenFlags = sys::fs::OF_None;
#if __clang_major__ <= 17
  if (FileType == CGFT_AssemblyFile)
#else
  if (FileType ==llvm::CodeGenFileType::AssemblyFile)
#endif
    OpenFlags |= sys::fs::OF_TextWithCRLF;
  auto Out = std::make_unique<llvm::ToolOutputFile>(
      OutputFilename, EC, OpenFlags);
  if (EC) {
    WithColor::error(llvm::errs(), Argv0)
        << EC.message() << '\n';
    return false;
  }

  legacy::PassManager PM;
#if __clang_major__ <= 17
  if (FileType == CGFT_AssemblyFile && EmitLLVM) {
#else
  if (FileType == llvm::CodeGenFileType::AssemblyFile && EmitLLVM) {
#endif
    PM.add(createPrintModulePass(Out->os()));
  } else {
    if (TM->addPassesToEmitFile(PM, Out->os(), nullptr,
                                FileType)) {
      WithColor::error(llvm::errs(), Argv0)
          << "No support for file type\n";
      return false;
    }
  }
  PM.run(*M);
  Out->keep();
  return true;
}

int main(int Argc, const char **Argv) {
  llvm::InitLLVM X(Argc, Argv);

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  llvm::cl::SetVersionPrinter(&printVersion);
  llvm::cl::ParseCommandLineOptions(Argc, Argv, Head);

  if (codegen::getMCPU() == "help" ||
      std::any_of(codegen::getMAttrs().begin(),
                  codegen::getMAttrs().end(),
                  [](const std::string &a) {
                    return a == "help";
                  })) {
    auto Triple = llvm::Triple(LLVM_DEFAULT_TARGET_TRIPLE);
    std::string ErrMsg;
    if (auto Target = llvm::TargetRegistry::lookupTarget(
            Triple.getTriple(), ErrMsg)) {
      llvm::errs() << "Targeting " << Target->getName()
                   << ". ";
      // This prints the available CPUs and features of the
      // target to stderr...
      Target->createMCSubtargetInfo(
          Triple.getTriple(), codegen::getCPUStr(),
          codegen::getFeaturesStr());
    } else {
      llvm::errs() << ErrMsg << "\n";
      exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
  }

  llvm::TargetMachine *TM = createTargetMachine(Argv[0]);
  if (!TM)
    exit(EXIT_FAILURE);

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
      FileOrErr = llvm::MemoryBuffer::getFile(InputFile);
  if (std::error_code BufferError = FileOrErr.getError()) {
    llvm::WithColor::error(llvm::errs(), Argv[0])
        << "Error reading " << InputFile << ": "
        << BufferError.message() << "\n";
  }

  llvm::SourceMgr SrcMgr;
  DiagnosticsEngine Diags(SrcMgr);

  // Tell SrcMgr about this buffer, which is what the
  // parser will pick up.
  SrcMgr.AddNewSourceBuffer(std::move(*FileOrErr),
                            llvm::SMLoc());

  auto TheLexer = Lexer(SrcMgr, Diags);
  auto TheSema = Sema(Diags);
  auto TheParser = Parser(TheLexer, TheSema);
  auto *Mod = TheParser.parse();
  if (Mod && !Diags.numErrors()) {
    llvm::LLVMContext Ctx;
    if (CodeGenerator *CG =
            CodeGenerator::create(Ctx, TM)) {
      std::unique_ptr<llvm::Module> M =
          CG->run(Mod, InputFile);
      if (!emit(Argv[0], M.get(), TM, InputFile)) {
        llvm::WithColor::error(llvm::errs(), Argv[0])
            << "Error writing output\n";
      }
      delete CG;
    }
  }
}
