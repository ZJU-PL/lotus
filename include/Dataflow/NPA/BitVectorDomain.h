#ifndef NPA_BIT_VECTOR_DOMAIN_H
#define NPA_BIT_VECTOR_DOMAIN_H

#include "Utils/LLVM/SystemHeaders.h"
#include <llvm/ADT/APInt.h>

namespace npa {

/**
 * BitVectorDomain – idempotent semiring over APInt
 * combine : bitwise OR (\u222a)
 * extend  : bitwise AND (\u2229) – path concatenation keeps bits set on all steps
 * zero    : all-zero vector (\u2205)
 * Note: width is fixed per analysis instance. Helper factory below creates
 *       sized elements so that static interface in NPA remains satisfied.
 */
class BitVectorDomain {
public:
  using value_type = llvm::APInt;
  using test_type  = bool;            // no symbolic guards for now
  static constexpr bool idempotent = true;

  // The width is determined at runtime but stored in a static so that the
  // zero() factory can create well-sized vectors.
  static void setBitWidth(unsigned W) { BitWidth = W; }
  static unsigned getBitWidth() { return BitWidth; }

  static value_type zero() { return llvm::APInt(BitWidth, 0); }

  static bool equal(const value_type &a, const value_type &b) {
    return a.eq(b);
  }
  static value_type combine(const value_type &a, const value_type &b) {
    return a | b; /* union */
  }
  static value_type extend(const value_type &a, const value_type &b) {
    return a & b; /* intersection along path */
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(bool phi, const value_type &thenV, const value_type &elseV) {
    return phi ? thenV : elseV;
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    return a & (~b);
  }

private:
  static unsigned BitWidth; // defined in BitVectorDomain.cpp
};

} // namespace npa

#endif /* NPA_BIT_VECTOR_DOMAIN_H */