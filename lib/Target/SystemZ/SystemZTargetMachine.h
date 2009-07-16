//==- SystemZTargetMachine.h - Define TargetMachine for SystemZ ---*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the SystemZ specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//


#ifndef LLVM_TARGET_SYSTEMZ_TARGETMACHINE_H
#define LLVM_TARGET_SYSTEMZ_TARGETMACHINE_H

#include "SystemZInstrInfo.h"
#include "SystemZISelLowering.h"
#include "SystemZRegisterInfo.h"
#include "SystemZSubtarget.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {

/// SystemZTargetMachine
///
class SystemZTargetMachine : public LLVMTargetMachine {
  SystemZSubtarget        Subtarget;
  const TargetData        DataLayout;       // Calculates type size & alignment
  SystemZInstrInfo        InstrInfo;
  SystemZTargetLowering   TLInfo;

  // SystemZ does not have any call stack frame, therefore not having
  // any SystemZ specific FrameInfo class.
  TargetFrameInfo       FrameInfo;

protected:
  virtual const TargetAsmInfo *createTargetAsmInfo() const;

public:
  SystemZTargetMachine(const Module &M, const std::string &FS);

  virtual const TargetFrameInfo *getFrameInfo() const { return &FrameInfo; }
  virtual const SystemZInstrInfo *getInstrInfo() const  { return &InstrInfo; }
  virtual const TargetData *getTargetData() const     { return &DataLayout;}
  virtual const SystemZSubtarget *getSubtargetImpl() const { return &Subtarget; }

  virtual const SystemZRegisterInfo *getRegisterInfo() const {
    return &InstrInfo.getRegisterInfo();
  }

  virtual SystemZTargetLowering *getTargetLowering() const {
    return const_cast<SystemZTargetLowering*>(&TLInfo);
  }

  virtual bool addInstSelector(PassManagerBase &PM, CodeGenOpt::Level OptLevel);
  virtual bool addAssemblyEmitter(PassManagerBase &PM,
                                  CodeGenOpt::Level OptLevel, bool Verbose,
                                  raw_ostream &Out);
  static unsigned getModuleMatchQuality(const Module &M);
}; // SystemZTargetMachine.

} // end namespace llvm

#endif // LLVM_TARGET_SystemZ_TARGETMACHINE_H
