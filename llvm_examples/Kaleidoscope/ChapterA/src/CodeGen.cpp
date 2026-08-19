//===- CodeGen.cpp - LLVM IR generation from the Kaleidoscope AST ---------===//
//
// Implementation of IR generation. The AST is pure data; this file performs
// all traversal via a switch on the node kind (with llvm::cast<>), mirrors
// the MLIR Toy tutorial's MLIRGenImpl structure, and keeps scoping in an
// llvm::ScopedHashTable so shadowed bindings restore themselves via RAII --
// including on error paths.
//
// Deliberate departures from the upstream Kaleidoscope toy.cpp:
//  - an unknown binary operator is a reported error, not an assert;
//  - the '=' destination is checked with llvm::dyn_cast, so '1 = 2' is a
//    reported error, not undefined behavior;
//  - symbol table lookups never insert (no operator[] pollution);
//  - verifyFunction/verifyModule results are actually checked;
//  - if/then/else basic blocks cannot leak on error paths.
//
//===----------------------------------------------------------------------===//

#include "toy/CodeGen.h"
#include "toy/AST.h"

#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <optional>

using namespace toy;

//===----------------------------------------------------------------------===//
// Session state
//===----------------------------------------------------------------------===//

struct CodeGenSession::Impl {
  CodeGenOptions options;

  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<llvm::IRBuilder<>> builder;

  // Per-function optimization pipeline (only used with options.optimize).
  std::unique_ptr<llvm::FunctionPassManager> fpm;
  std::unique_ptr<llvm::LoopAnalysisManager> lam;
  std::unique_ptr<llvm::FunctionAnalysisManager> fam;
  std::unique_ptr<llvm::CGSCCAnalysisManager> cgam;
  std::unique_ptr<llvm::ModuleAnalysisManager> mam;
  std::unique_ptr<llvm::PassInstrumentationCallbacks> pic;
  std::unique_ptr<llvm::StandardInstrumentations> si;

  /// Registry of every function prototype seen so far, so declarations can be
  /// re-emitted into fresh modules (JIT mode). Non-owning: the ModuleAST owns
  /// the prototypes and must outlive the session.
  llvm::StringMap<PrototypeAST *> functionProtos;

  /// The symbol table for mutable variables (allocas). Scopes are RAII.
  llvm::ScopedHashTable<llvm::StringRef, llvm::AllocaInst *> symbolTable;
  using SymbolTableScopeT =
      llvm::ScopedHashTableScope<llvm::StringRef, llvm::AllocaInst *>;

  /// Data layout applied to every module (set by the JIT or a TargetMachine).
  std::optional<llvm::DataLayout> dataLayout;

  // Debug info state (only with options.emitDebugInfo).
  std::unique_ptr<llvm::DIBuilder> dbuilder;
  llvm::DICompileUnit *debugCU = nullptr;
  llvm::DIType *debugDoubleTy = nullptr;

  explicit Impl(CodeGenOptions opts) : options(std::move(opts)) {
    initializeModule();
  }

  //===--------------------------------------------------------------------===//
  // Module lifecycle
  //===--------------------------------------------------------------------===//

  void initializeModule() {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("kaleidoscope", *context);
    if (dataLayout)
      module->setDataLayout(*dataLayout);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    fpm = std::make_unique<llvm::FunctionPassManager>();
    lam = std::make_unique<llvm::LoopAnalysisManager>();
    fam = std::make_unique<llvm::FunctionAnalysisManager>();
    cgam = std::make_unique<llvm::CGSCCAnalysisManager>();
    mam = std::make_unique<llvm::ModuleAnalysisManager>();
    pic = std::make_unique<llvm::PassInstrumentationCallbacks>();
    si = std::make_unique<llvm::StandardInstrumentations>(
        *context, /*DebugLogging=*/false);
    si->registerCallbacks(*pic, mam.get());

    // Promote allocas to registers (SSA construction for mutable variables).
    fpm->addPass(llvm::PromotePass());
    // Simple "peephole" optimizations and bit-twiddling.
    fpm->addPass(llvm::InstCombinePass());
    // Reassociate expressions.
    fpm->addPass(llvm::ReassociatePass());
    // Eliminate common subexpressions.
    fpm->addPass(llvm::GVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc).
    fpm->addPass(llvm::SimplifyCFGPass());

    llvm::PassBuilder pb;
    pb.registerModuleAnalyses(*mam);
    pb.registerFunctionAnalyses(*fam);
    pb.crossRegisterProxies(*lam, *fam, *cgam, *mam);

    if (options.emitDebugInfo)
      initializeDebugInfo();
  }

  void initializeDebugInfo() {
    module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                          llvm::DEBUG_METADATA_VERSION);
    if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
      module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

    dbuilder = std::make_unique<llvm::DIBuilder>(*module);
    debugCU = dbuilder->createCompileUnit(
        llvm::dwarf::DW_LANG_C,
        dbuilder->createFile(options.sourceFile, "."),
        "Kaleidoscope Compiler", /*isOptimized=*/options.optimize, "",
        /*RV=*/0);
    debugDoubleTy =
        dbuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
  }

  /// Tear down everything referencing the current context, then move the
  /// module/context pair out. Order matters: instrumentation and the builder
  /// hold references into the context.
  llvm::orc::ThreadSafeModule take() {
    si.reset();
    pic.reset();
    fpm.reset();
    lam.reset();
    fam.reset();
    cgam.reset();
    mam.reset();
    builder.reset();
    dbuilder.reset();
    debugCU = nullptr;
    debugDoubleTy = nullptr;

    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(context));
    initializeModule();
    return tsm;
  }

  //===--------------------------------------------------------------------===//
  // Error handling
  //===--------------------------------------------------------------------===//

  /// Report an error at a source location. Always returns nullptr so call
  /// sites read `return emitError(loc, "...")`.
  llvm::Value *emitError(const Location &loc, const llvm::Twine &msg) {
    llvm::errs() << "Codegen error (" << loc.line << ", " << loc.col
                 << "): " << msg << "\n";
    return nullptr;
  }

  //===--------------------------------------------------------------------===//
  // Debug info helpers
  //===--------------------------------------------------------------------===//

  llvm::DISubroutineType *createFunctionDebugType(unsigned numArgs) {
    llvm::SmallVector<llvm::Metadata *, 8> eltTys(numArgs + 1, debugDoubleTy);
    return dbuilder->createSubroutineType(
        dbuilder->getOrCreateTypeArray(eltTys));
  }

  /// Attach the location of `node` to subsequently emitted instructions.
  void emitLocation(const ExprAST *node, llvm::DIScope *scope) {
    if (!dbuilder)
      return;
    if (!node) {
      builder->SetCurrentDebugLocation(llvm::DebugLoc());
      return;
    }
    builder->SetCurrentDebugLocation(llvm::DILocation::get(
        scope->getContext(), node->loc().line, node->loc().col, scope));
  }

  /// Debug scope for the function currently being generated (null without -g).
  llvm::DISubprogram *currentSubprogram = nullptr;

  //===--------------------------------------------------------------------===//
  // Helpers
  //===--------------------------------------------------------------------===//

  llvm::Type *doubleTy() { return llvm::Type::getDoubleTy(*context); }

  /// Find a function by name: first in the current module, then by
  /// re-emitting a declaration from a previously seen prototype.
  llvm::Function *getFunction(llvm::StringRef name) {
    if (auto *f = module->getFunction(name))
      return f;
    auto it = functionProtos.find(name);
    if (it != functionProtos.end())
      return emitPrototype(*it->second);
    return nullptr;
  }

  /// Create an alloca in the entry block of the function, so mem2reg can
  /// promote it.
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Function *fn,
                                           llvm::StringRef varName) {
    llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return tmp.CreateAlloca(doubleTy(), nullptr, varName);
  }

  /// Drop a function whose body failed to build. If nothing references it,
  /// remove it entirely; if earlier code already calls it (e.g. it was
  /// declared by an extern and used before this definition), revert it to a
  /// plain declaration instead -- erasing it would leave those call sites
  /// with dangling operands (use-after-free at module teardown).
  void discardBrokenFunction(llvm::Function *fn) {
    fn->deleteBody();
    fn->setSubprogram(nullptr);
    if (fn->use_empty())
      fn->eraseFromParent();
  }

  //===--------------------------------------------------------------------===//
  // Expression code generation (dispatch on the AST node kind)
  //===--------------------------------------------------------------------===//

  llvm::Value *emitExpr(ExprAST &expr) {
    if (dbuilder && currentSubprogram)
      emitLocation(&expr, currentSubprogram);
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return emit(llvm::cast<NumberExprAST>(expr));
    case ExprAST::Expr_Var:
      return emit(llvm::cast<VariableExprAST>(expr));
    case ExprAST::Expr_Unary:
      return emit(llvm::cast<UnaryExprAST>(expr));
    case ExprAST::Expr_BinOp:
      return emit(llvm::cast<BinaryExprAST>(expr));
    case ExprAST::Expr_Call:
      return emit(llvm::cast<CallExprAST>(expr));
    case ExprAST::Expr_If:
      return emit(llvm::cast<IfExprAST>(expr));
    case ExprAST::Expr_For:
      return emit(llvm::cast<ForExprAST>(expr));
    case ExprAST::Expr_VarDecl:
      return emit(llvm::cast<VarExprAST>(expr));
    }
    return emitError(expr.loc(), "unhandled expression kind");
  }

  llvm::Value *emit(NumberExprAST &num) {
    return llvm::ConstantFP::get(*context, llvm::APFloat(num.getValue()));
  }

  llvm::Value *emit(VariableExprAST &var) {
    llvm::AllocaInst *alloca = symbolTable.lookup(var.getName());
    if (!alloca)
      return emitError(var.loc(),
                       "unknown variable '" + var.getName() + "'");
    return builder->CreateLoad(alloca->getAllocatedType(), alloca,
                               var.getName());
  }

  llvm::Value *emit(UnaryExprAST &unary) {
    llvm::Value *operand = emitExpr(*unary.getOperand());
    if (!operand)
      return nullptr;

    llvm::Function *fn =
        getFunction(std::string("unary") + unary.getOpcode());
    if (!fn)
      return emitError(unary.loc(), llvm::Twine("unknown unary operator '") +
                                        llvm::Twine(unary.getOpcode()) + "'");
    return builder->CreateCall(fn, operand, "unop");
  }

  llvm::Value *emit(BinaryExprAST &bin) {
    // Assignment is special: the LHS is not emitted as an expression.
    if (bin.getOp() == '=') {
      // The destination must be a variable reference. dyn_cast (enabled by
      // the AST kind tags) makes this a checked error instead of UB.
      auto *lhs = llvm::dyn_cast<VariableExprAST>(bin.getLHS());
      if (!lhs)
        return emitError(bin.loc(), "destination of '=' must be a variable");

      llvm::Value *val = emitExpr(*bin.getRHS());
      if (!val)
        return nullptr;

      llvm::AllocaInst *alloca = symbolTable.lookup(lhs->getName());
      if (!alloca)
        return emitError(lhs->loc(),
                         "unknown variable '" + lhs->getName() + "'");
      builder->CreateStore(val, alloca);
      return val;
    }

    llvm::Value *l = emitExpr(*bin.getLHS());
    llvm::Value *r = emitExpr(*bin.getRHS());
    if (!l || !r)
      return nullptr;

    switch (bin.getOp()) {
    case '+':
      return builder->CreateFAdd(l, r, "addtmp");
    case '-':
      return builder->CreateFSub(l, r, "subtmp");
    case '*':
      return builder->CreateFMul(l, r, "multmp");
    case '<':
      l = builder->CreateFCmpULT(l, r, "cmptmp");
      // Convert bool 0/1 to double 0.0 or 1.0
      return builder->CreateUIToFP(l, doubleTy(), "booltmp");
    default:
      break;
    }

    // Not a builtin: must be a user-defined operator. Unlike upstream's
    // assert, a missing definition is a reported error.
    llvm::Function *fn = getFunction(std::string("binary") + bin.getOp());
    if (!fn)
      return emitError(bin.loc(), llvm::Twine("unknown binary operator '") +
                                      llvm::Twine(bin.getOp()) + "'");
    llvm::Value *ops[] = {l, r};
    return builder->CreateCall(fn, ops, "binop");
  }

  llvm::Value *emit(CallExprAST &call) {
    llvm::Function *callee = getFunction(call.getCallee());
    if (!callee)
      return emitError(call.loc(),
                       "unknown function '" + call.getCallee() + "'");

    if (callee->arg_size() != call.getArgs().size())
      return emitError(call.loc(),
                       llvm::Twine("incorrect number of arguments: expected ") +
                           llvm::Twine(callee->arg_size()) + ", got " +
                           llvm::Twine(call.getArgs().size()));

    std::vector<llvm::Value *> args;
    for (auto &arg : call.getArgs()) {
      llvm::Value *v = emitExpr(*arg);
      if (!v)
        return nullptr;
      args.push_back(v);
    }
    return builder->CreateCall(callee, args, "calltmp");
  }

  llvm::Value *emit(IfExprAST &ifExpr) {
    llvm::Value *cond = emitExpr(*ifExpr.getCond());
    if (!cond)
      return nullptr;

    // Convert condition to a bool by comparing non-equal to 0.0.
    cond = builder->CreateFCmpONE(
        cond, llvm::ConstantFP::get(*context, llvm::APFloat(0.0)), "ifcond");

    llvm::Function *fn = builder->GetInsertBlock()->getParent();

    // Create the three blocks. 'then' is appended now; 'else' and 'ifcont'
    // are appended after the corresponding arm is emitted so the block order
    // matches the source. They are owned from the start, so error paths
    // cannot leak them.
    llvm::BasicBlock *thenBB =
        llvm::BasicBlock::Create(*context, "then", fn);
    llvm::BasicBlock *elseBB = llvm::BasicBlock::Create(*context, "else");
    llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(*context, "ifcont");
    auto eraseUnparented = [&]() {
      if (!elseBB->getParent())
        delete elseBB;
      if (!mergeBB->getParent())
        delete mergeBB;
    };

    builder->CreateCondBr(cond, thenBB, elseBB);

    // Emit the 'then' arm.
    builder->SetInsertPoint(thenBB);
    llvm::Value *thenV = emitExpr(*ifExpr.getThen());
    if (!thenV) {
      eraseUnparented();
      return nullptr;
    }
    builder->CreateBr(mergeBB);
    // Emission can change the current block; remember it for the PHI.
    thenBB = builder->GetInsertBlock();

    // Emit the 'else' arm.
    fn->insert(fn->end(), elseBB);
    builder->SetInsertPoint(elseBB);
    llvm::Value *elseV = emitExpr(*ifExpr.getElse());
    if (!elseV) {
      eraseUnparented();
      return nullptr;
    }
    builder->CreateBr(mergeBB);
    elseBB = builder->GetInsertBlock();

    // Emit the merge block with the PHI.
    fn->insert(fn->end(), mergeBB);
    builder->SetInsertPoint(mergeBB);
    llvm::PHINode *phi = builder->CreatePHI(doubleTy(), 2, "iftmp");
    phi->addIncoming(thenV, thenBB);
    phi->addIncoming(elseV, elseBB);
    return phi;
  }

  // Output the for-loop as:
  //   var = alloca double; store start -> var; br loop
  // loop:
  //   body; step; endcond (with the pre-increment value);
  //   store (load var) + step -> var; br endcond, loop, afterloop
  // afterloop:
  llvm::Value *emit(ForExprAST &forExpr) {
    llvm::Function *fn = builder->GetInsertBlock()->getParent();

    // The loop variable lives in an alloca (mem2reg rebuilds the PHI).
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(fn, forExpr.getVarName());

    // Emit the start expression first, without the variable in scope.
    llvm::Value *startVal = emitExpr(*forExpr.getStart());
    if (!startVal)
      return nullptr;
    builder->CreateStore(startVal, alloca);

    llvm::BasicBlock *loopBB = llvm::BasicBlock::Create(*context, "loop", fn);
    builder->CreateBr(loopBB);
    builder->SetInsertPoint(loopBB);

    // The loop variable is in scope for the body/step/end expressions;
    // shadowing of an outer variable restores itself when the scope dies.
    SymbolTableScopeT loopScope(symbolTable);
    symbolTable.insert(forExpr.getVarName(), alloca);

    // Emit the body; its value is ignored, but an error propagates.
    if (!emitExpr(*forExpr.getBody()))
      return nullptr;

    // Emit the step value (1.0 if unspecified).
    llvm::Value *stepVal = nullptr;
    if (forExpr.getStep()) {
      stepVal = emitExpr(*forExpr.getStep());
      if (!stepVal)
        return nullptr;
    } else {
      stepVal = llvm::ConstantFP::get(*context, llvm::APFloat(1.0));
    }

    // Compute the end condition with the pre-increment value.
    llvm::Value *endCond = emitExpr(*forExpr.getEnd());
    if (!endCond)
      return nullptr;

    // Reload, increment, and restore the alloca (the body may mutate it).
    llvm::Value *curVar = builder->CreateLoad(alloca->getAllocatedType(),
                                              alloca, forExpr.getVarName());
    llvm::Value *nextVar = builder->CreateFAdd(curVar, stepVal, "nextvar");
    builder->CreateStore(nextVar, alloca);

    endCond = builder->CreateFCmpONE(
        endCond, llvm::ConstantFP::get(*context, llvm::APFloat(0.0)),
        "loopcond");

    llvm::BasicBlock *afterBB =
        llvm::BasicBlock::Create(*context, "afterloop", fn);
    builder->CreateCondBr(endCond, loopBB, afterBB);
    builder->SetInsertPoint(afterBB);

    // A for expression always evaluates to 0.0.
    return llvm::Constant::getNullValue(doubleTy());
  }

  llvm::Value *emit(VarExprAST &varExpr) {
    llvm::Function *fn = builder->GetInsertBlock()->getParent();

    // One RAII scope covers all declared variables: shadowed outer bindings
    // restore automatically when the scope dies -- on success or error.
    SymbolTableScopeT varScope(symbolTable);

    for (auto &decl : varExpr.getVarNames()) {
      // Emit the initializer before adding the variable to scope, so
      // 'var a = 1 in var a = a in ...' refers to the outer 'a'.
      llvm::Value *initVal = nullptr;
      if (decl.second) {
        initVal = emitExpr(*decl.second);
        if (!initVal)
          return nullptr;
      } else {
        initVal = llvm::ConstantFP::get(*context, llvm::APFloat(0.0));
      }

      llvm::AllocaInst *alloca = createEntryBlockAlloca(fn, decl.first);
      builder->CreateStore(initVal, alloca);
      symbolTable.insert(decl.first, alloca);
    }

    return emitExpr(*varExpr.getBody());
  }

  //===--------------------------------------------------------------------===//
  // Prototypes and functions
  //===--------------------------------------------------------------------===//

  llvm::Function *emitPrototype(PrototypeAST &proto) {
    std::vector<llvm::Type *> doubles(proto.getArgs().size(), doubleTy());
    llvm::FunctionType *ft =
        llvm::FunctionType::get(doubleTy(), doubles, false);
    llvm::Function *fn = llvm::Function::Create(
        ft, llvm::Function::ExternalLinkage, proto.getName(), module.get());

    unsigned idx = 0;
    for (auto &arg : fn->args())
      arg.setName(proto.getArgs()[idx++]);
    return fn;
  }

  llvm::Function *emitFunction(FunctionAST &funcAST) {
    PrototypeAST &proto = *funcAST.getProto();
    // Register the prototype (non-owning) for cross-module redeclaration.
    functionProtos[proto.getName()] = &proto;

    llvm::Function *fn = getFunction(proto.getName());
    if (!fn)
      return nullptr;
    if (!fn->empty()) {
      emitError(proto.loc(),
                "function '" + proto.getName() + "' cannot be redefined");
      return nullptr;
    }

    llvm::BasicBlock *bb = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(bb);

    // Debug info: subprogram scope for this function.
    if (dbuilder) {
      llvm::DIFile *unit = dbuilder->createFile(debugCU->getFilename(),
                                                debugCU->getDirectory());
      currentSubprogram = dbuilder->createFunction(
          unit, proto.getName(), llvm::StringRef(), unit, proto.loc().line,
          createFunctionDebugType(fn->arg_size()), proto.loc().line,
          llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
      fn->setSubprogram(currentSubprogram);
      // Don't attach locations to the argument spills.
      emitLocation(nullptr, currentSubprogram);
    }

    // Arguments live in entry-block allocas so they are mutable; one RAII
    // scope pops them when the function is done.
    SymbolTableScopeT argScope(symbolTable);
    unsigned argIdx = 0;
    for (auto &arg : fn->args()) {
      llvm::AllocaInst *alloca = createEntryBlockAlloca(fn, arg.getName());
      builder->CreateStore(&arg, alloca);
      symbolTable.insert(proto.getArgs()[argIdx++], alloca);

      if (dbuilder) {
        llvm::DILocalVariable *d = dbuilder->createParameterVariable(
            currentSubprogram, arg.getName(), argIdx, currentSubprogram->getFile(),
            proto.loc().line, debugDoubleTy, true);
        dbuilder->insertDeclare(
            alloca, d, dbuilder->createExpression(),
            llvm::DILocation::get(*context, proto.loc().line, 0,
                                  currentSubprogram),
            builder->GetInsertBlock());
      }
    }

    llvm::Value *retVal = emitExpr(*funcAST.getBody());
    if (!retVal) {
      // Error reading the body: discard it (see discardBrokenFunction for
      // why this is not a plain eraseFromParent). The parser owns operator
      // precedence, so there is nothing to unregister here.
      discardBrokenFunction(fn);
      currentSubprogram = nullptr;
      return nullptr;
    }

    builder->CreateRet(retVal);
    currentSubprogram = nullptr;

    // Validate the generated code -- and actually act on the result.
    if (llvm::verifyFunction(*fn, &llvm::errs())) {
      discardBrokenFunction(fn);
      return nullptr;
    }

    if (options.optimize)
      fpm->run(*fn, *fam);

    return fn;
  }

  llvm::Function *emitRecord(RecordAST &record) {
    if (auto *func = llvm::dyn_cast<FunctionAST>(&record))
      return emitFunction(*func);

    auto *ext = llvm::cast<ExternAST>(&record);
    functionProtos[ext->getProto()->getName()] = ext->getProto();
    // Emit (or reuse) the declaration in the current module.
    return getFunction(ext->getProto()->getName());
  }
};

//===----------------------------------------------------------------------===//
// Public facade
//===----------------------------------------------------------------------===//

CodeGenSession::CodeGenSession(CodeGenOptions options)
    : impl(std::make_unique<Impl>(std::move(options))) {}

CodeGenSession::~CodeGenSession() = default;

llvm::Function *CodeGenSession::emitRecord(RecordAST &record) {
  return impl->emitRecord(record);
}

llvm::Module &CodeGenSession::currentModule() { return *impl->module; }

bool CodeGenSession::finalize() {
  if (impl->dbuilder)
    impl->dbuilder->finalize();
  if (llvm::verifyModule(*impl->module, &llvm::errs())) {
    llvm::errs() << "Codegen error: module verification failed\n";
    return false;
  }
  return true;
}

llvm::orc::ThreadSafeModule CodeGenSession::takeModule() {
  return impl->take();
}

void CodeGenSession::setDataLayout(const llvm::DataLayout &layout) {
  impl->dataLayout = layout;
  impl->module->setDataLayout(layout);
}
