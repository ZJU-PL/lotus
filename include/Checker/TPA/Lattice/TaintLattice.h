#pragma once

#include "Checker/TPA/Lattice/Lattice.h"

#include <cstdint>

namespace tpa::taint {

enum class TaintLattice : uint8_t { Unknown, Untainted, Tainted, Either };

template <> struct Lattice<TaintLattice> {
  static LatticeCompareResult compare(TaintLattice lhs, TaintLattice rhs) {
    if (lhs == rhs)
      return LatticeCompareResult::Equal;
    else if (rhs == TaintLattice::Either || lhs == TaintLattice::Unknown)
      return LatticeCompareResult::LessThan;
    else if (lhs == TaintLattice::Either || rhs == TaintLattice::Unknown)
      return LatticeCompareResult::GreaterThan;
    else
      return LatticeCompareResult::Incomparable;
  }

  static TaintLattice merge(TaintLattice lhs, TaintLattice rhs) {
    // Unknown=0 (00), Untainted=1 (01), Tainted=2 (10), Either=3 (11):
    // lattice join is exact bitwise OR.
    return static_cast<TaintLattice>(static_cast<uint8_t>(lhs) |
                                     static_cast<uint8_t>(rhs));
  }

  static bool willMergeSmearTaintness(TaintLattice lhs, TaintLattice rhs) {
    if (lhs == TaintLattice::Either && rhs == TaintLattice::Either)
      return false;
    else if (lhs == TaintLattice::Unknown || rhs == TaintLattice::Unknown)
      return false;
    else
      return lhs != rhs;
  }
};

} // namespace tpa::taint
