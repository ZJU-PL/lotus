#pragma once

#include "Checker/TPA/Support/ProgramPoint.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

namespace tpa::taint {

class TaintMemo {
private:
  struct Entry {
    TaintStore store;
    // Objects whose taint value in `store` grew since `pp` was last evaluated.
    llvm::SmallVector<const tpa::MemoryObject *, 8> dirtyObjs;
  };

  // Pointer-stable storage so const TaintStore* lookups remain valid across
  // subsequent memo insertions during propagation.
  std::deque<Entry> entries;
  llvm::DenseMap<ProgramPoint, uint32_t> memo;

public:
  using const_iterator = decltype(memo)::const_iterator;
  using DirtyObjectList = llvm::SmallVector<const tpa::MemoryObject *, 8>;

  TaintMemo() = default;

  // Return NULL if key not found
  const TaintStore *lookup(const ProgramPoint &pLoc) const {
    auto itr = memo.find(pLoc);
    if (itr == memo.end())
      return nullptr;
    else
      return &entries[itr->second].store;
  }

  bool insert(const ProgramPoint &pp, const tpa::MemoryObject *obj,
              TaintLattice tVal) {
    auto res = memo.try_emplace(pp, static_cast<uint32_t>(entries.size()));
    if (res.second)
      entries.emplace_back();
    Entry &entry = entries[res.first->second];
    if (entry.store.weakUpdate(obj, tVal)) {
      entry.dirtyObjs.push_back(obj);
      return true;
    }
    return false;
  }

  DirtyObjectList takeDirtyObjects(const ProgramPoint &pp) {
    DirtyObjectList out;
    auto itr = memo.find(pp);
    if (itr != memo.end())
      out.swap(entries[itr->second].dirtyObjs);
    return out;
  }

  void update(const ProgramPoint &pp, TaintStore &&store) {
    auto res = memo.try_emplace(pp, static_cast<uint32_t>(entries.size()));
    if (res.second)
      entries.emplace_back();
    Entry &entry = entries[res.first->second];
    entry.store = std::move(store);
    entry.dirtyObjs.clear();
  }

  size_t size() const { return memo.size(); }
  const_iterator begin() const { return memo.begin(); }
  const_iterator end() const { return memo.end(); }
};

} // namespace tpa::taint