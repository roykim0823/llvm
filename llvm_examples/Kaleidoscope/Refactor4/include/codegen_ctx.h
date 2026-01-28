#ifndef CODEGEN_CONTEXT_H
#define CODEGEN_CONTEXT_H

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include <map>
#include <memory>

#include "../../include/KaleidoscopeJIT.h"

namespace toy {

class CodegenContext {
public:
    std::unique_ptr<llvm::LLVMContext> theContext;  // An opaque object that owns a lot of core LLVM data structures, 
                                                  // such as the type and constant value tables.
    std::unique_ptr<llvm::Module> theModule;      // an LLVM construct that contains functions and global variables.
    std::unique_ptr<llvm::IRBuilder<>> builder;     // A helper object that makes it easy to generate LLVM instructions.
    std::map<std::string, llvm::Value *> namedValues;  // it keeps track of which values are defined in the current scope and what their LLVM representation is.
                                                    // a.k.a. symbol table

    std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM; // The function pass manager is used to optimize functions.
    std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
    llvm::ExitOnError ExitOnErr;
    
    void InitializeModuleAndPassManager() {
        // Open a new context and module.
        theContext = std::make_unique<llvm::LLVMContext>();
        theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);        
        theModule->setDataLayout(TheJIT->getDataLayout());

        // Create a new builder for the module.
        builder = std::make_unique<llvm::IRBuilder<>>(*theContext);

        // Create a new pass manager attached to it.
        TheFPM = std::make_unique<llvm::legacy::FunctionPassManager>(theModule.get());

        // Do simple "peephole" optimizations and bit-twiddling optzns.
        TheFPM->add(llvm::createInstructionCombiningPass());
        // Reassociate expressions.
        TheFPM->add(llvm::createReassociatePass());
        // Eliminate Common SubExpressions.
        TheFPM->add(llvm::createGVNPass());
        // Simplify the control flow graph (deleting unreachable blocks, etc).
        TheFPM->add(llvm::createCFGSimplificationPass());

        TheFPM->doInitialization();
    }

};
} // end namespace toy

#endif