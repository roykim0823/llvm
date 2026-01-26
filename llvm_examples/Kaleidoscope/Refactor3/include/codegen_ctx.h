#ifndef CODEGEN_CONTEXT_H
#define CODEGEN_CONTEXT_H

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>

namespace toy {

class CodegenContext {
public:
    std::unique_ptr<llvm::LLVMContext> theContext;  // An opaque object that owns a lot of core LLVM data structures, 
                                                  // such as the type and constant value tables.
    std::unique_ptr<llvm::Module> theModule;      // an LLVM construct that contains functions and global variables.
    std::unique_ptr<llvm::IRBuilder<>> builder;     // A helper object that makes it easy to generate LLVM instructions.
    std::map<std::string, llvm::Value *> namedValues;  // it keeps track of which values are defined in the current scope and what their LLVM representation is.
                                                    // a.k.a. symbol table

    CodegenContext() {
        // Open a new context and module.
        theContext = std::make_unique<llvm::LLVMContext>();
        theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);
        // Create a new builder for the module.
        builder = std::make_unique<llvm::IRBuilder<>>(*theContext);
    }
};

} // end namespace toy

#endif