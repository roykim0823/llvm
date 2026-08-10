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
    std::map<std::string, llvm::Value *> namedValues;  // it keeps track of which values are defined in the current scope and what their LLVM representation is.
                                                    // a.k.a. symbol table

    // Chapter 4.2 additions:
    std::unique_ptr<llvm::FunctionPassManager> theFPM;
    std::unique_ptr<llvm::LoopAnalysisManager> theLAM;
    std::unique_ptr<llvm::FunctionAnalysisManager> theFAM;
    std::unique_ptr<llvm::CGSCCAnalysisManager> theCGAM;
    std::unique_ptr<llvm::ModuleAnalysisManager> theMAM;
    std::unique_ptr<llvm::PassInstrumentationCallbacks> thePIC;
    std::unique_ptr<llvm::StandardInstrumentations> theSI;

    std::unique_ptr<llvm::orc::KaleidoscopeJIT> theJIT;
    llvm::ExitOnError ExitOnErr;

    // Global variables
    // -----------------
    // To support JIT, we need to keep track of function prototypes across modules.
    // This map serves as a registry for function prototypes, allowing us to look up and
    // codegen function declarations as needed when generating code for function calls.
    std::map<std::string, std::unique_ptr<toy::PrototypeAST>> functionProtos;

    // move from Paser to support user-defined op
    std::map<char, int> binopPrecedence;

    IRGenContext() {
        // Initialize the JIT.  This takes ownership of the process control object, and will clean it up on destruction.
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        // Initialize the JIT.  This takes ownership of the process control object, and will clean it up on destruction.
        theJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
        InitializeModuleAndPassManager();

        // Initialize the binary op percedence
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

        // set the data layout of the module to match the target machine's data layout.
        // This is important for ensuring that the generated code is compatible with the target architecture.
        theModule->setDataLayout(theJIT->getDataLayout());

        // Create a new builder for the module.
        builder = std::make_unique<llvm::IRBuilder<>>(*theContext);

        // Create new pass and analysis managers.
        theFPM = std::make_unique<llvm::FunctionPassManager>();
        theLAM = std::make_unique<llvm::LoopAnalysisManager>();
        theFAM = std::make_unique<llvm::FunctionAnalysisManager>();
        theCGAM = std::make_unique<llvm::CGSCCAnalysisManager>();
        theMAM = std::make_unique<llvm::ModuleAnalysisManager>();
        thePIC = std::make_unique<llvm::PassInstrumentationCallbacks>();
        theSI = std::make_unique<llvm::StandardInstrumentations>(*theContext,
                                                     /*DebugLogging*/ true);
        theSI->registerCallbacks(*thePIC, theMAM.get());

        // Add transform passes.
        // Do simple "peephole" optimizations and bit-twiddling optzns.
        theFPM->addPass(llvm::InstCombinePass());
        // Reassociate expressions.
        theFPM->addPass(llvm::ReassociatePass());
        // Eliminate Common SubExpressions.
        theFPM->addPass(llvm::GVNPass());
        // Simplify the control flow graph (deleting unreachable blocks, etc).
        theFPM->addPass(llvm::SimplifyCFGPass());

        // Register analysis passes used in these transform passes.
        llvm::PassBuilder PB;
        PB.registerModuleAnalyses(*theMAM);
        PB.registerFunctionAnalyses(*theFAM);
        PB.crossRegisterProxies(*theLAM, *theFAM, *theCGAM, *theMAM);
    }
};

} // end namespace toy

#endif