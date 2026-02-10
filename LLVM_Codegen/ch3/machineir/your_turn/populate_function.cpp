#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h" // For CreateStackObject.
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineMemOperand.h" // For MachinePointerInfo.
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/CodeGen/TargetOpcodes.h"     // For INLINEASM.
#include "llvm/CodeGenTypes/LowLevelType.h" // For LLT.
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h" // For ICMP_EQ.

using namespace llvm;

// The goal of this function is to build a MachineFunction that
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
// The proposed ABI is:
// - 32-bit arguments are passed through registers: w0, w1
// - 32-bit returned values are passed through registers: w0, w1
// w0 and w1 are given as argument of this Function.
//
// The local variable named var is expected to live on the stack.
MachineFunction *populateMachineIR(MachineModuleInfo &MMI, Function &Foo,
                                   Register W0, Register W1) {
  MachineFunction &MF = MMI.getOrCreateMachineFunction(Foo);

  // Create the 3 basic blocks that compose Foo.
  MachineBasicBlock *EntryBB = MF.CreateMachineBasicBlock();
  MF.push_back(EntryBB);
  MachineBasicBlock *ThenBB = MF.CreateMachineBasicBlock();
  MF.push_back(ThenBB);
  MachineBasicBlock *ExitBB = MF.CreateMachineBasicBlock();
  MF.push_back(ExitBB);

  // Create the configuration of the CFG.
  EntryBB->addSuccessor(ThenBB);
  EntryBB->addSuccessor(ExitBB);
  ThenBB->addSuccessor(ExitBB);

  // The type for bool.
  LLT I1 = LLT::scalar(1);
  // The type of var.
  LLT I32 = LLT::scalar(32);

  // To use to create load and store for var.
  MachinePointerInfo PtrInfo;
  Align VarStackAlign(4);

  // The type for the address of var.
  LLT VarAddrLLT = LLT::pointer(/*AddressSpace=*/0, /*SizeInBits=*/64);

  // The stack slot for var.
  int FrameIndex = MF.getFrameInfo().CreateStackObject(32, VarStackAlign,
                                                       /*IsSpillSlot=*/false);

  // Populate EntryBB
  MachineIRBuilder MIBuilder(*EntryBB, EntryBB->end());
  // Get the two arguments a and b from W0 and W1.
  Register A = MIBuilder.buildCopy(I32, W0).getReg(0);
  Register B = MIBuilder.buildCopy(I32, W1).getReg(0);
  // Get the stack slot for var.
  Register VarStackAddr =
      MIBuilder.buildFrameIndex(VarAddrLLT, FrameIndex).getReg(0);
  // Add a and b.
  Register Sum = MIBuilder.buildAdd(I32, A, B).getReg(0);
  // Store the result in var.
  MIBuilder.buildStore(Sum, VarStackAddr, PtrInfo, VarStackAlign);
  // Load var to compare it with 0xFF.
  Register VarLoaded =
      MIBuilder.buildLoad(I32, VarStackAddr, PtrInfo, VarStackAlign).getReg(0);
  // Create constant 0xFF.
  Register Cst0xFF = MIBuilder.buildConstant(I32, 0xFF).getReg(0);
  // Compare var and 0xFF.
  Register Cmp = MIBuilder.buildICmp(ICmpInst::ICMP_EQ, I1, VarLoaded, Cst0xFF)
                     .getReg(0);
  // Create the conditional branch.
  MIBuilder.buildBrCond(Cmp, *ThenBB);
  // Otherwise jump to ExitBB;
  MIBuilder.buildBr(*ExitBB);

  // Populate ThenBB
  // Reset MIBuilder to point at the end of ThenBB.
  MIBuilder.setInsertPt(*ThenBB, ThenBB->end());
  // Load var to pass it to bar.
  Register VarForBar1 =
      MIBuilder.buildLoad(I32, VarStackAddr, PtrInfo, VarStackAlign).getReg(0);
  // Move var into W0 for the call to bar.
  MIBuilder.buildCopy(W0, VarForBar1);
  // Fake call to bar.
  MIBuilder.buildInstr(TargetOpcode::INLINEASM, {}, {})
      .addExternalSymbol("bl @bar")
      .addImm(0)
      .addReg(W0, RegState::Implicit);
  // Fake call to baz.
  MIBuilder.buildInstr(TargetOpcode::INLINEASM, {}, {})
      .addExternalSymbol("bl @baz")
      .addImm(0)
      .addReg(W0, RegState::Implicit | RegState::Define);
  // Store the result of baz into var.
  Register BazResult = MIBuilder.buildCopy(I32, W0).getReg(0);
  MIBuilder.buildStore(BazResult, VarStackAddr, PtrInfo, VarStackAlign);
  // Fall through to ExitBB, no need to for a terminator.

  // Populate ExitBB
  // Reset MIBuilder to point at the end of ExitBB.
  MIBuilder.setInsertPt(*ExitBB, ExitBB->end());
  // Load var to pass it to bar.
  Register VarForBar2 =
      MIBuilder.buildLoad(I32, VarStackAddr, PtrInfo, VarStackAlign).getReg(0);
  // Move var into W0 for the call to bar.
  MIBuilder.buildCopy(W0, VarForBar2);
  // Fake call to bar.
  MIBuilder.buildInstr(TargetOpcode::INLINEASM, {}, {})
      .addExternalSymbol("bl @bar")
      .addImm(0)
      .addReg(W0, RegState::Implicit);
  // End of the function, return void;
  MIBuilder.buildInstr(TargetOpcode::INLINEASM, {}, {})
      .addExternalSymbol("ret")
      .addImm(0);

  return &MF;
}
