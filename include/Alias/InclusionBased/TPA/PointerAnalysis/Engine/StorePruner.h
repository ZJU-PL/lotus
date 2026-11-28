#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/ProgramPoint.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Store.h"
#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"

#include <cstdint>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>

namespace tpa {

class MemoryManager;
class PointerManager;

// Exact bounded memoization cache for StorePruner::pruneStoreIfNeeded.
// Keyed by (store.rootVersionId(), sorted unique non-accessible arg PtsSets).
// Returning the cached Store reuses its exact persistent radix-tree root,
// turning repeated callee-entry Store::mergeWith calls into O(1) hits.
class StorePrunerCache {
public:
  static constexpr size_t kNumEntries = 4096;
  struct Entry {
    uint64_t storeVersion = 0;
    llvm::SmallVector<const void *, 4> argSets;
    bool pruned = false;
    Store outStore;
  };

private:
  std::vector<Entry> table;

public:
  StorePrunerCache() = default;

  Entry &slot(uint64_t hash) {
    if (table.empty())
      table.resize(kNumEntries);
    return table[hash & (kNumEntries - 1u)];
  }

  const Entry *find(uint64_t hash, uint64_t storeVer,
                    llvm::ArrayRef<const void *> keyArgSets) const {
    if (table.empty())
      return nullptr;
    const auto &entry = table[hash & (kNumEntries - 1u)];
    if (entry.storeVersion == storeVer &&
        llvm::ArrayRef<const void *>(entry.argSets) == keyArgSets)
      return &entry;
    return nullptr;
  }
};

class StorePruner {
private:
  const Env &env;
  const PointerManager &ptrManager;
  const MemoryManager &memManager;
  StorePrunerCache *cache;

  using ObjectSet = llvm::DenseSet<const MemoryObject *>;
  ObjectSet getRootSet(const Store &, const ProgramPoint &);
  void findAllReachableObjects(const Store &, ObjectSet &);
  Store filterStore(const Store &, const ObjectSet &);

public:
  StorePruner(const Env &e, const PointerManager &p, const MemoryManager &m,
              StorePrunerCache *c = nullptr)
      : env(e), ptrManager(p), memManager(m), cache(c) {}

  Store pruneStore(const Store &, const ProgramPoint &);
  // Returns true and populates `out` when at least one binding was pruned;
  // returns false when all bindings in `store` are reachable (caller may reuse
  // `store` directly without copying).
  bool pruneStoreIfNeeded(const Store &, const ProgramPoint &, Store &out);
};

} // namespace tpa
