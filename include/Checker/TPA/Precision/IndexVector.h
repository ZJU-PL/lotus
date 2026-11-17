#pragma once

#include "Checker/TPA/Lattice/TaintLattice.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace tpa::taint {

using IndexVector = std::vector<std::size_t>;
using TaintVector = std::vector<TaintLattice>;

inline IndexVector getDemandingIndices(const TaintVector &taints) {
  assert(!taints.empty());
  IndexVector ret;

  auto mergeVal = TaintLattice::Unknown;
  for (auto t : taints)
    mergeVal = Lattice<TaintLattice>::merge(mergeVal, t);

  if (mergeVal == TaintLattice::Either) {
    for (std::size_t i = 0, e = taints.size(); i < e; ++i)
      if (taints[i] == TaintLattice::Tainted)
        ret.push_back(i);
  }

  return ret;
}

inline IndexVector getImpreciseIndices(const TaintVector &taints) {
  assert(!taints.empty());
  IndexVector ret;

  for (std::size_t i = 0, e = taints.size(); i < e; ++i)
    if (taints[i] == TaintLattice::Either)
      ret.push_back(i);

  return ret;
}

} // namespace tpa::taint
