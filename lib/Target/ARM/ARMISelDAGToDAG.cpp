//===-- ARMISelDAGToDAG.cpp - A dag to dag inst selector for ARM ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the ARM target.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMISelLowering.h"
#include "ARMTargetMachine.h"
#include "ARMAddressingModes.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
using namespace llvm;

//===--------------------------------------------------------------------===//
/// ARMDAGToDAGISel - ARM specific code to select ARM machine
/// instructions for SelectionDAG operations.
///
namespace {
class ARMDAGToDAGISel : public SelectionDAGISel {
  ARMTargetLowering Lowering;

  /// Subtarget - Keep a pointer to the ARMSubtarget around so that we can
  /// make the right decision when generating code for different targets.
  const ARMSubtarget *Subtarget;

public:
  explicit ARMDAGToDAGISel(ARMTargetMachine &TM)
    : SelectionDAGISel(Lowering), Lowering(TM),
    Subtarget(&TM.getSubtarget<ARMSubtarget>()) {
  }

  virtual const char *getPassName() const {
    return "ARM Instruction Selection";
  } 
  
  SDNode *Select(SDValue Op);
  virtual void InstructionSelect(SelectionDAG &DAG);
  bool SelectAddrMode2(SDValue Op, SDValue N, SDValue &Base,
                       SDValue &Offset, SDValue &Opc);
  bool SelectAddrMode2Offset(SDValue Op, SDValue N,
                             SDValue &Offset, SDValue &Opc);
  bool SelectAddrMode3(SDValue Op, SDValue N, SDValue &Base,
                       SDValue &Offset, SDValue &Opc);
  bool SelectAddrMode3Offset(SDValue Op, SDValue N,
                             SDValue &Offset, SDValue &Opc);
  bool SelectAddrMode5(SDValue Op, SDValue N, SDValue &Base,
                       SDValue &Offset);

  bool SelectAddrModePC(SDValue Op, SDValue N, SDValue &Offset,
                         SDValue &Label);

  bool SelectThumbAddrModeRR(SDValue Op, SDValue N, SDValue &Base,
                             SDValue &Offset);
  bool SelectThumbAddrModeRI5(SDValue Op, SDValue N, unsigned Scale,
                              SDValue &Base, SDValue &OffImm,
                              SDValue &Offset);
  bool SelectThumbAddrModeS1(SDValue Op, SDValue N, SDValue &Base,
                             SDValue &OffImm, SDValue &Offset);
  bool SelectThumbAddrModeS2(SDValue Op, SDValue N, SDValue &Base,
                             SDValue &OffImm, SDValue &Offset);
  bool SelectThumbAddrModeS4(SDValue Op, SDValue N, SDValue &Base,
                             SDValue &OffImm, SDValue &Offset);
  bool SelectThumbAddrModeSP(SDValue Op, SDValue N, SDValue &Base,
                             SDValue &OffImm);

  bool SelectShifterOperandReg(SDValue Op, SDValue N, SDValue &A,
                               SDValue &B, SDValue &C);
  
  // Include the pieces autogenerated from the target description.
#include "ARMGenDAGISel.inc"
};
}

void ARMDAGToDAGISel::InstructionSelect(SelectionDAG &DAG) {
  DEBUG(BB->dump());

  SelectRoot();
  DAG.RemoveDeadNodes();
}

bool ARMDAGToDAGISel::SelectAddrMode2(SDValue Op, SDValue N,
                                      SDValue &Base, SDValue &Offset,
                                      SDValue &Opc) {
  if (N.getOpcode() == ISD::MUL) {
    if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      // X * [3,5,9] -> X + X * [2,4,8] etc.
      int RHSC = (int)RHS->getValue();
      if (RHSC & 1) {
        RHSC = RHSC & ~1;
        ARM_AM::AddrOpc AddSub = ARM_AM::add;
        if (RHSC < 0) {
          AddSub = ARM_AM::sub;
          RHSC = - RHSC;
        }
        if (isPowerOf2_32(RHSC)) {
          unsigned ShAmt = Log2_32(RHSC);
          Base = Offset = N.getOperand(0);
          Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, ShAmt,
                                                            ARM_AM::lsl),
                                          MVT::i32);
          return true;
        }
      }
    }
  }

  if (N.getOpcode() != ISD::ADD && N.getOpcode() != ISD::SUB) {
    Base = N;
    if (N.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    } else if (N.getOpcode() == ARMISD::Wrapper) {
      Base = N.getOperand(0);
    }
    Offset = CurDAG->getRegister(0, MVT::i32);
    Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(ARM_AM::add, 0,
                                                      ARM_AM::no_shift),
                                    MVT::i32);
    return true;
  }
  
  // Match simple R +/- imm12 operands.
  if (N.getOpcode() == ISD::ADD)
    if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      int RHSC = (int)RHS->getValue();
      if ((RHSC >= 0 && RHSC < 0x1000) ||
          (RHSC < 0 && RHSC > -0x1000)) { // 12 bits.
        Base = N.getOperand(0);
        if (Base.getOpcode() == ISD::FrameIndex) {
          int FI = cast<FrameIndexSDNode>(Base)->getIndex();
          Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
        }
        Offset = CurDAG->getRegister(0, MVT::i32);

        ARM_AM::AddrOpc AddSub = ARM_AM::add;
        if (RHSC < 0) {
          AddSub = ARM_AM::sub;
          RHSC = - RHSC;
        }
        Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, RHSC,
                                                          ARM_AM::no_shift),
                                        MVT::i32);
        return true;
      }
    }
  
  // Otherwise this is R +/- [possibly shifted] R
  ARM_AM::AddrOpc AddSub = N.getOpcode() == ISD::ADD ? ARM_AM::add:ARM_AM::sub;
  ARM_AM::ShiftOpc ShOpcVal = ARM_AM::getShiftOpcForNode(N.getOperand(1));
  unsigned ShAmt = 0;
  
  Base   = N.getOperand(0);
  Offset = N.getOperand(1);
  
  if (ShOpcVal != ARM_AM::no_shift) {
    // Check to see if the RHS of the shift is a constant, if not, we can't fold
    // it.
    if (ConstantSDNode *Sh =
           dyn_cast<ConstantSDNode>(N.getOperand(1).getOperand(1))) {
      ShAmt = Sh->getValue();
      Offset = N.getOperand(1).getOperand(0);
    } else {
      ShOpcVal = ARM_AM::no_shift;
    }
  }
  
  // Try matching (R shl C) + (R).
  if (N.getOpcode() == ISD::ADD && ShOpcVal == ARM_AM::no_shift) {
    ShOpcVal = ARM_AM::getShiftOpcForNode(N.getOperand(0));
    if (ShOpcVal != ARM_AM::no_shift) {
      // Check to see if the RHS of the shift is a constant, if not, we can't
      // fold it.
      if (ConstantSDNode *Sh =
          dyn_cast<ConstantSDNode>(N.getOperand(0).getOperand(1))) {
        ShAmt = Sh->getValue();
        Offset = N.getOperand(0).getOperand(0);
        Base = N.getOperand(1);
      } else {
        ShOpcVal = ARM_AM::no_shift;
      }
    }
  }
  
  Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, ShAmt, ShOpcVal),
                                  MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectAddrMode2Offset(SDValue Op, SDValue N,
                                            SDValue &Offset, SDValue &Opc) {
  unsigned Opcode = Op.getOpcode();
  ISD::MemIndexedMode AM = (Opcode == ISD::LOAD)
    ? cast<LoadSDNode>(Op)->getAddressingMode()
    : cast<StoreSDNode>(Op)->getAddressingMode();
  ARM_AM::AddrOpc AddSub = (AM == ISD::PRE_INC || AM == ISD::POST_INC)
    ? ARM_AM::add : ARM_AM::sub;
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N)) {
    int Val = (int)C->getValue();
    if (Val >= 0 && Val < 0x1000) { // 12 bits.
      Offset = CurDAG->getRegister(0, MVT::i32);
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, Val,
                                                        ARM_AM::no_shift),
                                      MVT::i32);
      return true;
    }
  }

  Offset = N;
  ARM_AM::ShiftOpc ShOpcVal = ARM_AM::getShiftOpcForNode(N);
  unsigned ShAmt = 0;
  if (ShOpcVal != ARM_AM::no_shift) {
    // Check to see if the RHS of the shift is a constant, if not, we can't fold
    // it.
    if (ConstantSDNode *Sh = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      ShAmt = Sh->getValue();
      Offset = N.getOperand(0);
    } else {
      ShOpcVal = ARM_AM::no_shift;
    }
  }

  Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, ShAmt, ShOpcVal),
                                  MVT::i32);
  return true;
}


bool ARMDAGToDAGISel::SelectAddrMode3(SDValue Op, SDValue N,
                                      SDValue &Base, SDValue &Offset,
                                      SDValue &Opc) {
  if (N.getOpcode() == ISD::SUB) {
    // X - C  is canonicalize to X + -C, no need to handle it here.
    Base = N.getOperand(0);
    Offset = N.getOperand(1);
    Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::sub, 0),MVT::i32);
    return true;
  }
  
  if (N.getOpcode() != ISD::ADD) {
    Base = N;
    if (N.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    }
    Offset = CurDAG->getRegister(0, MVT::i32);
    Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::add, 0),MVT::i32);
    return true;
  }
  
  // If the RHS is +/- imm8, fold into addr mode.
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    int RHSC = (int)RHS->getValue();
    if ((RHSC >= 0 && RHSC < 256) ||
        (RHSC < 0 && RHSC > -256)) { // note -256 itself isn't allowed.
      Base = N.getOperand(0);
      if (Base.getOpcode() == ISD::FrameIndex) {
        int FI = cast<FrameIndexSDNode>(Base)->getIndex();
        Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
      }
      Offset = CurDAG->getRegister(0, MVT::i32);

      ARM_AM::AddrOpc AddSub = ARM_AM::add;
      if (RHSC < 0) {
        AddSub = ARM_AM::sub;
        RHSC = - RHSC;
      }
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(AddSub, RHSC),MVT::i32);
      return true;
    }
  }
  
  Base = N.getOperand(0);
  Offset = N.getOperand(1);
  Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::add, 0), MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectAddrMode3Offset(SDValue Op, SDValue N,
                                            SDValue &Offset, SDValue &Opc) {
  unsigned Opcode = Op.getOpcode();
  ISD::MemIndexedMode AM = (Opcode == ISD::LOAD)
    ? cast<LoadSDNode>(Op)->getAddressingMode()
    : cast<StoreSDNode>(Op)->getAddressingMode();
  ARM_AM::AddrOpc AddSub = (AM == ISD::PRE_INC || AM == ISD::POST_INC)
    ? ARM_AM::add : ARM_AM::sub;
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N)) {
    int Val = (int)C->getValue();
    if (Val >= 0 && Val < 256) {
      Offset = CurDAG->getRegister(0, MVT::i32);
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(AddSub, Val), MVT::i32);
      return true;
    }
  }

  Offset = N;
  Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(AddSub, 0), MVT::i32);
  return true;
}


bool ARMDAGToDAGISel::SelectAddrMode5(SDValue Op, SDValue N,
                                      SDValue &Base, SDValue &Offset) {
  if (N.getOpcode() != ISD::ADD) {
    Base = N;
    if (N.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    } else if (N.getOpcode() == ARMISD::Wrapper) {
      Base = N.getOperand(0);
    }
    Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(ARM_AM::add, 0),
                                       MVT::i32);
    return true;
  }
  
  // If the RHS is +/- imm8, fold into addr mode.
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    int RHSC = (int)RHS->getValue();
    if ((RHSC & 3) == 0) {  // The constant is implicitly multiplied by 4.
      RHSC >>= 2;
      if ((RHSC >= 0 && RHSC < 256) ||
          (RHSC < 0 && RHSC > -256)) { // note -256 itself isn't allowed.
        Base = N.getOperand(0);
        if (Base.getOpcode() == ISD::FrameIndex) {
          int FI = cast<FrameIndexSDNode>(Base)->getIndex();
          Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
        }

        ARM_AM::AddrOpc AddSub = ARM_AM::add;
        if (RHSC < 0) {
          AddSub = ARM_AM::sub;
          RHSC = - RHSC;
        }
        Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(AddSub, RHSC),
                                           MVT::i32);
        return true;
      }
    }
  }
  
  Base = N;
  Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(ARM_AM::add, 0),
                                     MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectAddrModePC(SDValue Op, SDValue N,
                                        SDValue &Offset, SDValue &Label) {
  if (N.getOpcode() == ARMISD::PIC_ADD && N.hasOneUse()) {
    Offset = N.getOperand(0);
    SDValue N1 = N.getOperand(1);
    Label  = CurDAG->getTargetConstant(cast<ConstantSDNode>(N1)->getValue(),
                                       MVT::i32);
    return true;
  }
  return false;
}

bool ARMDAGToDAGISel::SelectThumbAddrModeRR(SDValue Op, SDValue N,
                                            SDValue &Base, SDValue &Offset){
  if (N.getOpcode() != ISD::ADD) {
    Base = N;
    // We must materialize a zero in a reg! Returning an constant here won't
    // work since its node is -1 so it won't get added to the selection queue.
    // Explicitly issue a tMOVri8 node!
    Offset = SDValue(CurDAG->getTargetNode(ARM::tMOVi8, MVT::i32,
                                    CurDAG->getTargetConstant(0, MVT::i32)), 0);
    return true;
  }

  Base = N.getOperand(0);
  Offset = N.getOperand(1);
  return true;
}

bool
ARMDAGToDAGISel::SelectThumbAddrModeRI5(SDValue Op, SDValue N,
                                        unsigned Scale, SDValue &Base,
                                        SDValue &OffImm, SDValue &Offset) {
  if (Scale == 4) {
    SDValue TmpBase, TmpOffImm;
    if (SelectThumbAddrModeSP(Op, N, TmpBase, TmpOffImm))
      return false;  // We want to select tLDRspi / tSTRspi instead.
    if (N.getOpcode() == ARMISD::Wrapper &&
        N.getOperand(0).getOpcode() == ISD::TargetConstantPool)
      return false;  // We want to select tLDRpci instead.
  }

  if (N.getOpcode() != ISD::ADD) {
    Base = (N.getOpcode() == ARMISD::Wrapper) ? N.getOperand(0) : N;
    Offset = CurDAG->getRegister(0, MVT::i32);
    OffImm = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  // Thumb does not have [sp, r] address mode.
  RegisterSDNode *LHSR = dyn_cast<RegisterSDNode>(N.getOperand(0));
  RegisterSDNode *RHSR = dyn_cast<RegisterSDNode>(N.getOperand(1));
  if ((LHSR && LHSR->getReg() == ARM::SP) ||
      (RHSR && RHSR->getReg() == ARM::SP)) {
    Base = N;
    Offset = CurDAG->getRegister(0, MVT::i32);
    OffImm = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  // If the RHS is + imm5 * scale, fold into addr mode.
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    int RHSC = (int)RHS->getValue();
    if ((RHSC & (Scale-1)) == 0) {  // The constant is implicitly multiplied.
      RHSC /= Scale;
      if (RHSC >= 0 && RHSC < 32) {
        Base = N.getOperand(0);
        Offset = CurDAG->getRegister(0, MVT::i32);
        OffImm = CurDAG->getTargetConstant(RHSC, MVT::i32);
        return true;
      }
    }
  }

  Base = N.getOperand(0);
  Offset = N.getOperand(1);
  OffImm = CurDAG->getTargetConstant(0, MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectThumbAddrModeS1(SDValue Op, SDValue N,
                                            SDValue &Base, SDValue &OffImm,
                                            SDValue &Offset) {
  return SelectThumbAddrModeRI5(Op, N, 1, Base, OffImm, Offset);
}

bool ARMDAGToDAGISel::SelectThumbAddrModeS2(SDValue Op, SDValue N,
                                            SDValue &Base, SDValue &OffImm,
                                            SDValue &Offset) {
  return SelectThumbAddrModeRI5(Op, N, 2, Base, OffImm, Offset);
}

bool ARMDAGToDAGISel::SelectThumbAddrModeS4(SDValue Op, SDValue N,
                                            SDValue &Base, SDValue &OffImm,
                                            SDValue &Offset) {
  return SelectThumbAddrModeRI5(Op, N, 4, Base, OffImm, Offset);
}

bool ARMDAGToDAGISel::SelectThumbAddrModeSP(SDValue Op, SDValue N,
                                           SDValue &Base, SDValue &OffImm) {
  if (N.getOpcode() == ISD::FrameIndex) {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    OffImm = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  if (N.getOpcode() != ISD::ADD)
    return false;

  RegisterSDNode *LHSR = dyn_cast<RegisterSDNode>(N.getOperand(0));
  if (N.getOperand(0).getOpcode() == ISD::FrameIndex ||
      (LHSR && LHSR->getReg() == ARM::SP)) {
    // If the RHS is + imm8 * scale, fold into addr mode.
    if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      int RHSC = (int)RHS->getValue();
      if ((RHSC & 3) == 0) {  // The constant is implicitly multiplied.
        RHSC >>= 2;
        if (RHSC >= 0 && RHSC < 256) {
          Base = N.getOperand(0);
          if (Base.getOpcode() == ISD::FrameIndex) {
            int FI = cast<FrameIndexSDNode>(Base)->getIndex();
            Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
          }
          OffImm = CurDAG->getTargetConstant(RHSC, MVT::i32);
          return true;
        }
      }
    }
  }
  
  return false;
}

bool ARMDAGToDAGISel::SelectShifterOperandReg(SDValue Op,
                                              SDValue N, 
                                              SDValue &BaseReg,
                                              SDValue &ShReg,
                                              SDValue &Opc) {
  ARM_AM::ShiftOpc ShOpcVal = ARM_AM::getShiftOpcForNode(N);

  // Don't match base register only case. That is matched to a separate
  // lower complexity pattern with explicit register operand.
  if (ShOpcVal == ARM_AM::no_shift) return false;
  
  BaseReg = N.getOperand(0);
  unsigned ShImmVal = 0;
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    ShReg = CurDAG->getRegister(0, MVT::i32);
    ShImmVal = RHS->getValue() & 31;
  } else {
    ShReg = N.getOperand(1);
  }
  Opc = CurDAG->getTargetConstant(ARM_AM::getSORegOpc(ShOpcVal, ShImmVal),
                                  MVT::i32);
  return true;
}

/// getAL - Returns a ARMCC::AL immediate node.
static inline SDValue getAL(SelectionDAG *CurDAG) {
  return CurDAG->getTargetConstant((uint64_t)ARMCC::AL, MVT::i32);
}


SDNode *ARMDAGToDAGISel::Select(SDValue Op) {
  SDNode *N = Op.Val;

  if (N->isMachineOpcode())
    return NULL;   // Already selected.

  switch (N->getOpcode()) {
  default: break;
  case ISD::Constant: {
    unsigned Val = cast<ConstantSDNode>(N)->getValue();
    bool UseCP = true;
    if (Subtarget->isThumb())
      UseCP = (Val > 255 &&                          // MOV
               ~Val > 255 &&                         // MOV + MVN
               !ARM_AM::isThumbImmShiftedVal(Val));  // MOV + LSL
    else
      UseCP = (ARM_AM::getSOImmVal(Val) == -1 &&     // MOV
               ARM_AM::getSOImmVal(~Val) == -1 &&    // MVN
               !ARM_AM::isSOImmTwoPartVal(Val));     // two instrs.
    if (UseCP) {
      SDValue CPIdx =
        CurDAG->getTargetConstantPool(ConstantInt::get(Type::Int32Ty, Val),
                                      TLI.getPointerTy());

      SDNode *ResNode;
      if (Subtarget->isThumb())
        ResNode = CurDAG->getTargetNode(ARM::tLDRcp, MVT::i32, MVT::Other,
                                        CPIdx, CurDAG->getEntryNode());
      else {
        SDValue Ops[] = {
          CPIdx, 
          CurDAG->getRegister(0, MVT::i32),
          CurDAG->getTargetConstant(0, MVT::i32),
          getAL(CurDAG),
          CurDAG->getRegister(0, MVT::i32),
          CurDAG->getEntryNode()
        };
        ResNode=CurDAG->getTargetNode(ARM::LDRcp, MVT::i32, MVT::Other, Ops, 6);
      }
      ReplaceUses(Op, SDValue(ResNode, 0));
      return NULL;
    }
      
    // Other cases are autogenerated.
    break;
  }
  case ISD::FrameIndex: {
    // Selects to ADDri FI, 0 which in turn will become ADDri SP, imm.
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    if (Subtarget->isThumb())
      return CurDAG->SelectNodeTo(N, ARM::tADDrSPi, MVT::i32, TFI,
                                  CurDAG->getTargetConstant(0, MVT::i32));
    else {
      SDValue Ops[] = { TFI, CurDAG->getTargetConstant(0, MVT::i32),
                          getAL(CurDAG), CurDAG->getRegister(0, MVT::i32),
                          CurDAG->getRegister(0, MVT::i32) };
      return CurDAG->SelectNodeTo(N, ARM::ADDri, MVT::i32, Ops, 5);
    }
  }
  case ISD::ADD: {
    // Select add sp, c to tADDhirr.
    SDValue N0 = Op.getOperand(0);
    SDValue N1 = Op.getOperand(1);
    RegisterSDNode *LHSR = dyn_cast<RegisterSDNode>(Op.getOperand(0));
    RegisterSDNode *RHSR = dyn_cast<RegisterSDNode>(Op.getOperand(1));
    if (LHSR && LHSR->getReg() == ARM::SP) {
      std::swap(N0, N1);
      std::swap(LHSR, RHSR);
    }
    if (RHSR && RHSR->getReg() == ARM::SP) {
      AddToISelQueue(N0);
      AddToISelQueue(N1);
      return CurDAG->SelectNodeTo(N, ARM::tADDhirr, Op.getValueType(), N0, N1);
    }
    break;
  }
  case ISD::MUL:
    if (Subtarget->isThumb())
      break;
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
      unsigned RHSV = C->getValue();
      if (!RHSV) break;
      if (isPowerOf2_32(RHSV-1)) {  // 2^n+1?
        SDValue V = Op.getOperand(0);
        AddToISelQueue(V);
        unsigned ShImm = ARM_AM::getSORegOpc(ARM_AM::lsl, Log2_32(RHSV-1));
        SDValue Ops[] = { V, V, CurDAG->getRegister(0, MVT::i32),
                            CurDAG->getTargetConstant(ShImm, MVT::i32),
                            getAL(CurDAG), CurDAG->getRegister(0, MVT::i32),
                            CurDAG->getRegister(0, MVT::i32) };
        return CurDAG->SelectNodeTo(N, ARM::ADDrs, MVT::i32, Ops, 7);
      }
      if (isPowerOf2_32(RHSV+1)) {  // 2^n-1?
        SDValue V = Op.getOperand(0);
        AddToISelQueue(V);
        unsigned ShImm = ARM_AM::getSORegOpc(ARM_AM::lsl, Log2_32(RHSV+1));
        SDValue Ops[] = { V, V, CurDAG->getRegister(0, MVT::i32),
                            CurDAG->getTargetConstant(ShImm, MVT::i32),
                            getAL(CurDAG), CurDAG->getRegister(0, MVT::i32),
                            CurDAG->getRegister(0, MVT::i32) };
        return CurDAG->SelectNodeTo(N, ARM::RSBrs, MVT::i32, Ops, 7);
      }
    }
    break;
  case ARMISD::FMRRD:
    AddToISelQueue(Op.getOperand(0));
    return CurDAG->getTargetNode(ARM::FMRRD, MVT::i32, MVT::i32,
                                 Op.getOperand(0), getAL(CurDAG),
                                 CurDAG->getRegister(0, MVT::i32));
  case ISD::UMUL_LOHI: {
    AddToISelQueue(Op.getOperand(0));
    AddToISelQueue(Op.getOperand(1));
    SDValue Ops[] = { Op.getOperand(0), Op.getOperand(1),
                        getAL(CurDAG), CurDAG->getRegister(0, MVT::i32),
                        CurDAG->getRegister(0, MVT::i32) };
    return CurDAG->getTargetNode(ARM::UMULL, MVT::i32, MVT::i32, Ops, 5);
  }
  case ISD::SMUL_LOHI: {
    AddToISelQueue(Op.getOperand(0));
    AddToISelQueue(Op.getOperand(1));
    SDValue Ops[] = { Op.getOperand(0), Op.getOperand(1),
                        getAL(CurDAG), CurDAG->getRegister(0, MVT::i32),
                        CurDAG->getRegister(0, MVT::i32) };
    return CurDAG->getTargetNode(ARM::SMULL, MVT::i32, MVT::i32, Ops, 5);
  }
  case ISD::LOAD: {
    LoadSDNode *LD = cast<LoadSDNode>(Op);
    ISD::MemIndexedMode AM = LD->getAddressingMode();
    MVT LoadedVT = LD->getMemoryVT();
    if (AM != ISD::UNINDEXED) {
      SDValue Offset, AMOpc;
      bool isPre = (AM == ISD::PRE_INC) || (AM == ISD::PRE_DEC);
      unsigned Opcode = 0;
      bool Match = false;
      if (LoadedVT == MVT::i32 &&
          SelectAddrMode2Offset(Op, LD->getOffset(), Offset, AMOpc)) {
        Opcode = isPre ? ARM::LDR_PRE : ARM::LDR_POST;
        Match = true;
      } else if (LoadedVT == MVT::i16 &&
                 SelectAddrMode3Offset(Op, LD->getOffset(), Offset, AMOpc)) {
        Match = true;
        Opcode = (LD->getExtensionType() == ISD::SEXTLOAD)
          ? (isPre ? ARM::LDRSH_PRE : ARM::LDRSH_POST)
          : (isPre ? ARM::LDRH_PRE : ARM::LDRH_POST);
      } else if (LoadedVT == MVT::i8 || LoadedVT == MVT::i1) {
        if (LD->getExtensionType() == ISD::SEXTLOAD) {
          if (SelectAddrMode3Offset(Op, LD->getOffset(), Offset, AMOpc)) {
            Match = true;
            Opcode = isPre ? ARM::LDRSB_PRE : ARM::LDRSB_POST;
          }
        } else {
          if (SelectAddrMode2Offset(Op, LD->getOffset(), Offset, AMOpc)) {
            Match = true;
            Opcode = isPre ? ARM::LDRB_PRE : ARM::LDRB_POST;
          }
        }
      }

      if (Match) {
        SDValue Chain = LD->getChain();
        SDValue Base = LD->getBasePtr();
        AddToISelQueue(Chain);
        AddToISelQueue(Base);
        AddToISelQueue(Offset);
        SDValue Ops[]= { Base, Offset, AMOpc, getAL(CurDAG),
                           CurDAG->getRegister(0, MVT::i32), Chain };
        return CurDAG->getTargetNode(Opcode, MVT::i32, MVT::i32,
                                     MVT::Other, Ops, 6);
      }
    }
    // Other cases are autogenerated.
    break;
  }
  case ARMISD::BRCOND: {
    // Pattern: (ARMbrcond:void (bb:Other):$dst, (imm:i32):$cc)
    // Emits: (Bcc:void (bb:Other):$dst, (imm:i32):$cc)
    // Pattern complexity = 6  cost = 1  size = 0

    // Pattern: (ARMbrcond:void (bb:Other):$dst, (imm:i32):$cc)
    // Emits: (tBcc:void (bb:Other):$dst, (imm:i32):$cc)
    // Pattern complexity = 6  cost = 1  size = 0

    unsigned Opc = Subtarget->isThumb() ? ARM::tBcc : ARM::Bcc;
    SDValue Chain = Op.getOperand(0);
    SDValue N1 = Op.getOperand(1);
    SDValue N2 = Op.getOperand(2);
    SDValue N3 = Op.getOperand(3);
    SDValue InFlag = Op.getOperand(4);
    assert(N1.getOpcode() == ISD::BasicBlock);
    assert(N2.getOpcode() == ISD::Constant);
    assert(N3.getOpcode() == ISD::Register);

    AddToISelQueue(Chain);
    AddToISelQueue(N1);
    AddToISelQueue(InFlag);
    SDValue Tmp2 = CurDAG->getTargetConstant(((unsigned)
                               cast<ConstantSDNode>(N2)->getValue()), MVT::i32);
    SDValue Ops[] = { N1, Tmp2, N3, Chain, InFlag };
    SDNode *ResNode = CurDAG->getTargetNode(Opc, MVT::Other, MVT::Flag, Ops, 5);
    Chain = SDValue(ResNode, 0);
    if (Op.Val->getNumValues() == 2) {
      InFlag = SDValue(ResNode, 1);
      ReplaceUses(SDValue(Op.Val, 1), InFlag);
    }
    ReplaceUses(SDValue(Op.Val, 0), SDValue(Chain.Val, Chain.ResNo));
    return NULL;
  }
  case ARMISD::CMOV: {
    bool isThumb = Subtarget->isThumb();
    MVT VT = Op.getValueType();
    SDValue N0 = Op.getOperand(0);
    SDValue N1 = Op.getOperand(1);
    SDValue N2 = Op.getOperand(2);
    SDValue N3 = Op.getOperand(3);
    SDValue InFlag = Op.getOperand(4);
    assert(N2.getOpcode() == ISD::Constant);
    assert(N3.getOpcode() == ISD::Register);

    // Pattern: (ARMcmov:i32 GPR:i32:$false, so_reg:i32:$true, (imm:i32):$cc)
    // Emits: (MOVCCs:i32 GPR:i32:$false, so_reg:i32:$true, (imm:i32):$cc)
    // Pattern complexity = 18  cost = 1  size = 0
    SDValue CPTmp0;
    SDValue CPTmp1;
    SDValue CPTmp2;
    if (!isThumb && VT == MVT::i32 &&
        SelectShifterOperandReg(Op, N1, CPTmp0, CPTmp1, CPTmp2)) {
      AddToISelQueue(N0);
      AddToISelQueue(CPTmp0);
      AddToISelQueue(CPTmp1);
      AddToISelQueue(CPTmp2);
      AddToISelQueue(InFlag);
      SDValue Tmp2 = CurDAG->getTargetConstant(((unsigned)
                               cast<ConstantSDNode>(N2)->getValue()), MVT::i32);
      SDValue Ops[] = { N0, CPTmp0, CPTmp1, CPTmp2, Tmp2, N3, InFlag };
      return CurDAG->SelectNodeTo(Op.Val, ARM::MOVCCs, MVT::i32, Ops, 7);
    }

    // Pattern: (ARMcmov:i32 GPR:i32:$false,
    //             (imm:i32)<<P:Predicate_so_imm>><<X:so_imm_XFORM>>:$true,
    //             (imm:i32):$cc)
    // Emits: (MOVCCi:i32 GPR:i32:$false,
    //           (so_imm_XFORM:i32 (imm:i32):$true), (imm:i32):$cc)
    // Pattern complexity = 10  cost = 1  size = 0
    if (VT == MVT::i32 &&
        N3.getOpcode() == ISD::Constant &&
        Predicate_so_imm(N3.Val)) {
      AddToISelQueue(N0);
      AddToISelQueue(InFlag);
      SDValue Tmp1 = CurDAG->getTargetConstant(((unsigned)
                               cast<ConstantSDNode>(N1)->getValue()), MVT::i32);
      Tmp1 = Transform_so_imm_XFORM(Tmp1.Val);
      SDValue Tmp2 = CurDAG->getTargetConstant(((unsigned)
                               cast<ConstantSDNode>(N2)->getValue()), MVT::i32);
      SDValue Ops[] = { N0, Tmp1, Tmp2, N3, InFlag };
      return CurDAG->SelectNodeTo(Op.Val, ARM::MOVCCi, MVT::i32, Ops, 5);
    }

    // Pattern: (ARMcmov:i32 GPR:i32:$false, GPR:i32:$true, (imm:i32):$cc)
    // Emits: (MOVCCr:i32 GPR:i32:$false, GPR:i32:$true, (imm:i32):$cc)
    // Pattern complexity = 6  cost = 1  size = 0
    //
    // Pattern: (ARMcmov:i32 GPR:i32:$false, GPR:i32:$true, (imm:i32):$cc)
    // Emits: (tMOVCCr:i32 GPR:i32:$false, GPR:i32:$true, (imm:i32):$cc)
    // Pattern complexity = 6  cost = 11  size = 0
    //
    // Also FCPYScc and FCPYDcc.
    AddToISelQueue(N0);
    AddToISelQueue(N1);
    AddToISelQueue(InFlag);
    SDValue Tmp2 = CurDAG->getTargetConstant(((unsigned)
                               cast<ConstantSDNode>(N2)->getValue()), MVT::i32);
    SDValue Ops[] = { N0, N1, Tmp2, N3, InFlag };
    unsigned Opc = 0;
    switch (VT.getSimpleVT()) {
    default: assert(false && "Illegal conditional move type!");
      break;
    case MVT::i32:
      Opc = isThumb ? ARM::tMOVCCr : ARM::MOVCCr;
      break;
    case MVT::f32:
      Opc = ARM::FCPYScc;
      break;
    case MVT::f64:
      Opc = ARM::FCPYDcc;
      break; 
    }
    return CurDAG->SelectNodeTo(Op.Val, Opc, VT, Ops, 5);
  }
  case ARMISD::CNEG: {
    MVT VT = Op.getValueType();
    SDValue N0 = Op.getOperand(0);
    SDValue N1 = Op.getOperand(1);
    SDValue N2 = Op.getOperand(2);
    SDValue N3 = Op.getOperand(3);
    SDValue InFlag = Op.getOperand(4);
    assert(N2.getOpcode() == ISD::Constant);
    assert(N3.getOpcode() == ISD::Register);

    AddToISelQueue(N0);
    AddToISelQueue(N1);
    AddToISelQueue(InFlag);
    SDValue Tmp2 = CurDAG->getTargetConstant(((unsigned)
                               cast<ConstantSDNode>(N2)->getValue()), MVT::i32);
    SDValue Ops[] = { N0, N1, Tmp2, N3, InFlag };
    unsigned Opc = 0;
    switch (VT.getSimpleVT()) {
    default: assert(false && "Illegal conditional move type!");
      break;
    case MVT::f32:
      Opc = ARM::FNEGScc;
      break;
    case MVT::f64:
      Opc = ARM::FNEGDcc;
      break; 
    }
    return CurDAG->SelectNodeTo(Op.Val, Opc, VT, Ops, 5);
  }
  }
  return SelectCode(Op);
}

/// createARMISelDag - This pass converts a legalized DAG into a
/// ARM-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createARMISelDag(ARMTargetMachine &TM) {
  return new ARMDAGToDAGISel(TM);
}
