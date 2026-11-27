#pragma once

#include "Checker/TPA/Lattice/TaintLattice.h"

#include <cstddef>
#include <utility>

#include <llvm/ADT/DenseMap.h>

namespace tpa::taint {

template <typename KeyType> class TaintMap {
private:
  llvm::DenseMap<KeyType, TaintLattice> taintMap;

public:
  using const_iterator = typename decltype(taintMap)::const_iterator;

  TaintMap() = default;

  TaintLattice lookup(KeyType key) const {
    auto itr = taintMap.find(key);
    if (itr != taintMap.end())
      return itr->second;
    else
      return TaintLattice::Unknown;
  }

  const TaintLattice *findValue(KeyType key) const {
    auto itr = taintMap.find(key);
    return itr == taintMap.end() ? nullptr : &itr->second;
  }

  bool weakUpdate(KeyType key, TaintLattice l) {
    auto res = taintMap.try_emplace(key, l);
    if (res.second)
      return true;

    auto newVal = Lattice<TaintLattice>::merge(res.first->second, l);
    if (res.first->second == newVal)
      return false;
    res.first->second = newVal;
    return true;
  }

  bool strongUpdate(KeyType key, TaintLattice l) {
    auto res = taintMap.try_emplace(key, l);
    if (res.second)
      return true;

    if (res.first->second == l)
      return false;
    res.first->second = l;
    return true;
  }

  bool mergeWith(const TaintMap<KeyType> &other) {
    auto ret = false;
    for (auto const &mapping : other)
      ret |= weakUpdate(mapping.first, mapping.second);
    return ret;
  }

  const_iterator begin() const { return taintMap.begin(); }
  const_iterator end() const { return taintMap.end(); }
  size_t size() const { return taintMap.size(); }
  bool empty() const { return taintMap.empty(); }
};

} // namespace tpa::taint