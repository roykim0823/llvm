#ifndef IR_GEN_CTX_H
#define IR_GEN_CTX_H

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"

#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include "llvm/Transforms/Utils/Mem2Reg.h"

#include "../../include/KaleidoscopeJIT.h"

#include <map>
#include <memory>

namespace toy {

// forward declaration of AST classes to avoid circular dependency
class PrototypeAST;

class IRGenContext {
public:
    std::unique_ptr<llvm::LLVMContext> theContext;  // An opaque object that owns a lot of core LLVM data structures,
                                                  // such as the type and constant value tables.
    std::unique_ptr<llvm::Module> theModule;      // an LLVM construct that contains functions and global variables.
    std::unique_ptr<llvm::IRBuilder<>> builder;     // A helper object that makes it easy to generate LLVM instructions.

    // llvm::Value* -> llvm::AllocaInst* to use alloca
    std::map<std::string, llvm::AllocaInst*> namedValues;  // it keeps track of which values are defined in the current scope and what their LLVM representation is.
                                                            // a.k.a. symbol table

    std::unique_ptr<llvm::orc::KaleidoscopeJIT> theJIT;
    llvm::ExitOnError ExitOnErr;

    // Global variables
    // -----------------
    // To support JIT, we need to keep track of function prototypes across modules.
    // This map serves as a registry for function prototypes, allowing us to look up and
    // codegen function declarations as needed when generating code for function calls.
    std::map<std::string, std::unique_ptr<toy::PrototypeAST>> functionProtos;

    // Moved from Parser to support user-defined operators: FunctionAST::codegen
    // installs a new operator's precedence here, so codegen must own the table.
    std::map<char, int> binopPrecedence;

    IRGenContext() {
        // Set up the host target (the JIT below exists only to supply a data layout).
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        // Initialize the JIT.  This takes ownership of the process control object, and will clean it up on destruction.
        theJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
        InitializeModuleAndPassManager();

        // Initialize the binary op precedence
        binopPrecedence['='] = 2;  // Mutable Variable
        binopPrecedence['<'] = 10;
        binopPrecedence['+'] = 20;
        binopPrecedence['-'] = 20;
        binopPrecedence['*'] = 40;
    }

    // To support JIT, we need to be able to create a new module and pass manager for each top-level expression we evaluate.
    // It is called from Parser::handleTopLevelExpression and Parser::handleDefinition
    //after we add the module to the JIT, so that each top-level expression and function definition gets its own module.
    void InitializeModuleAndPassManager() {
        // Open a new context and module.
        theContext = std::make_unique<llvm::LLVMContext>();
        theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);

        theModule->setDataLayout(theJIT->getDataLayout());

        // Create a new builder for the module.
        builder = std::make_unique<llvm::IRBuilder<>>(*theContext);
    }
};

} // end namespace toy

#endif