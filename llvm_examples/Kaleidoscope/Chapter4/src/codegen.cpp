//===----------------------------------------------------------------------===//
// Code generation: CodeGenSession::Impl
//===----------------------------------------------------------------------===//
// The tutorial's codegen state (TheContext, TheModule, Builder, NamedValues,
// TheFPM + analysis managers, FunctionProtos) and its per-node `codegen()`
// methods all live here, as members of one private struct. The AST does not
// know about LLVM; this file dispatches on each node's kind tag and emits IR
// for it.
//
// Chapter 4: every function is optimized by a FunctionPassManager the moment
// it is built, and because the JIT freezes each module it receives, the
// session can hand its module off (take()) and open a fresh one -- with a
// registry of prototypes so calls into earlier modules can be re-declared.

#include "codegen.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/APFloat.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include "ast.h"
#include "log.h"

using namespace toy;

struct CodeGenSession::Impl {
  std::unique_ptr<llvm::LLVMContext> theContext;  // An opaque object that owns a lot of core LLVM data structures,
                                                  // such as the type and constant value tables.
  std::unique_ptr<llvm::Module> theModule;        // an LLVM construct that contains functions and global variables.
  std::unique_ptr<llvm::IRBuilder<>> builder;     // A helper object that makes it easy to generate LLVM instructions.
  std::map<std::string, llvm::Value *> namedValues;  // it keeps track of which values are defined in the current scope and what their LLVM representation is.
                                                     // a.k.a. symbol table

  // Chapter 4.2 additions: the per-function optimization pipeline.
  std::unique_ptr<llvm::FunctionPassManager> theFPM;
  std::unique_ptr<llvm::LoopAnalysisManager> theLAM;
  std::unique_ptr<llvm::FunctionAnalysisManager> theFAM;
  std::unique_ptr<llvm::CGSCCAnalysisManager> theCGAM;
  std::unique_ptr<llvm::ModuleAnalysisManager> theMAM;
  std::unique_ptr<llvm::PassInstrumentationCallbacks> thePIC;
  std::unique_ptr<llvm::StandardInstrumentations> theSI;

  // Chapter 4.3 additions: cross-module support.
  // To support JIT, we need to keep track of function prototypes across modules.
  // This map serves as a registry for function prototypes, allowing us to look up and
  // codegen function declarations as needed when generating code for function calls.
  // It stores COPIES (a prototype is just a name and its argument names), so the AST
  // that produced them stays intact and can be freed by the driver at any time.
  std::map<std::string, PrototypeAST> functionProtos;

  // Data layout applied to every module the session opens (set by the driver
  // from the JIT). Empty until setDataLayout() is called.
  std::optional<llvm::DataLayout> dataLayout;

  Impl() { initializeModule(); }

  //-----------------------------
  // Module lifecycle
  //-----------------------------

  // To support JIT, we need to be able to create a new module and pass manager for each
  // top-level expression we evaluate (the JIT freezes every module it is given). Called
  // once from the constructor and again from take() after the old module has moved out.
  void initializeModule() {
    // Open a new context and module.
    theContext = std::make_unique<llvm::LLVMContext>();
    theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);

    // set the data layout of the module to match the target machine's data layout.
    // This is important for ensuring that the generated code is compatible with the target architecture.
    if (dataLayout)
      theModule->setDataLayout(*dataLayout);

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

  // Tear down everything referencing the current context, then move the
  // module/context pair out. Order matters: the instrumentation, the
  // analysis managers and the builder all hold references into the context,
  // so they must go before the context does.
  llvm::orc::ThreadSafeModule take() {
    theSI.reset();
    thePIC.reset();
    theFPM.reset();
    theLAM.reset();
    theFAM.reset();
    theCGAM.reset();
    theMAM.reset();
    builder.reset();
    namedValues.clear();   // held Value*s into the module that is leaving

    llvm::orc::ThreadSafeModule TSM(std::move(theModule), std::move(theContext));
    initializeModule();
    return TSM;
  }

  //-----------------------------
  // Helpers
  //-----------------------------

  // To allow each function to live in its own module, we'll need a way to re-generate
  // previous function declarations into each new module we open.
  llvm::Function *getFunction(const std::string &Name) {
    // First, see if the function has already been added to the current module.
    if (auto *F = theModule->getFunction(Name))
      return F;

    // If not, check whether we can codegen the declaration from some existing prototype.
    auto FI = functionProtos.find(Name);
    if (FI != functionProtos.end())
      return emitDeclaration(FI->second);

    // If no existing prototype exists, return null.
    return nullptr;
  }

  //-----------------------------
  // Expression Code Generation
  //-----------------------------

  // Dispatch on the node kind. Each case hands the concrete node to the
  // matching emit() overload below.
  llvm::Value *emitExpr(ExprAST &expr) {
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return emit(llvm::cast<NumberExprAST>(expr));
    case ExprAST::Expr_Var:
      return emit(llvm::cast<VariableExprAST>(expr));
    case ExprAST::Expr_BinOp:
      return emit(llvm::cast<BinaryExprAST>(expr));
    case ExprAST::Expr_Call:
      return emit(llvm::cast<CallExprAST>(expr));
    }
    llvm_unreachable("unknown expression kind");
  }

  llvm::Value *emit(NumberExprAST &num) {
    return llvm::ConstantFP::get(*theContext, llvm::APFloat(num.getVal()));
  }

  llvm::Value *emit(VariableExprAST &var) {
    // Look this variable up in the function.
    llvm::Value *V = namedValues[var.getName()];
    if (!V)
      return logErrorV("Unknown variable name");
    return V;
  }

  llvm::Value *emit(BinaryExprAST &bin) {
    // Recursively emits code for the left-hand side of the expression, then the right-hand side,
    // then, we compute the result of the binary expression.
    llvm::Value *L = emitExpr(*bin.getLHS());
    llvm::Value *R = emitExpr(*bin.getRHS());
    if (!L || !R)
      return nullptr;

    switch (bin.getOp()) {
    case '+':
      return builder->CreateFAdd(L, R, "addtmp");
    case '-':
      return builder->CreateFSub(L, R, "subtmp");
    case '*':
      return builder->CreateFMul(L, R, "multmp");
    case '<':
      L = builder->CreateFCmpULT(L, R, "cmptmp");
      // Convert bool 0/1 to double 0.0 or 1.0
      return builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*theContext), "booltmp");
    default:
      return logErrorV("invalid binary operator");
    }
  }

  llvm::Value *emit(CallExprAST &call) {
    // Look up the name in the global module table.
    // To support multiple modules, we need to re-generate the function declaration
    // into the new module if it doesn't already exist.
    llvm::Function *CalleeF = getFunction(call.getCallee());
    if (!CalleeF)
      return logErrorV("Unknown function referenced");

    // If argument mismatch error.
    const auto &Args = call.getArgs();
    if (CalleeF->arg_size() != Args.size())
      return logErrorV("Incorrect # arguments passed");

    std::vector<llvm::Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      ArgsV.push_back(emitExpr(*Args[i]));
      if (!ArgsV.back())
        return nullptr;
    }

    return builder->CreateCall(CalleeF, ArgsV, "calltmp");
  }

  //-----------------------------
  // Function Code Generation: prototypes and functions
  //-----------------------------

  // Emit a declaration for the prototype into the current module. Used for extern
  // declarations, for the head of every definition, and by getFunction() to
  // re-declare a known function into a fresh module.
  llvm::Function *emitDeclaration(const PrototypeAST &proto) {
    const auto &Args = proto.getArgs();

    // Make the function type:  double(double,double) etc.
    std::vector<llvm::Type *> Doubles(Args.size(), llvm::Type::getDoubleTy(*theContext));
    llvm::FunctionType *FT =
        llvm::FunctionType::get(llvm::Type::getDoubleTy(*theContext), Doubles, false);

    // Create the IR Function corresponding to the Prototype
    llvm::Function *F =
        llvm::Function::Create(FT, llvm::Function::ExternalLinkage, proto.getName(), theModule.get());

    // Set names for all arguments.
    unsigned Idx = 0;
    for (auto &Arg : F->args())
      Arg.setName(Args[Idx++]);

    return F;
  }

  // `extern`: declare it here and remember the prototype for later modules.
  llvm::Function *emitPrototype(PrototypeAST &proto) {
    llvm::Function *F = emitDeclaration(proto);
    functionProtos.insert_or_assign(proto.getName(), proto);
    return F;
  }

  llvm::Function *emitFunction(FunctionAST &fn) {
    const PrototypeAST &Proto = *fn.getProto();

    // Resolve the Function to fill in: reuse a declaration already in this
    // module (a prior `extern`), otherwise declare it now from this prototype.
    llvm::Function *TheFunction = theModule->getFunction(Proto.getName());
    if (!TheFunction)
      TheFunction = emitDeclaration(Proto);

    // Create a new basic block to start insertion into.
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(*theContext, "entry", TheFunction);
    builder->SetInsertPoint(BB);

    // Record the function arguments in the NamedValues map.
    namedValues.clear();
    for (auto &Arg : TheFunction->args())
      namedValues[std::string(Arg.getName())] = &Arg;

    if (llvm::Value *RetVal = emitExpr(*fn.getBody())) {
      // Finish off the function.
      builder->CreateRet(RetVal);

      // Validate the generated code, checking for consistency.
      llvm::verifyFunction(*TheFunction);

      // Optimize the function.
      theFPM->run(*TheFunction, *theFAM);

      // Only now register the prototype (newest definition wins), so later
      // modules can re-declare and call this function. A definition whose body
      // failed is never registered: a later call reports "Unknown function
      // referenced" instead of reaching the JIT with an unresolvable symbol.
      functionProtos.insert_or_assign(Proto.getName(), Proto);

      return TheFunction;
    }

    // Error reading body, remove function.
    TheFunction->eraseFromParent();
    return nullptr;
  }
};

//===----------------------------------------------------------------------===//
// Public facade
//===----------------------------------------------------------------------===//

CodeGenSession::CodeGenSession() : impl(std::make_unique<Impl>()) {}

// Out of line so that Impl is a complete type where unique_ptr<Impl> is destroyed.
CodeGenSession::~CodeGenSession() = default;

llvm::Function *CodeGenSession::emitPrototype(PrototypeAST &proto) {
  return impl->emitPrototype(proto);
}

llvm::Function *CodeGenSession::emitFunction(FunctionAST &fn) {
  return impl->emitFunction(fn);
}

llvm::Module &CodeGenSession::currentModule() { return *impl->theModule; }

void CodeGenSession::eraseFunction(llvm::Function *fn) {
  // The pass managers cache analyses (dominator tree, ...) keyed by Function*.
  // Drop them before the pointer is freed, or a later function that happens to
  // reuse the address would be optimized against a stale, dangling result.
  impl->theFAM->clear(*fn, fn->getName());
  fn->eraseFromParent();
}

llvm::orc::ThreadSafeModule CodeGenSession::takeModule() { return impl->take(); }

void CodeGenSession::setDataLayout(const llvm::DataLayout &layout) {
  impl->dataLayout = layout;
  impl->theModule->setDataLayout(layout);
}
