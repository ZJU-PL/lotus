#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/ProgramPoint.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Store.h"
#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"

#include <llvm/ADT/DenseSet.h>

namespace tpa {

class MemoryManager;
class PointerManager;

class StorePruner {
private:
  const Env &env;
  const PointerManager &ptrManager;
  const MemoryManager &memManager;

  using ObjectSet = llvm::DenseSet<const MemoryObject *>;
  ObjectSet getRootSet(const Store &, const ProgramPoint &);
  void findAllReachableObjects(const Store &, ObjectSet &);
  Store filterStore(const Store &, const ObjectSet &);

public:
  StorePruner(const Env &e, const PointerManager &p, const MemoryManager &m)
      : env(e), ptrManager(p), memManager(m) {}

  Store pruneStore(const Store &, const ProgramPoint &);
  // Returns true and populates `out` when at least one binding was pruned;
  // returns false when all bindings in `store` are reachable (caller may reuse
  // `store` directly without copying).
  bool pruneStoreIfNeeded(const Store &, const ProgramPoint &, Store &out);
};

} // namespace tpa
