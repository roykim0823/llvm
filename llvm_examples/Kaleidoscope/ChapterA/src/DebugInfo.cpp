//===- DebugInfo.cpp - DWARF emission for the toy compiler ----------------===//

#include "DebugInfo.h"

#include "toy/AST.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using namespace toy;

DebugInfoEmitter::DebugInfoEmitter(llvm::Module &module,
                                   llvm::StringRef sourceFile,
                                   bool optimized) {
  module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                       llvm::DEBUG_METADATA_VERSION);
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
    module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

  dbuilder = std::make_unique<llvm::DIBuilder>(module);
  cu = dbuilder->createCompileUnit(
      llvm::dwarf::DW_LANG_C, dbuilder->createFile(sourceFile, "."),
      "Kaleidoscope Compiler", optimized, "", /*RV=*/0);
  doubleTy = dbuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
}

void DebugInfoEmitter::functionBegin(const PrototypeAST &proto,
                                     llvm::Function &fn,
                                     llvm::IRBuilder<> &builder) {
  if (!dbuilder)
    return;

  llvm::DIFile *unit =
      dbuilder->createFile(cu->getFilename(), cu->getDirectory());
  // All Kaleidoscope functions are (double, ...) -> double.
  llvm::SmallVector<llvm::Metadata *, 8> eltTys(fn.arg_size() + 1, doubleTy);
  llvm::DISubroutineType *fnTy =
      dbuilder->createSubroutineType(dbuilder->getOrCreateTypeArray(eltTys));

  currentSubprogram = dbuilder->createFunction(
      unit, proto.getName(), llvm::StringRef(), unit, proto.loc().line, fnTy,
      proto.loc().line, llvm::DINode::FlagPrototyped,
      llvm::DISubprogram::SPFlagDefinition);
  fn.setSubprogram(currentSubprogram);

  // Don't attach locations to the argument spills that come next.
  builder.SetCurrentDebugLocation(llvm::DebugLoc());
}

void DebugInfoEmitter::declareParameter(const PrototypeAST &proto,
                                        llvm::AllocaInst &alloca,
                                        unsigned index,
                                        llvm::IRBuilder<> &builder) {
  if (!dbuilder)
    return;

  llvm::DILocalVariable *d = dbuilder->createParameterVariable(
      currentSubprogram, alloca.getName(), index,
      currentSubprogram->getFile(), proto.loc().line, doubleTy, true);
  dbuilder->insertDeclare(
      &alloca, d, dbuilder->createExpression(),
      llvm::DILocation::get(alloca.getContext(), proto.loc().line, 0,
                            currentSubprogram),
      builder.GetInsertBlock());
}

void DebugInfoEmitter::setLocation(const ExprAST *node,
                                   llvm::IRBuilder<> &builder) {
  if (!dbuilder || !currentSubprogram)
    return;
  if (!node) {
    builder.SetCurrentDebugLocation(llvm::DebugLoc());
    return;
  }
  builder.SetCurrentDebugLocation(
      llvm::DILocation::get(currentSubprogram->getContext(), node->loc().line,
                            node->loc().col, currentSubprogram));
}

void DebugInfoEmitter::functionEnd(llvm::Function &fn, bool succeeded) {
  if (!dbuilder)
    return;
  if (!succeeded)
    fn.setSubprogram(nullptr);
  currentSubprogram = nullptr;
}

void DebugInfoEmitter::finalize() {
  if (dbuilder)
    dbuilder->finalize();
}
