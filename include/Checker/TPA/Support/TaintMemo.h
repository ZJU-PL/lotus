#pragma once

#include "Checker/TPA/Support/ProgramPoint.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

namespace tpa::taint {

class TaintMemo {
public:
  using DirtyObjectList = llvm::SmallVector<uint32_t, 8>;

  struct Entry {
    TaintStore store;
    // Dense MemoryObject IDs whose taint value in `store` grew since `pp` was
    // last evaluated.
    DirtyObjectList dirtyObjIds;
  };

private:
  // Pointer-stable storage so const TaintStore* lookups remain valid across
  // subsequent memo insertions during propagation.
  std::deque<Entry> entries;
  llvm::DenseMap<ProgramPoint, uint32_t> memo;
  // Dense MemoryObject::getId() -> MemoryObject* table for objects stored in
  // this memo.
  std::vector<const tpa::MemoryObject *> idToObj;

public:
  using const_iterator = decltype(memo)::const_iterator;

  TaintMemo() = default;

  void registerObject(const tpa::MemoryObject *obj) {
    assert(obj != nullptr);
    const uint32_t id = obj->getId();
    if (id >= idToObj.size())
      idToObj.resize(static_cast<size_t>(id) + 1, nullptr);
    idToObj[id] = obj;
  }

  const tpa::MemoryObject *getObjectById(uint32_t id) const {
    assert(id < idToObj.size() && idToObj[id] != nullptr);
    return idToObj[id];
  }

  // Return NULL if key not found
  const TaintStore *lookup(const ProgramPoint &pLoc) const {
    auto itr = memo.find(pLoc);
    if (itr == memo.end())
      return nullptr;
    else
      return &entries[itr->second].store;
  }

  Entry &getOrCreateEntry(const ProgramPoint &pp) {
    auto res = memo.try_emplace(pp, static_cast<uint32_t>(entries.size()));
    if (res.second)
      entries.emplace_back();
    return entries[res.first->second];
  }

  bool insertChunkIntoEntry(Entry &entry, uint32_t chunkIdx, uint64_t valWord) {
    const uint64_t diff = entry.store.mergeChunk(chunkIdx, valWord);
    if (diff == 0ULL)
      return false;
    const uint32_t baseId = chunkIdx << 5;
    uint64_t lanes = (diff | (diff >> 1)) & TaintStore::kOddLaneMask;
    while (lanes) {
      const unsigned tz = static_cast<unsigned>(__builtin_ctzll(lanes));
      entry.dirtyObjIds.push_back(baseId | (tz >> 1));
      lanes &= (lanes - 1);
    }
    return true;
  }

  bool insertIntoEntry(Entry &entry, const tpa::MemoryObject *obj,
                       TaintLattice tVal) {
    registerObject(obj);
    const uint32_t id = obj->getId();
    if (entry.store.weakUpdateById(id, tVal)) {
      entry.dirtyObjIds.push_back(id);
      return true;
    }
    return false;
  }

  bool insert(const ProgramPoint &pp, const tpa::MemoryObject *obj,
              TaintLattice tVal) {
    return insertIntoEntry(getOrCreateEntry(pp), obj, tVal);
  }

  DirtyObjectList takeDirtyObjects(const ProgramPoint &pp) {
    DirtyObjectList out;
    auto itr = memo.find(pp);
    if (itr != memo.end())
      out.swap(entries[itr->second].dirtyObjIds);
    return out;
  }

  void update(const ProgramPoint &pp, TaintStore &&store) {
    Entry &entry = getOrCreateEntry(pp);
    entry.store = std::move(store);
    entry.dirtyObjIds.clear();
  }

  size_t size() const { return memo.size(); }
  const_iterator begin() const { return memo.begin(); }
  const_iterator end() const { return memo.end(); }
};

} // namespace tpa::taint