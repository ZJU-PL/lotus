/**
 * @file CRange.h
 * @brief Constant range utility class for TypeQualifier analysis
 *
 * This file provides the CRange class, which extends LLVM's ConstantRange
 * with additional utility methods for range manipulation. It is used in
 * value range analysis to track possible integer values during analysis.
 *
 * @ingroup TypeQualifier
 */

#ifndef _CRANGE_H
#define _CRANGE_H

#pragma once

#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Instructions.h>

// llvm::ConstantRange fixup.
class CRange : public llvm::ConstantRange {
  using APInt = llvm::APInt;
  using super = llvm::ConstantRange;

public:
  CRange(uint32_t BitWidth, bool isFullSet) : super(BitWidth, isFullSet) {}
  // Constructors.
  CRange(const super &CR) : super(CR) {}
  CRange(const APInt &Value) : super(Value) {}
  CRange(const APInt &Lower, const APInt &Upper) : super(Lower, Upper) {}
  static CRange makeFullSet(uint32_t BitWidth) {
    return CRange(BitWidth, true);
  }
  static CRange makeEmptySet(uint32_t BitWidth) {
    return CRange(BitWidth, false);
  }
  static CRange makeICmpRegion(unsigned Pred, const CRange &other) {
    return CRange(
        super::makeAllowedICmpRegion((llvm::CmpInst::Predicate)Pred, other));
  }

  void match(const CRange &R) {
    if (this->getBitWidth() != R.getBitWidth()) {
      llvm::errs() << "warning: range " << *this << " " << this->getBitWidth()
                   << " and " << R << " " << R.getBitWidth() << " unmatch\n";
      *this = this->zextOrTrunc(R.getBitWidth());
    }
  }

  bool safeUnion(const CRange &R) {
    CRange V = R, Old = *this;
    V.match(*this);
    *this = this->unionWith(V);
    return Old != *this;
  }

  CRange sdiv(const CRange &RHS) const {
    if (isEmptySet() || RHS.isEmptySet())
      return makeEmptySet(getBitWidth());
    // FIXME: too conservative.
    return makeFullSet(getBitWidth());
  }
};

#endif
