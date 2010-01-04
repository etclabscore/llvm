//===- InstCombineCasts.cpp -----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the visit functions for cast operations.
//
//===----------------------------------------------------------------------===//

#include "InstCombine.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/PatternMatch.h"
using namespace llvm;
using namespace PatternMatch;

/// CanEvaluateInDifferentType - Return true if we can take the specified value
/// and return it as type Ty without inserting any new casts and without
/// changing the computed value.  This is used by code that tries to decide
/// whether promoting or shrinking integer operations to wider or smaller types
/// will allow us to eliminate a truncate or extend.
///
/// This is a truncation operation if Ty is smaller than V->getType(), or an
/// extension operation if Ty is larger.
///
/// If CastOpc is a truncation, then Ty will be a type smaller than V.  We
/// should return true if trunc(V) can be computed by computing V in the smaller
/// type.  If V is an instruction, then trunc(inst(x,y)) can be computed as
/// inst(trunc(x),trunc(y)), which only makes sense if x and y can be
/// efficiently truncated.
///
/// If CastOpc is a sext or zext, we are asking if the low bits of the value can
/// bit computed in a larger type, which is then and'd or sext_in_reg'd to get
/// the final result.
bool InstCombiner::CanEvaluateInDifferentType(Value *V, const Type *Ty,
                                              unsigned CastOpc,
                                              int &NumCastsRemoved){
  // We can always evaluate constants in another type.
  if (isa<Constant>(V))
    return true;
  
  Instruction *I = dyn_cast<Instruction>(V);
  if (!I) return false;
  
  const Type *OrigTy = V->getType();
  
  // If this is an extension or truncate, we can often eliminate it.
  if (isa<TruncInst>(I) || isa<ZExtInst>(I) || isa<SExtInst>(I)) {
    // If this is a cast from the destination type, we can trivially eliminate
    // it, and this will remove a cast overall.
    if (I->getOperand(0)->getType() == Ty) {
      // If the first operand is itself a cast, and is eliminable, do not count
      // this as an eliminable cast.  We would prefer to eliminate those two
      // casts first.
      if (!isa<CastInst>(I->getOperand(0)) && I->hasOneUse())
        ++NumCastsRemoved;
      return true;
    }
  }

  // We can't extend or shrink something that has multiple uses: doing so would
  // require duplicating the instruction in general, which isn't profitable.
  if (!I->hasOneUse()) return false;

  unsigned Opc = I->getOpcode();
  switch (Opc) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    // These operators can all arbitrarily be extended or truncated.
    return CanEvaluateInDifferentType(I->getOperand(0), Ty, CastOpc,
                                      NumCastsRemoved) &&
           CanEvaluateInDifferentType(I->getOperand(1), Ty, CastOpc,
                                      NumCastsRemoved);

  case Instruction::UDiv:
  case Instruction::URem: {
    // UDiv and URem can be truncated if all the truncated bits are zero.
    uint32_t OrigBitWidth = OrigTy->getScalarSizeInBits();
    uint32_t BitWidth = Ty->getScalarSizeInBits();
    if (BitWidth < OrigBitWidth) {
      APInt Mask = APInt::getHighBitsSet(OrigBitWidth, OrigBitWidth-BitWidth);
      if (MaskedValueIsZero(I->getOperand(0), Mask) &&
          MaskedValueIsZero(I->getOperand(1), Mask)) {
        return CanEvaluateInDifferentType(I->getOperand(0), Ty, CastOpc,
                                          NumCastsRemoved) &&
               CanEvaluateInDifferentType(I->getOperand(1), Ty, CastOpc,
                                          NumCastsRemoved);
      }
    }
    break;
  }
  case Instruction::Shl:
    // If we are truncating the result of this SHL, and if it's a shift of a
    // constant amount, we can always perform a SHL in a smaller type.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(I->getOperand(1))) {
      uint32_t BitWidth = Ty->getScalarSizeInBits();
      if (BitWidth < OrigTy->getScalarSizeInBits() &&
          CI->getLimitedValue(BitWidth) < BitWidth)
        return CanEvaluateInDifferentType(I->getOperand(0), Ty, CastOpc,
                                          NumCastsRemoved);
    }
    break;
  case Instruction::LShr:
    // If this is a truncate of a logical shr, we can truncate it to a smaller
    // lshr iff we know that the bits we would otherwise be shifting in are
    // already zeros.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(I->getOperand(1))) {
      uint32_t OrigBitWidth = OrigTy->getScalarSizeInBits();
      uint32_t BitWidth = Ty->getScalarSizeInBits();
      if (BitWidth < OrigBitWidth &&
          MaskedValueIsZero(I->getOperand(0),
            APInt::getHighBitsSet(OrigBitWidth, OrigBitWidth-BitWidth)) &&
          CI->getLimitedValue(BitWidth) < BitWidth) {
        return CanEvaluateInDifferentType(I->getOperand(0), Ty, CastOpc,
                                          NumCastsRemoved);
      }
    }
    break;
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
    // If this is the same kind of case as our original (e.g. zext+zext), we
    // can safely replace it.  Note that replacing it does not reduce the number
    // of casts in the input.
    if (Opc == CastOpc)
      return true;

    // sext (zext ty1), ty2 -> zext ty2
    if (CastOpc == Instruction::SExt && Opc == Instruction::ZExt)
      return true;
    break;
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(I);
    return CanEvaluateInDifferentType(SI->getTrueValue(), Ty, CastOpc,
                                      NumCastsRemoved) &&
           CanEvaluateInDifferentType(SI->getFalseValue(), Ty, CastOpc,
                                      NumCastsRemoved);
  }
  case Instruction::PHI: {
    // We can change a phi if we can change all operands.
    PHINode *PN = cast<PHINode>(I);
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
      if (!CanEvaluateInDifferentType(PN->getIncomingValue(i), Ty, CastOpc,
                                      NumCastsRemoved))
        return false;
    return true;
  }
  default:
    // TODO: Can handle more cases here.
    break;
  }
  
  return false;
}

/// EvaluateInDifferentType - Given an expression that 
/// CanEvaluateInDifferentType returns true for, actually insert the code to
/// evaluate the expression.
Value *InstCombiner::EvaluateInDifferentType(Value *V, const Type *Ty, 
                                             bool isSigned) {
  if (Constant *C = dyn_cast<Constant>(V))
    return ConstantExpr::getIntegerCast(C, Ty, isSigned /*Sext or ZExt*/);

  // Otherwise, it must be an instruction.
  Instruction *I = cast<Instruction>(V);
  Instruction *Res = 0;
  unsigned Opc = I->getOpcode();
  switch (Opc) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::AShr:
  case Instruction::LShr:
  case Instruction::Shl:
  case Instruction::UDiv:
  case Instruction::URem: {
    Value *LHS = EvaluateInDifferentType(I->getOperand(0), Ty, isSigned);
    Value *RHS = EvaluateInDifferentType(I->getOperand(1), Ty, isSigned);
    Res = BinaryOperator::Create((Instruction::BinaryOps)Opc, LHS, RHS);
    break;
  }    
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
    // If the source type of the cast is the type we're trying for then we can
    // just return the source.  There's no need to insert it because it is not
    // new.
    if (I->getOperand(0)->getType() == Ty)
      return I->getOperand(0);
    
    // Otherwise, must be the same type of cast, so just reinsert a new one.
    Res = CastInst::Create(cast<CastInst>(I)->getOpcode(), I->getOperand(0),Ty);
    break;
  case Instruction::Select: {
    Value *True = EvaluateInDifferentType(I->getOperand(1), Ty, isSigned);
    Value *False = EvaluateInDifferentType(I->getOperand(2), Ty, isSigned);
    Res = SelectInst::Create(I->getOperand(0), True, False);
    break;
  }
  case Instruction::PHI: {
    PHINode *OPN = cast<PHINode>(I);
    PHINode *NPN = PHINode::Create(Ty);
    for (unsigned i = 0, e = OPN->getNumIncomingValues(); i != e; ++i) {
      Value *V =EvaluateInDifferentType(OPN->getIncomingValue(i), Ty, isSigned);
      NPN->addIncoming(V, OPN->getIncomingBlock(i));
    }
    Res = NPN;
    break;
  }
  default: 
    // TODO: Can handle more cases here.
    llvm_unreachable("Unreachable!");
    break;
  }
  
  Res->takeName(I);
  return InsertNewInstBefore(Res, *I);
}


/// This function is a wrapper around CastInst::isEliminableCastPair. It
/// simply extracts arguments and returns what that function returns.
static Instruction::CastOps 
isEliminableCastPair(
  const CastInst *CI, ///< The first cast instruction
  unsigned opcode,       ///< The opcode of the second cast instruction
  const Type *DstTy,     ///< The target type for the second cast instruction
  TargetData *TD         ///< The target data for pointer size
) {

  const Type *SrcTy = CI->getOperand(0)->getType();   // A from above
  const Type *MidTy = CI->getType();                  // B from above

  // Get the opcodes of the two Cast instructions
  Instruction::CastOps firstOp = Instruction::CastOps(CI->getOpcode());
  Instruction::CastOps secondOp = Instruction::CastOps(opcode);

  unsigned Res = CastInst::isEliminableCastPair(firstOp, secondOp, SrcTy, MidTy,
                                                DstTy,
                                  TD ? TD->getIntPtrType(CI->getContext()) : 0);
  
  // We don't want to form an inttoptr or ptrtoint that converts to an integer
  // type that differs from the pointer size.
  if ((Res == Instruction::IntToPtr &&
          (!TD || SrcTy != TD->getIntPtrType(CI->getContext()))) ||
      (Res == Instruction::PtrToInt &&
          (!TD || DstTy != TD->getIntPtrType(CI->getContext()))))
    Res = 0;
  
  return Instruction::CastOps(Res);
}

/// ValueRequiresCast - Return true if the cast from "V to Ty" actually results
/// in any code being generated.  It does not require codegen if V is simple
/// enough or if the cast can be folded into other casts.
bool InstCombiner::ValueRequiresCast(Instruction::CastOps opcode,const Value *V,
                                     const Type *Ty) {
  if (V->getType() == Ty || isa<Constant>(V)) return false;
  
  // If this is another cast that can be eliminated, it isn't codegen either.
  if (const CastInst *CI = dyn_cast<CastInst>(V))
    if (isEliminableCastPair(CI, opcode, Ty, TD))
      return false;
  return true;
}


/// @brief Implement the transforms common to all CastInst visitors.
Instruction *InstCombiner::commonCastTransforms(CastInst &CI) {
  Value *Src = CI.getOperand(0);

  // Many cases of "cast of a cast" are eliminable. If it's eliminable we just
  // eliminate it now.
  if (CastInst *CSrc = dyn_cast<CastInst>(Src)) {   // A->B->C cast
    if (Instruction::CastOps opc = 
        isEliminableCastPair(CSrc, CI.getOpcode(), CI.getType(), TD)) {
      // The first cast (CSrc) is eliminable so we need to fix up or replace
      // the second cast (CI). CSrc will then have a good chance of being dead.
      return CastInst::Create(opc, CSrc->getOperand(0), CI.getType());
    }
  }

  // If we are casting a select then fold the cast into the select
  if (SelectInst *SI = dyn_cast<SelectInst>(Src))
    if (Instruction *NV = FoldOpIntoSelect(CI, SI))
      return NV;

  // If we are casting a PHI then fold the cast into the PHI
  if (isa<PHINode>(Src)) {
    // We don't do this if this would create a PHI node with an illegal type if
    // it is currently legal.
    if (!isa<IntegerType>(Src->getType()) ||
        !isa<IntegerType>(CI.getType()) ||
        ShouldChangeType(CI.getType(), Src->getType()))
      if (Instruction *NV = FoldOpIntoPhi(CI))
        return NV;
  }
  
  return 0;
}

/// @brief Implement the transforms for cast of pointer (bitcast/ptrtoint)
Instruction *InstCombiner::commonPointerCastTransforms(CastInst &CI) {
  Value *Src = CI.getOperand(0);
  
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Src)) {
    // If casting the result of a getelementptr instruction with no offset, turn
    // this into a cast of the original pointer!
    if (GEP->hasAllZeroIndices()) {
      // Changing the cast operand is usually not a good idea but it is safe
      // here because the pointer operand is being replaced with another 
      // pointer operand so the opcode doesn't need to change.
      Worklist.Add(GEP);
      CI.setOperand(0, GEP->getOperand(0));
      return &CI;
    }
    
    // If the GEP has a single use, and the base pointer is a bitcast, and the
    // GEP computes a constant offset, see if we can convert these three
    // instructions into fewer.  This typically happens with unions and other
    // non-type-safe code.
    if (TD && GEP->hasOneUse() && isa<BitCastInst>(GEP->getOperand(0))) {
      if (GEP->hasAllConstantIndices()) {
        // We are guaranteed to get a constant from EmitGEPOffset.
        ConstantInt *OffsetV = cast<ConstantInt>(EmitGEPOffset(GEP));
        int64_t Offset = OffsetV->getSExtValue();
        
        // Get the base pointer input of the bitcast, and the type it points to.
        Value *OrigBase = cast<BitCastInst>(GEP->getOperand(0))->getOperand(0);
        const Type *GEPIdxTy =
          cast<PointerType>(OrigBase->getType())->getElementType();
        SmallVector<Value*, 8> NewIndices;
        if (FindElementAtOffset(GEPIdxTy, Offset, NewIndices)) {
          // If we were able to index down into an element, create the GEP
          // and bitcast the result.  This eliminates one bitcast, potentially
          // two.
          Value *NGEP = cast<GEPOperator>(GEP)->isInBounds() ?
            Builder->CreateInBoundsGEP(OrigBase,
                                       NewIndices.begin(), NewIndices.end()) :
            Builder->CreateGEP(OrigBase, NewIndices.begin(), NewIndices.end());
          NGEP->takeName(GEP);
          
          if (isa<BitCastInst>(CI))
            return new BitCastInst(NGEP, CI.getType());
          assert(isa<PtrToIntInst>(CI));
          return new PtrToIntInst(NGEP, CI.getType());
        }
      }      
    }
  }
    
  return commonCastTransforms(CI);
}

/// commonIntCastTransforms - This function implements the common transforms
/// for trunc, zext, and sext.
Instruction *InstCombiner::commonIntCastTransforms(CastInst &CI) {
  if (Instruction *Result = commonCastTransforms(CI))
    return Result;

  Value *Src = CI.getOperand(0);
  const Type *SrcTy = Src->getType();
  const Type *DestTy = CI.getType();
  uint32_t SrcBitSize = SrcTy->getScalarSizeInBits();
  uint32_t DestBitSize = DestTy->getScalarSizeInBits();

  // See if we can simplify any instructions used by the LHS whose sole 
  // purpose is to compute bits we don't care about.
  if (SimplifyDemandedInstructionBits(CI))
    return &CI;

  // If the source isn't an instruction or has more than one use then we
  // can't do anything more. 
  Instruction *SrcI = dyn_cast<Instruction>(Src);
  if (!SrcI || !Src->hasOneUse())
    return 0;

  // Attempt to propagate the cast into the instruction for int->int casts.
  int NumCastsRemoved = 0;
  // Only do this if the dest type is a simple type, don't convert the
  // expression tree to something weird like i93 unless the source is also
  // strange.
  if ((isa<VectorType>(DestTy) ||
       ShouldChangeType(SrcI->getType(), DestTy)) &&
      CanEvaluateInDifferentType(SrcI, DestTy,
                                 CI.getOpcode(), NumCastsRemoved)) {
    // If this cast is a truncate, evaluting in a different type always
    // eliminates the cast, so it is always a win.  If this is a zero-extension,
    // we need to do an AND to maintain the clear top-part of the computation,
    // so we require that the input have eliminated at least one cast.  If this
    // is a sign extension, we insert two new casts (to do the extension) so we
    // require that two casts have been eliminated.
    bool DoXForm = false;
    bool JustReplace = false;
    switch (CI.getOpcode()) {
    default:
      // All the others use floating point so we shouldn't actually 
      // get here because of the check above.
      llvm_unreachable("Unknown cast type");
    case Instruction::Trunc:
      DoXForm = true;
      break;
    case Instruction::ZExt: {
      DoXForm = NumCastsRemoved >= 1;
      
      if (!DoXForm && 0) {
        // If it's unnecessary to issue an AND to clear the high bits, it's
        // always profitable to do this xform.
        Value *TryRes = EvaluateInDifferentType(SrcI, DestTy, false);
        APInt Mask(APInt::getBitsSet(DestBitSize, SrcBitSize, DestBitSize));
        if (MaskedValueIsZero(TryRes, Mask))
          return ReplaceInstUsesWith(CI, TryRes);
        
        if (Instruction *TryI = dyn_cast<Instruction>(TryRes))
          if (TryI->use_empty())
            EraseInstFromFunction(*TryI);
      }
      break;
    }
    case Instruction::SExt: {
      DoXForm = NumCastsRemoved >= 2;
      if (!DoXForm && !isa<TruncInst>(SrcI) && 0) {
        // If we do not have to emit the truncate + sext pair, then it's always
        // profitable to do this xform.
        //
        // It's not safe to eliminate the trunc + sext pair if one of the
        // eliminated cast is a truncate. e.g.
        // t2 = trunc i32 t1 to i16
        // t3 = sext i16 t2 to i32
        // !=
        // i32 t1
        Value *TryRes = EvaluateInDifferentType(SrcI, DestTy, true);
        unsigned NumSignBits = ComputeNumSignBits(TryRes);
        if (NumSignBits > (DestBitSize - SrcBitSize))
          return ReplaceInstUsesWith(CI, TryRes);
        
        if (Instruction *TryI = dyn_cast<Instruction>(TryRes))
          if (TryI->use_empty())
            EraseInstFromFunction(*TryI);
      }
      break;
    }
    }
    
    if (DoXForm) {
      DEBUG(errs() << "ICE: EvaluateInDifferentType converting expression type"
            " to avoid cast: " << CI);
      Value *Res = EvaluateInDifferentType(SrcI, DestTy, 
                                           CI.getOpcode() == Instruction::SExt);
      if (JustReplace)
        // Just replace this cast with the result.
        return ReplaceInstUsesWith(CI, Res);

      assert(Res->getType() == DestTy);
      switch (CI.getOpcode()) {
      default: llvm_unreachable("Unknown cast type!");
      case Instruction::Trunc:
        // Just replace this cast with the result.
        return ReplaceInstUsesWith(CI, Res);
      case Instruction::ZExt: {
        assert(SrcBitSize < DestBitSize && "Not a zext?");

        // If the high bits are already zero, just replace this cast with the
        // result.
        APInt Mask(APInt::getBitsSet(DestBitSize, SrcBitSize, DestBitSize));
        if (MaskedValueIsZero(Res, Mask))
          return ReplaceInstUsesWith(CI, Res);

        // We need to emit an AND to clear the high bits.
        Constant *C = ConstantInt::get(CI.getContext(), 
                                 APInt::getLowBitsSet(DestBitSize, SrcBitSize));
        return BinaryOperator::CreateAnd(Res, C);
      }
      case Instruction::SExt: {
        // If the high bits are already filled with sign bit, just replace this
        // cast with the result.
        unsigned NumSignBits = ComputeNumSignBits(Res);
        if (NumSignBits > (DestBitSize - SrcBitSize))
          return ReplaceInstUsesWith(CI, Res);

        // We need to emit a cast to truncate, then a cast to sext.
        return new SExtInst(Builder->CreateTrunc(Res, Src->getType()), DestTy);
      }
      }
    }
  }
  
  Value *Op0 = SrcI->getNumOperands() > 0 ? SrcI->getOperand(0) : 0;
  Value *Op1 = SrcI->getNumOperands() > 1 ? SrcI->getOperand(1) : 0;

  switch (SrcI->getOpcode()) {
  case Instruction::Add:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    // If we are discarding information, rewrite.
    if (DestBitSize < SrcBitSize && DestBitSize != 1) {
      // Don't insert two casts unless at least one can be eliminated.
      if (!ValueRequiresCast(CI.getOpcode(), Op1, DestTy) ||
          !ValueRequiresCast(CI.getOpcode(), Op0, DestTy)) {
        Value *Op0c = Builder->CreateTrunc(Op0, DestTy, Op0->getName());
        Value *Op1c = Builder->CreateTrunc(Op1, DestTy, Op1->getName());
        return BinaryOperator::Create(
            cast<BinaryOperator>(SrcI)->getOpcode(), Op0c, Op1c);
      }
    }

    // cast (xor bool X, true) to int  --> xor (cast bool X to int), 1
    if (isa<ZExtInst>(CI) && SrcBitSize == 1 && 
        SrcI->getOpcode() == Instruction::Xor &&
        Op1 == ConstantInt::getTrue(CI.getContext()) &&
        (!Op0->hasOneUse() || !isa<CmpInst>(Op0))) {
      Value *New = Builder->CreateZExt(Op0, DestTy, Op0->getName());
      return BinaryOperator::CreateXor(New,
                                      ConstantInt::get(CI.getType(), 1));
    }
    break;

  case Instruction::Shl: {
    // Canonicalize trunc inside shl, if we can.
    ConstantInt *CI = dyn_cast<ConstantInt>(Op1);
    if (CI && DestBitSize < SrcBitSize &&
        CI->getLimitedValue(DestBitSize) < DestBitSize) {
      Value *Op0c = Builder->CreateTrunc(Op0, DestTy, Op0->getName());
      Value *Op1c = Builder->CreateTrunc(Op1, DestTy, Op1->getName());
      return BinaryOperator::CreateShl(Op0c, Op1c);
    }
    break;
  }
  }
  return 0;
}


Instruction *InstCombiner::visitTrunc(TruncInst &CI) {
  if (Instruction *Result = commonIntCastTransforms(CI))
    return Result;
  
  Value *Src = CI.getOperand(0);
  const Type *Ty = CI.getType();
  uint32_t DestBitWidth = Ty->getScalarSizeInBits();
  uint32_t SrcBitWidth = Src->getType()->getScalarSizeInBits();

  // Canonicalize trunc x to i1 -> (icmp ne (and x, 1), 0)
  if (DestBitWidth == 1) {
    Constant *One = ConstantInt::get(Src->getType(), 1);
    Src = Builder->CreateAnd(Src, One, "tmp");
    Value *Zero = Constant::getNullValue(Src->getType());
    return new ICmpInst(ICmpInst::ICMP_NE, Src, Zero);
  }

  // Optimize trunc(lshr(), c) to pull the shift through the truncate.
  ConstantInt *ShAmtV = 0;
  Value *ShiftOp = 0;
  if (Src->hasOneUse() &&
      match(Src, m_LShr(m_Value(ShiftOp), m_ConstantInt(ShAmtV)))) {
    uint32_t ShAmt = ShAmtV->getLimitedValue(SrcBitWidth);
    
    // Get a mask for the bits shifting in.
    APInt Mask(APInt::getLowBitsSet(SrcBitWidth, ShAmt).shl(DestBitWidth));
    if (MaskedValueIsZero(ShiftOp, Mask)) {
      if (ShAmt >= DestBitWidth)        // All zeros.
        return ReplaceInstUsesWith(CI, Constant::getNullValue(Ty));
      
      // Okay, we can shrink this.  Truncate the input, then return a new
      // shift.
      Value *V1 = Builder->CreateTrunc(ShiftOp, Ty, ShiftOp->getName());
      Value *V2 = ConstantExpr::getTrunc(ShAmtV, Ty);
      return BinaryOperator::CreateLShr(V1, V2);
    }
  }
 
  return 0;
}

/// transformZExtICmp - Transform (zext icmp) to bitwise / integer operations
/// in order to eliminate the icmp.
Instruction *InstCombiner::transformZExtICmp(ICmpInst *ICI, Instruction &CI,
                                             bool DoXform) {
  // If we are just checking for a icmp eq of a single bit and zext'ing it
  // to an integer, then shift the bit to the appropriate place and then
  // cast to integer to avoid the comparison.
  if (ConstantInt *Op1C = dyn_cast<ConstantInt>(ICI->getOperand(1))) {
    const APInt &Op1CV = Op1C->getValue();
      
    // zext (x <s  0) to i32 --> x>>u31      true if signbit set.
    // zext (x >s -1) to i32 --> (x>>u31)^1  true if signbit clear.
    if ((ICI->getPredicate() == ICmpInst::ICMP_SLT && Op1CV == 0) ||
        (ICI->getPredicate() == ICmpInst::ICMP_SGT &&Op1CV.isAllOnesValue())) {
      if (!DoXform) return ICI;

      Value *In = ICI->getOperand(0);
      Value *Sh = ConstantInt::get(In->getType(),
                                   In->getType()->getScalarSizeInBits()-1);
      In = Builder->CreateLShr(In, Sh, In->getName()+".lobit");
      if (In->getType() != CI.getType())
        In = Builder->CreateIntCast(In, CI.getType(), false/*ZExt*/, "tmp");

      if (ICI->getPredicate() == ICmpInst::ICMP_SGT) {
        Constant *One = ConstantInt::get(In->getType(), 1);
        In = Builder->CreateXor(In, One, In->getName()+".not");
      }

      return ReplaceInstUsesWith(CI, In);
    }
      
      
      
    // zext (X == 0) to i32 --> X^1      iff X has only the low bit set.
    // zext (X == 0) to i32 --> (X>>1)^1 iff X has only the 2nd bit set.
    // zext (X == 1) to i32 --> X        iff X has only the low bit set.
    // zext (X == 2) to i32 --> X>>1     iff X has only the 2nd bit set.
    // zext (X != 0) to i32 --> X        iff X has only the low bit set.
    // zext (X != 0) to i32 --> X>>1     iff X has only the 2nd bit set.
    // zext (X != 1) to i32 --> X^1      iff X has only the low bit set.
    // zext (X != 2) to i32 --> (X>>1)^1 iff X has only the 2nd bit set.
    if ((Op1CV == 0 || Op1CV.isPowerOf2()) && 
        // This only works for EQ and NE
        ICI->isEquality()) {
      // If Op1C some other power of two, convert:
      uint32_t BitWidth = Op1C->getType()->getBitWidth();
      APInt KnownZero(BitWidth, 0), KnownOne(BitWidth, 0);
      APInt TypeMask(APInt::getAllOnesValue(BitWidth));
      ComputeMaskedBits(ICI->getOperand(0), TypeMask, KnownZero, KnownOne);
        
      APInt KnownZeroMask(~KnownZero);
      if (KnownZeroMask.isPowerOf2()) { // Exactly 1 possible 1?
        if (!DoXform) return ICI;

        bool isNE = ICI->getPredicate() == ICmpInst::ICMP_NE;
        if (Op1CV != 0 && (Op1CV != KnownZeroMask)) {
          // (X&4) == 2 --> false
          // (X&4) != 2 --> true
          Constant *Res = ConstantInt::get(Type::getInt1Ty(CI.getContext()),
                                           isNE);
          Res = ConstantExpr::getZExt(Res, CI.getType());
          return ReplaceInstUsesWith(CI, Res);
        }
          
        uint32_t ShiftAmt = KnownZeroMask.logBase2();
        Value *In = ICI->getOperand(0);
        if (ShiftAmt) {
          // Perform a logical shr by shiftamt.
          // Insert the shift to put the result in the low bit.
          In = Builder->CreateLShr(In, ConstantInt::get(In->getType(),ShiftAmt),
                                   In->getName()+".lobit");
        }
          
        if ((Op1CV != 0) == isNE) { // Toggle the low bit.
          Constant *One = ConstantInt::get(In->getType(), 1);
          In = Builder->CreateXor(In, One, "tmp");
        }
          
        if (CI.getType() == In->getType())
          return ReplaceInstUsesWith(CI, In);
        else
          return CastInst::CreateIntegerCast(In, CI.getType(), false/*ZExt*/);
      }
    }
  }

  // icmp ne A, B is equal to xor A, B when A and B only really have one bit.
  // It is also profitable to transform icmp eq into not(xor(A, B)) because that
  // may lead to additional simplifications.
  if (ICI->isEquality() && CI.getType() == ICI->getOperand(0)->getType()) {
    if (const IntegerType *ITy = dyn_cast<IntegerType>(CI.getType())) {
      uint32_t BitWidth = ITy->getBitWidth();
      Value *LHS = ICI->getOperand(0);
      Value *RHS = ICI->getOperand(1);

      APInt KnownZeroLHS(BitWidth, 0), KnownOneLHS(BitWidth, 0);
      APInt KnownZeroRHS(BitWidth, 0), KnownOneRHS(BitWidth, 0);
      APInt TypeMask(APInt::getAllOnesValue(BitWidth));
      ComputeMaskedBits(LHS, TypeMask, KnownZeroLHS, KnownOneLHS);
      ComputeMaskedBits(RHS, TypeMask, KnownZeroRHS, KnownOneRHS);

      if (KnownZeroLHS == KnownZeroRHS && KnownOneLHS == KnownOneRHS) {
        APInt KnownBits = KnownZeroLHS | KnownOneLHS;
        APInt UnknownBit = ~KnownBits;
        if (UnknownBit.countPopulation() == 1) {
          if (!DoXform) return ICI;

          Value *Result = Builder->CreateXor(LHS, RHS);

          // Mask off any bits that are set and won't be shifted away.
          if (KnownOneLHS.uge(UnknownBit))
            Result = Builder->CreateAnd(Result,
                                        ConstantInt::get(ITy, UnknownBit));

          // Shift the bit we're testing down to the lsb.
          Result = Builder->CreateLShr(
               Result, ConstantInt::get(ITy, UnknownBit.countTrailingZeros()));

          if (ICI->getPredicate() == ICmpInst::ICMP_EQ)
            Result = Builder->CreateXor(Result, ConstantInt::get(ITy, 1));
          Result->takeName(ICI);
          return ReplaceInstUsesWith(CI, Result);
        }
      }
    }
  }

  return 0;
}

Instruction *InstCombiner::visitZExt(ZExtInst &CI) {
  // If one of the common conversion will work, do it.
  if (Instruction *Result = commonIntCastTransforms(CI))
    return Result;

  Value *Src = CI.getOperand(0);

  // If this is a TRUNC followed by a ZEXT then we are dealing with integral
  // types and if the sizes are just right we can convert this into a logical
  // 'and' which will be much cheaper than the pair of casts.
  if (TruncInst *CSrc = dyn_cast<TruncInst>(Src)) {   // A->B->C cast
    // Get the sizes of the types involved.  We know that the intermediate type
    // will be smaller than A or C, but don't know the relation between A and C.
    Value *A = CSrc->getOperand(0);
    unsigned SrcSize = A->getType()->getScalarSizeInBits();
    unsigned MidSize = CSrc->getType()->getScalarSizeInBits();
    unsigned DstSize = CI.getType()->getScalarSizeInBits();
    // If we're actually extending zero bits, then if
    // SrcSize <  DstSize: zext(a & mask)
    // SrcSize == DstSize: a & mask
    // SrcSize  > DstSize: trunc(a) & mask
    if (SrcSize < DstSize) {
      APInt AndValue(APInt::getLowBitsSet(SrcSize, MidSize));
      Constant *AndConst = ConstantInt::get(A->getType(), AndValue);
      Value *And = Builder->CreateAnd(A, AndConst, CSrc->getName()+".mask");
      return new ZExtInst(And, CI.getType());
    }
    
    if (SrcSize == DstSize) {
      APInt AndValue(APInt::getLowBitsSet(SrcSize, MidSize));
      return BinaryOperator::CreateAnd(A, ConstantInt::get(A->getType(),
                                                           AndValue));
    }
    if (SrcSize > DstSize) {
      Value *Trunc = Builder->CreateTrunc(A, CI.getType(), "tmp");
      APInt AndValue(APInt::getLowBitsSet(DstSize, MidSize));
      return BinaryOperator::CreateAnd(Trunc, 
                                       ConstantInt::get(Trunc->getType(),
                                                               AndValue));
    }
  }

  if (ICmpInst *ICI = dyn_cast<ICmpInst>(Src))
    return transformZExtICmp(ICI, CI);

  BinaryOperator *SrcI = dyn_cast<BinaryOperator>(Src);
  if (SrcI && SrcI->getOpcode() == Instruction::Or) {
    // zext (or icmp, icmp) --> or (zext icmp), (zext icmp) if at least one
    // of the (zext icmp) will be transformed.
    ICmpInst *LHS = dyn_cast<ICmpInst>(SrcI->getOperand(0));
    ICmpInst *RHS = dyn_cast<ICmpInst>(SrcI->getOperand(1));
    if (LHS && RHS && LHS->hasOneUse() && RHS->hasOneUse() &&
        (transformZExtICmp(LHS, CI, false) ||
         transformZExtICmp(RHS, CI, false))) {
      Value *LCast = Builder->CreateZExt(LHS, CI.getType(), LHS->getName());
      Value *RCast = Builder->CreateZExt(RHS, CI.getType(), RHS->getName());
      return BinaryOperator::Create(Instruction::Or, LCast, RCast);
    }
  }

  // zext(trunc(t) & C) -> (t & zext(C)).
  if (SrcI && SrcI->getOpcode() == Instruction::And && SrcI->hasOneUse())
    if (ConstantInt *C = dyn_cast<ConstantInt>(SrcI->getOperand(1)))
      if (TruncInst *TI = dyn_cast<TruncInst>(SrcI->getOperand(0))) {
        Value *TI0 = TI->getOperand(0);
        if (TI0->getType() == CI.getType())
          return
            BinaryOperator::CreateAnd(TI0,
                                ConstantExpr::getZExt(C, CI.getType()));
      }

  // zext((trunc(t) & C) ^ C) -> ((t & zext(C)) ^ zext(C)).
  if (SrcI && SrcI->getOpcode() == Instruction::Xor && SrcI->hasOneUse())
    if (ConstantInt *C = dyn_cast<ConstantInt>(SrcI->getOperand(1)))
      if (BinaryOperator *And = dyn_cast<BinaryOperator>(SrcI->getOperand(0)))
        if (And->getOpcode() == Instruction::And && And->hasOneUse() &&
            And->getOperand(1) == C)
          if (TruncInst *TI = dyn_cast<TruncInst>(And->getOperand(0))) {
            Value *TI0 = TI->getOperand(0);
            if (TI0->getType() == CI.getType()) {
              Constant *ZC = ConstantExpr::getZExt(C, CI.getType());
              Value *NewAnd = Builder->CreateAnd(TI0, ZC, "tmp");
              return BinaryOperator::CreateXor(NewAnd, ZC);
            }
          }

  return 0;
}

Instruction *InstCombiner::visitSExt(SExtInst &CI) {
  if (Instruction *I = commonIntCastTransforms(CI))
    return I;
  
  Value *Src = CI.getOperand(0);
  
  // Canonicalize sign-extend from i1 to a select.
  if (Src->getType() == Type::getInt1Ty(CI.getContext()))
    return SelectInst::Create(Src,
                              Constant::getAllOnesValue(CI.getType()),
                              Constant::getNullValue(CI.getType()));

  // See if the value being truncated is already sign extended.  If so, just
  // eliminate the trunc/sext pair.
  if (Operator::getOpcode(Src) == Instruction::Trunc) {
    Value *Op = cast<User>(Src)->getOperand(0);
    unsigned OpBits   = Op->getType()->getScalarSizeInBits();
    unsigned MidBits  = Src->getType()->getScalarSizeInBits();
    unsigned DestBits = CI.getType()->getScalarSizeInBits();
    unsigned NumSignBits = ComputeNumSignBits(Op);

    if (OpBits == DestBits) {
      // Op is i32, Mid is i8, and Dest is i32.  If Op has more than 24 sign
      // bits, it is already ready.
      if (NumSignBits > DestBits-MidBits)
        return ReplaceInstUsesWith(CI, Op);
    } else if (OpBits < DestBits) {
      // Op is i32, Mid is i8, and Dest is i64.  If Op has more than 24 sign
      // bits, just sext from i32.
      if (NumSignBits > OpBits-MidBits)
        return new SExtInst(Op, CI.getType(), "tmp");
    } else {
      // Op is i64, Mid is i8, and Dest is i32.  If Op has more than 56 sign
      // bits, just truncate to i32.
      if (NumSignBits > OpBits-MidBits)
        return new TruncInst(Op, CI.getType(), "tmp");
    }
  }

  // If the input is a shl/ashr pair of a same constant, then this is a sign
  // extension from a smaller value.  If we could trust arbitrary bitwidth
  // integers, we could turn this into a truncate to the smaller bit and then
  // use a sext for the whole extension.  Since we don't, look deeper and check
  // for a truncate.  If the source and dest are the same type, eliminate the
  // trunc and extend and just do shifts.  For example, turn:
  //   %a = trunc i32 %i to i8
  //   %b = shl i8 %a, 6
  //   %c = ashr i8 %b, 6
  //   %d = sext i8 %c to i32
  // into:
  //   %a = shl i32 %i, 30
  //   %d = ashr i32 %a, 30
  Value *A = 0;
  ConstantInt *BA = 0, *CA = 0;
  if (match(Src, m_AShr(m_Shl(m_Value(A), m_ConstantInt(BA)),
                        m_ConstantInt(CA))) &&
      BA == CA && isa<TruncInst>(A)) {
    Value *I = cast<TruncInst>(A)->getOperand(0);
    if (I->getType() == CI.getType()) {
      unsigned MidSize = Src->getType()->getScalarSizeInBits();
      unsigned SrcDstSize = CI.getType()->getScalarSizeInBits();
      unsigned ShAmt = CA->getZExtValue()+SrcDstSize-MidSize;
      Constant *ShAmtV = ConstantInt::get(CI.getType(), ShAmt);
      I = Builder->CreateShl(I, ShAmtV, CI.getName());
      return BinaryOperator::CreateAShr(I, ShAmtV);
    }
  }
  
  return 0;
}


/// FitsInFPType - Return a Constant* for the specified FP constant if it fits
/// in the specified FP type without changing its value.
static Constant *FitsInFPType(ConstantFP *CFP, const fltSemantics &Sem) {
  bool losesInfo;
  APFloat F = CFP->getValueAPF();
  (void)F.convert(Sem, APFloat::rmNearestTiesToEven, &losesInfo);
  if (!losesInfo)
    return ConstantFP::get(CFP->getContext(), F);
  return 0;
}

/// LookThroughFPExtensions - If this is an fp extension instruction, look
/// through it until we get the source value.
static Value *LookThroughFPExtensions(Value *V) {
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (I->getOpcode() == Instruction::FPExt)
      return LookThroughFPExtensions(I->getOperand(0));
  
  // If this value is a constant, return the constant in the smallest FP type
  // that can accurately represent it.  This allows us to turn
  // (float)((double)X+2.0) into x+2.0f.
  if (ConstantFP *CFP = dyn_cast<ConstantFP>(V)) {
    if (CFP->getType() == Type::getPPC_FP128Ty(V->getContext()))
      return V;  // No constant folding of this.
    // See if the value can be truncated to float and then reextended.
    if (Value *V = FitsInFPType(CFP, APFloat::IEEEsingle))
      return V;
    if (CFP->getType() == Type::getDoubleTy(V->getContext()))
      return V;  // Won't shrink.
    if (Value *V = FitsInFPType(CFP, APFloat::IEEEdouble))
      return V;
    // Don't try to shrink to various long double types.
  }
  
  return V;
}

Instruction *InstCombiner::visitFPTrunc(FPTruncInst &CI) {
  if (Instruction *I = commonCastTransforms(CI))
    return I;
  
  // If we have fptrunc(fadd (fpextend x), (fpextend y)), where x and y are
  // smaller than the destination type, we can eliminate the truncate by doing
  // the add as the smaller type.  This applies to fadd/fsub/fmul/fdiv as well
  // as many builtins (sqrt, etc).
  BinaryOperator *OpI = dyn_cast<BinaryOperator>(CI.getOperand(0));
  if (OpI && OpI->hasOneUse()) {
    switch (OpI->getOpcode()) {
    default: break;
    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
      const Type *SrcTy = OpI->getType();
      Value *LHSTrunc = LookThroughFPExtensions(OpI->getOperand(0));
      Value *RHSTrunc = LookThroughFPExtensions(OpI->getOperand(1));
      if (LHSTrunc->getType() != SrcTy && 
          RHSTrunc->getType() != SrcTy) {
        unsigned DstSize = CI.getType()->getScalarSizeInBits();
        // If the source types were both smaller than the destination type of
        // the cast, do this xform.
        if (LHSTrunc->getType()->getScalarSizeInBits() <= DstSize &&
            RHSTrunc->getType()->getScalarSizeInBits() <= DstSize) {
          LHSTrunc = Builder->CreateFPExt(LHSTrunc, CI.getType());
          RHSTrunc = Builder->CreateFPExt(RHSTrunc, CI.getType());
          return BinaryOperator::Create(OpI->getOpcode(), LHSTrunc, RHSTrunc);
        }
      }
      break;  
    }
  }
  return 0;
}

Instruction *InstCombiner::visitFPExt(CastInst &CI) {
  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitFPToUI(FPToUIInst &FI) {
  Instruction *OpI = dyn_cast<Instruction>(FI.getOperand(0));
  if (OpI == 0)
    return commonCastTransforms(FI);

  // fptoui(uitofp(X)) --> X
  // fptoui(sitofp(X)) --> X
  // This is safe if the intermediate type has enough bits in its mantissa to
  // accurately represent all values of X.  For example, do not do this with
  // i64->float->i64.  This is also safe for sitofp case, because any negative
  // 'X' value would cause an undefined result for the fptoui. 
  if ((isa<UIToFPInst>(OpI) || isa<SIToFPInst>(OpI)) &&
      OpI->getOperand(0)->getType() == FI.getType() &&
      (int)FI.getType()->getScalarSizeInBits() < /*extra bit for sign */
                    OpI->getType()->getFPMantissaWidth())
    return ReplaceInstUsesWith(FI, OpI->getOperand(0));

  return commonCastTransforms(FI);
}

Instruction *InstCombiner::visitFPToSI(FPToSIInst &FI) {
  Instruction *OpI = dyn_cast<Instruction>(FI.getOperand(0));
  if (OpI == 0)
    return commonCastTransforms(FI);
  
  // fptosi(sitofp(X)) --> X
  // fptosi(uitofp(X)) --> X
  // This is safe if the intermediate type has enough bits in its mantissa to
  // accurately represent all values of X.  For example, do not do this with
  // i64->float->i64.  This is also safe for sitofp case, because any negative
  // 'X' value would cause an undefined result for the fptoui. 
  if ((isa<UIToFPInst>(OpI) || isa<SIToFPInst>(OpI)) &&
      OpI->getOperand(0)->getType() == FI.getType() &&
      (int)FI.getType()->getScalarSizeInBits() <=
                    OpI->getType()->getFPMantissaWidth())
    return ReplaceInstUsesWith(FI, OpI->getOperand(0));
  
  return commonCastTransforms(FI);
}

Instruction *InstCombiner::visitUIToFP(CastInst &CI) {
  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitSIToFP(CastInst &CI) {
  return commonCastTransforms(CI);
}

Instruction *InstCombiner::visitPtrToInt(PtrToIntInst &CI) {
  // If the destination integer type is smaller than the intptr_t type for
  // this target, do a ptrtoint to intptr_t then do a trunc.  This allows the
  // trunc to be exposed to other transforms.  Don't do this for extending
  // ptrtoint's, because we don't know if the target sign or zero extends its
  // pointers.
  if (TD &&
      CI.getType()->getScalarSizeInBits() < TD->getPointerSizeInBits()) {
    Value *P = Builder->CreatePtrToInt(CI.getOperand(0),
                                       TD->getIntPtrType(CI.getContext()),
                                       "tmp");
    return new TruncInst(P, CI.getType());
  }
  
  return commonPointerCastTransforms(CI);
}


Instruction *InstCombiner::visitIntToPtr(IntToPtrInst &CI) {
  // If the source integer type is larger than the intptr_t type for
  // this target, do a trunc to the intptr_t type, then inttoptr of it.  This
  // allows the trunc to be exposed to other transforms.  Don't do this for
  // extending inttoptr's, because we don't know if the target sign or zero
  // extends to pointers.
  if (TD && CI.getOperand(0)->getType()->getScalarSizeInBits() >
      TD->getPointerSizeInBits()) {
    Value *P = Builder->CreateTrunc(CI.getOperand(0),
                                    TD->getIntPtrType(CI.getContext()), "tmp");
    return new IntToPtrInst(P, CI.getType());
  }
  
  if (Instruction *I = commonCastTransforms(CI))
    return I;

  return 0;
}

Instruction *InstCombiner::visitBitCast(BitCastInst &CI) {
  // If the operands are integer typed then apply the integer transforms,
  // otherwise just apply the common ones.
  Value *Src = CI.getOperand(0);
  const Type *SrcTy = Src->getType();
  const Type *DestTy = CI.getType();

  if (isa<PointerType>(SrcTy)) {
    if (Instruction *I = commonPointerCastTransforms(CI))
      return I;
  } else {
    if (Instruction *Result = commonCastTransforms(CI))
      return Result;
  }


  // Get rid of casts from one type to the same type. These are useless and can
  // be replaced by the operand.
  if (DestTy == Src->getType())
    return ReplaceInstUsesWith(CI, Src);

  if (const PointerType *DstPTy = dyn_cast<PointerType>(DestTy)) {
    const PointerType *SrcPTy = cast<PointerType>(SrcTy);
    const Type *DstElTy = DstPTy->getElementType();
    const Type *SrcElTy = SrcPTy->getElementType();
    
    // If the address spaces don't match, don't eliminate the bitcast, which is
    // required for changing types.
    if (SrcPTy->getAddressSpace() != DstPTy->getAddressSpace())
      return 0;
    
    // If we are casting a alloca to a pointer to a type of the same
    // size, rewrite the allocation instruction to allocate the "right" type.
    // There is no need to modify malloc calls because it is their bitcast that
    // needs to be cleaned up.
    if (AllocaInst *AI = dyn_cast<AllocaInst>(Src))
      if (Instruction *V = PromoteCastOfAllocation(CI, *AI))
        return V;
    
    // If the source and destination are pointers, and this cast is equivalent
    // to a getelementptr X, 0, 0, 0...  turn it into the appropriate gep.
    // This can enhance SROA and other transforms that want type-safe pointers.
    Constant *ZeroUInt =
      Constant::getNullValue(Type::getInt32Ty(CI.getContext()));
    unsigned NumZeros = 0;
    while (SrcElTy != DstElTy && 
           isa<CompositeType>(SrcElTy) && !isa<PointerType>(SrcElTy) &&
           SrcElTy->getNumContainedTypes() /* not "{}" */) {
      SrcElTy = cast<CompositeType>(SrcElTy)->getTypeAtIndex(ZeroUInt);
      ++NumZeros;
    }

    // If we found a path from the src to dest, create the getelementptr now.
    if (SrcElTy == DstElTy) {
      SmallVector<Value*, 8> Idxs(NumZeros+1, ZeroUInt);
      return GetElementPtrInst::CreateInBounds(Src, Idxs.begin(), Idxs.end(),"",
                                               ((Instruction*) NULL));
    }
  }

  if (const VectorType *DestVTy = dyn_cast<VectorType>(DestTy)) {
    if (DestVTy->getNumElements() == 1) {
      if (!isa<VectorType>(SrcTy)) {
        Value *Elem = Builder->CreateBitCast(Src, DestVTy->getElementType());
        return InsertElementInst::Create(UndefValue::get(DestTy), Elem,
                     Constant::getNullValue(Type::getInt32Ty(CI.getContext())));
      }
      // FIXME: Canonicalize bitcast(insertelement) -> insertelement(bitcast)
    }
  }

  if (const VectorType *SrcVTy = dyn_cast<VectorType>(SrcTy)) {
    if (SrcVTy->getNumElements() == 1) {
      if (!isa<VectorType>(DestTy)) {
        Value *Elem = 
          Builder->CreateExtractElement(Src,
                     Constant::getNullValue(Type::getInt32Ty(CI.getContext())));
        return CastInst::Create(Instruction::BitCast, Elem, DestTy);
      }
    }
  }

  if (ShuffleVectorInst *SVI = dyn_cast<ShuffleVectorInst>(Src)) {
    if (SVI->hasOneUse()) {
      // Okay, we have (bitconvert (shuffle ..)).  Check to see if this is
      // a bitconvert to a vector with the same # elts.
      if (isa<VectorType>(DestTy) && 
          cast<VectorType>(DestTy)->getNumElements() ==
                SVI->getType()->getNumElements() &&
          SVI->getType()->getNumElements() ==
            cast<VectorType>(SVI->getOperand(0)->getType())->getNumElements()) {
        CastInst *Tmp;
        // If either of the operands is a cast from CI.getType(), then
        // evaluating the shuffle in the casted destination's type will allow
        // us to eliminate at least one cast.
        if (((Tmp = dyn_cast<CastInst>(SVI->getOperand(0))) && 
             Tmp->getOperand(0)->getType() == DestTy) ||
            ((Tmp = dyn_cast<CastInst>(SVI->getOperand(1))) && 
             Tmp->getOperand(0)->getType() == DestTy)) {
          Value *LHS = Builder->CreateBitCast(SVI->getOperand(0), DestTy);
          Value *RHS = Builder->CreateBitCast(SVI->getOperand(1), DestTy);
          // Return a new shuffle vector.  Use the same element ID's, as we
          // know the vector types match #elts.
          return new ShuffleVectorInst(LHS, RHS, SVI->getOperand(2));
        }
      }
    }
  }
  return 0;
}
