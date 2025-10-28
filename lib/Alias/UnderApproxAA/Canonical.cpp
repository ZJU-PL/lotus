#include "Alias/UnderApproxAA/Canonical.h"
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

using namespace llvm;
using namespace UnderApprox;

// ---------------------------------------------------------------------------
// canonicalisation helpers
// ---------------------------------------------------------------------------

const Value *UnderApprox::stripNoopCasts(const Value *V) {
  while (true) {
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (isNoopAddrSpaceCast(V)) {
      V = cast<AddrSpaceCastInst>(V)->getOperand(0);
      continue;
    }
    if (auto *II = dyn_cast<IntrinsicInst>(V)) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::launder_invariant_group:
      case Intrinsic::strip_invariant_group:
        V = II->getArgOperand(0);
        continue;
      default:
        break;
      }
    }
    return V;
  }
}

bool UnderApprox::isNoopAddrSpaceCast(const Value *V) {
  if (auto *ASC = dyn_cast<AddrSpaceCastInst>(V))
    return ASC->getSrcTy()->getPointerAddressSpace() ==
           ASC->getDestTy()->getPointerAddressSpace();
  return false;
}

bool UnderApprox::sameConstOffset(const DataLayout &DL,
                                  const Value *A, const Value *B) {
  APInt OffA(DL.getPointerSizeInBits(0), 0);
  APInt OffB(DL.getPointerSizeInBits(0), 0);
  const Value *BaseA = A->stripAndAccumulateInBoundsConstantOffsets(DL, OffA);
  const Value *BaseB = B->stripAndAccumulateInBoundsConstantOffsets(DL, OffB);
  return BaseA == BaseB && OffA == OffB;
}

bool UnderApprox::isZeroGEP(const Value *V) {
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return GEP->hasAllZeroIndices();
  return false;
}

bool UnderApprox::isRoundTripCast(const Value *A, const Value *B) {
  auto *ITP = dyn_cast<IntToPtrInst>(A);
  auto *PTI = dyn_cast<PtrToIntInst>(B);
  return ITP && PTI &&
         ITP->getOperand(0) == PTI && PTI->getOperand(0) == ITP;
}