#pragma once

#include "Checker/TPA/Support/ProgramPoint.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <unordered_map>
#include <utility>

namespace tpa::taint {

class TaintMemo {
private:
  std::unordered_map<ProgramPoint, TaintStore> memo;

public:
  using const_iterator = decltype(memo)::const_iterator;

  TaintMemo() = default;

  // Return NULL if key not found
  const TaintStore *lookup(const ProgramPoint &pLoc) const {
    auto itr = memo.find(pLoc);
    if (itr == memo.end())
      return nullptr;
    else
      return &itr->second;
  }

  bool insert(const ProgramPoint &pp, const tpa::MemoryObject *obj,
              TaintLattice tVal) {
    auto itr = memo.find(pp);
    if (itr == memo.end()) {
      itr = memo.insert(std::make_pair(pp, TaintStore())).first;
    }

    return itr->second.weakUpdate(obj, tVal);
  }

  void update(const ProgramPoint &pp, TaintStore &&store) {
    memo[pp] = std::move(store);
  }

  const_iterator begin() const { return memo.begin(); }
  const_iterator end() const { return memo.end(); }
};

} // namespace tpa::taint