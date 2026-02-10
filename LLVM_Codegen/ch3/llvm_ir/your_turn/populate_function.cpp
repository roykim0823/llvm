#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"    // For ConstantInt.
#include "llvm/IR/DerivedTypes.h" // For PointerType, FunctionType.
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Debug.h" // For errs().

#include <memory> // For unique_ptr

using namespace llvm;

// The goal of this function is to build a Module that
// represents the lowering of the following foo, a C function:
// extern int baz();
// extern void bar(int);
// void foo(int a, int b) {
//   int var = a + b;
//   if (var == 0xFF) {
//     bar(var);
//     var = baz();
//   }
//   bar(var);
// }
//
// The IR for this snippet (at O0) is:
// define void @foo(i32 %arg, i32 %arg1) {
// bb:
//   %i = alloca i32
//   %i2 = alloca i32
//   %i3 = alloca i32
//   store i32 %arg, ptr %i
//   store i32 %arg1, ptr %i2
//   %i4 = load i32, ptr %i
//   %i5 = load i32, ptr %i2
//   %i6 = add i32 %i4, %i5
//   store i32 %i6, ptr %i3
//   %i7 = load i32, ptr %i3
//   %i8 = icmp eq i32 %i7, 255
//   br i1 %i8, label %bb9, label %bb12
//
// bb9:
//   %i10 = load i32, ptr %i3
//   call void @bar(i32 %i10)
//   %i11 = call i32 @baz()
//   store i32 %i11, ptr %i3
//   br label %bb12
//
// bb12:
//   %i13 = load i32, ptr %i3
//   call void @bar(i32 %i13)
//   ret void
// }
//
// declare void @bar(i32)
// declare i32 @baz(...)
std::unique_ptr<Module> myBuildModule(LLVMContext &Ctxt) {
  // Create the types
  Type *Int32Ty = Type::getInt32Ty(Ctxt);
  Type *VoidTy = Type::getVoidTy(Ctxt);
  // Type *PtrTy = PointerType::get(Ctxt, /*AddrSpace=*/0);  // not necessary?

  // Create llvm::Module
  std::unique_ptr<Module> MyModule =
      std::make_unique<Module>("My Module", Ctxt);

  // Populate all the functions (declaration for now.)
  // baz()
  FunctionType *BazTy = FunctionType::get(Int32Ty, /*ArgsTy*/ false);
  Function *BazFunc =
      cast<Function>(MyModule->getOrInsertFunction("baz", BazTy).getCallee());

  // bar(int)
  FunctionType *BarTy = FunctionType::get(VoidTy, ArrayRef(Int32Ty), /*isVarArg=*/false);
  Function *BarFunc =
      cast<Function>(MyModule->getOrInsertFunction("bar", BarTy).getCallee());

  // foo(int, int)
  FunctionType *FooTy = FunctionType::get(VoidTy, ArrayRef({Int32Ty, Int32Ty}), false);
  Function *FooFunc =
      cast<Function>(MyModule->getOrInsertFunction("foo", FooTy).getCallee());

  // Create BasicBlocks for foo
  BasicBlock *BB = BasicBlock::Create(Ctxt, "bb", FooFunc);
  BasicBlock *BB9 = BasicBlock::Create(Ctxt, "bb9", FooFunc);
  BasicBlock *BB12 = BasicBlock::Create(Ctxt, "bb12", FooFunc);

  // Populate the first BasicBlock
  IRBuilder<> Builder(BB);
  // Allocate space for local variables
  Value *I = Builder.CreateAlloca(Int32Ty);
  Value *I2 = Builder.CreateAlloca(Int32Ty);
  Value *I3 = Builder.CreateAlloca(Int32Ty);

  // Store the arguments into local variables
  Builder.CreateStore(FooFunc->getArg(0), I);
  Builder.CreateStore(FooFunc->getArg(1), I2);

  // Load and add the values
  Value *I4 = Builder.CreateLoad(Int32Ty, I);
  Value *I5 = Builder.CreateLoad(Int32Ty, I2);
  Value *I6 = Builder.CreateAdd(I4, I5);
  Builder.CreateStore(I6, I3);

  // Compare with constant value (0xFF)
  Value *Const255 = ConstantInt::get(Int32Ty, 0xFF);
  Value *I7 = Builder.CreateLoad(Int32Ty, I3);
  Value *I8 = Builder.CreateICmpEQ(I7, Const255);

  // Branch based on comparison
  Builder.CreateCondBr(I8, BB9, BB12);
  // --------------------------------------------------------------

  // Populate BB9
  Builder.SetInsertPoint(BB9);
  Value *I10 = Builder.CreateLoad(Int32Ty, I3);
  Builder.CreateCall(BarFunc->getFunctionType(), BarFunc, ArrayRef(I10));
  Value *I11 = Builder.CreateCall(BazFunc->getFunctionType(), BazFunc);
  Builder.CreateStore(I11, I3);
  Builder.CreateBr(BB12);

  // Populate BB12
  Builder.SetInsertPoint(BB12);
  Value *I13 = Builder.CreateLoad(Int32Ty, I3);
  Builder.CreateCall(BarFunc->getFunctionType(), BarFunc, ArrayRef(I13));
  Builder.CreateRetVoid();

  return MyModule;
}
