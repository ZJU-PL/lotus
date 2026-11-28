// Implementation of the StorePruner.
//
// Store pruning is an optimization technique used at function calls.
// Instead of passing the entire memory store to the callee, we only pass the
// portion of the store that is "reachable" from the function arguments and
// global variables.
//
// This reduces the size of the store propagated through the analysis, improving
// performance and potentially precision (by hiding irrelevant parts of the
// heap/stack).
//
// Algorithm:
// 1. Root Identification: Collect all memory objects pointed to by arguments
// and all global objects.
// 2. Reachability Analysis: Traverse the memory graph (Store) starting from
// roots to find all reachable objects.
// 3. Filtering: Create a new Store containing only the reachable objects.

#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/StorePruner.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFGNode.h"
#include "Alias/InclusionBased/TPA/Util/IO/PointerAnalysis/Printer.h"

#include <algorithm>
#include <cstdint>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

// Heuristic to determine if an object is "globally accessible" by default.
// Stack and Heap objects are generally only accessible if a pointer to them is
// passed explicitly. Global objects (and functions) are always accessible.
static bool isAccessible(const MemoryObject *obj) {
  return !(obj->isStackObject() || obj->isHeapObject());
}

// Computes the initial set of objects that are immediately reachable at the
// call site. Roots include:
// 1. Objects pointed to by the function arguments.
// 2. Global objects (universally reachable).
StorePruner::ObjectSet StorePruner::getRootSet(const Store &store,
                                               const ProgramPoint &pp) {
  ObjectSet ret;

  const auto *ctx = pp.getContext();
  auto const &callNode = static_cast<const CallCFGNode &>(*pp.getCFGNode());

  // 1. Add objects reachable via arguments
  for (const auto *argVal : callNode) {
    const auto *argPtr = ptrManager.getPointer(ctx, argVal);
    // argPtr may be nullptr for values not tracked by the pointer manager
    // (e.g., constants, undef values, or global values not yet registered).
    // In such cases, we skip them as they don't contribute to the root set.
    if (argPtr == nullptr)
      continue;

    auto argSet = env.lookup(argPtr);
    ret.insert(argSet.begin(), argSet.end());
  }

  // 2. Add implicitly accessible objects (Globals) found in the current store
  for (auto const &mapping : store) {
    if (isAccessible(mapping.first))
      ret.insert(mapping.first);
  }

  return ret;
}

// Performs a BFS/DFS to find the transitive closure of reachable objects in the
// store.
void StorePruner::findAllReachableObjects(const Store &store,
                                          ObjectSet &reachableSet) {
  // Every object's stored pointers must be followed, but expanding the same
  // allocation's structural suffix at every field costs O(fields^2) per call.
  // A lower-offset expansion includes every higher-offset structural suffix.
  using Block = const MemoryBlock *;
  llvm::DenseMap<Block, size_t> scheduledMin, expandedMin;
  auto note = [&](const MemoryObject *object) {
    if (object->isSpecialObject() ||
        (!store.isOverlay() && isAccessible(object)))
      return;
    auto result =
        scheduledMin.try_emplace(object->getMemoryBlock(), object->getOffset());
    if (!result.second)
      result.first->second =
          std::min(result.first->second, object->getOffset());
  };
  std::vector<const MemoryObject *> work;
  work.reserve(reachableSet.size() * 2);
  for (const auto *object : reachableSet) {
    work.push_back(object);
    note(object);
  }
  auto enqueue = [&](const MemoryObject *object) {
    // For an owning store, getRootSet() has already seeded every globally
    // accessible object that has a binding in `store`, and any unbound global
    // or function object contributes neither bindings nor outgoing edges.
    if (!store.isOverlay() && isAccessible(object))
      return;
    if (reachableSet.insert(object).second) {
      note(object);
      work.push_back(object);
    }
  };
  for (size_t next = 0; next < work.size(); ++next) {
    const auto *object = work[next];
    // Global structural fields with bindings were all seeded as roots;
    // fields without bindings cannot lead to another object in an owning map.
    // Overlays can hide base bindings from iteration, so retain full expansion.
    if (!object->isSpecialObject() &&
        (store.isOverlay() || !isAccessible(object))) {
      auto *block = object->getMemoryBlock();
      const auto offset = object->getOffset();
      auto expanded = expandedMin.find(block);
      if (offset == scheduledMin.find(block)->second &&
          (expanded == expandedMin.end() || offset < expanded->second)) {
        expandedMin[block] = offset;
        for (const auto *field :
             memManager.getReachablePointerObjectsRef(object, false))
          enqueue(field);
      }
    }
    // This remains per-object, even when its structural suffix is covered.
    for (const auto *pointee : store.lookup(object))
      enqueue(pointee);
  }
}

// Constructs a new Store containing ONLY the reachable objects.
// Unreachable objects are pruned (not included in the result).
Store StorePruner::filterStore(const Store &store,
                               const ObjectSet &reachableSet) {
  Store ret;
  ret.reserve(std::min<size_t>(store.size(), reachableSet.size()));
  for (auto const &mapping : store) {
    if (reachableSet.count(mapping.first))
      ret.strongUpdate(mapping.first, mapping.second);
    // else: skip unreachable objects (pruning)
  }
  return ret;
}

bool StorePruner::pruneStoreIfNeeded(const Store &store, const ProgramPoint &pp,
                                     Store &out) {
  assert(pp.getCFGNode()->isCallNode() &&
         "Pruning can only happen on call node!");

  // O(1): if there are no stack/heap bindings anywhere in the persistent
  // radix tree, every binding is globally accessible and nothing can be pruned.
  const size_t totalNonAccessibleBound = store.nonAccessibleCount();
  if (totalNonAccessibleBound == 0)
    return false;

  // Collect deduplicated non-accessible argument PtsSets first so we can query
  // the exact StorePrunerCache keyed by (store.rootVersionId(), argSets).
  const auto *ctx = pp.getContext();
  const auto &callNode = static_cast<const CallCFGNode &>(*pp.getCFGNode());
  llvm::SmallVector<PtsSet, 4> nonAccArgSets;
  llvm::SmallVector<const void *, 4> keyArgSets;
  for (const auto *argVal : callNode) {
    const auto *argPtr = ptrManager.getPointer(ctx, argVal);
    if (argPtr == nullptr)
      continue;
    auto argSet = env.lookup(argPtr);
    if (!argSet.hasNonAccessible())
      continue;
    const void *raw = argSet.rawKey();
    if (std::find(keyArgSets.begin(), keyArgSets.end(), raw) ==
        keyArgSets.end()) {
      keyArgSets.push_back(raw);
      nonAccArgSets.push_back(argSet);
    }
  }
  if (keyArgSets.size() > 1u)
    std::sort(keyArgSets.begin(), keyArgSets.end());

  const uint64_t storeVer = store.rootVersionId();
  uint64_t cacheHash = storeVer * 11400714819323198485ULL;
  if (cache != nullptr) {
    for (const void *raw : keyArgSets) {
      cacheHash ^= (reinterpret_cast<uintptr_t>(raw) >> 4) *
                   14029467366897019727ULL;
      cacheHash = (cacheHash << 27) | (cacheHash >> 37);
    }
    if (const auto *hit = cache->find(cacheHash, storeVer, keyArgSets)) {
      if (hit->pruned)
        out = hit->outStore;
      return hit->pruned;
    }
  }

  ObjectSet reachableNonAcc;
  std::vector<const MemoryObject *> work;

  using Block = const MemoryBlock *;
  llvm::DenseMap<Block, size_t> scheduledMin, expandedMin;
  auto note = [&](const MemoryObject *object) {
    if (object->isSpecialObject())
      return;
    auto result =
        scheduledMin.try_emplace(object->getMemoryBlock(), object->getOffset());
    if (!result.second)
      result.first->second =
          std::min(result.first->second, object->getOffset());
  };
  auto enqueueNonAcc = [&](const MemoryObject *object) {
    if (isAccessible(object))
      return;
    if (reachableNonAcc.insert(object).second) {
      note(object);
      work.push_back(object);
    }
  };

  // Seed non-accessible objects pointed to by accessible (global/function)
  // bindings, skipping all subtrees with accToNonAccMask == 0 in O(1).
  store.forEachAccessiblePointingToNonAccessible([&](PtsSet pts) {
    for (const auto *pointee : pts)
      enqueueNonAcc(pointee);
  });

  // Seed non-accessible objects pointed to by call arguments.
  for (const auto &argSet : nonAccArgSets) {
    for (const auto *obj : argSet)
      enqueueNonAcc(obj);
  }

  // BFS strictly over reachable non-accessible (stack/heap) objects.
  llvm::SmallVector<uint32_t, 64> keepBoundNonAccIds;
  for (size_t next = 0; next < work.size(); ++next) {
    const auto *object = work[next];
    if (!object->isSpecialObject()) {
      auto *block = object->getMemoryBlock();
      const auto offset = object->getOffset();
      auto expanded = expandedMin.find(block);
      if (offset == scheduledMin.find(block)->second &&
          (expanded == expandedMin.end() || offset < expanded->second)) {
        expandedMin[block] = offset;
        for (const auto *field :
             memManager.getReachablePointerObjectsRef(object, false))
          enqueueNonAcc(field);
      }
    }
    if (const PtsSet *pts = store.findOwnBinding(object)) {
      keepBoundNonAccIds.push_back(object->getId());
      if (pts->hasNonAccessible()) {
        for (const auto *pointee : *pts)
          enqueueNonAcc(pointee);
      }
    }
  }

  const bool didPrune = keepBoundNonAccIds.size() != totalNonAccessibleBound;
  if (didPrune) {
    std::sort(keepBoundNonAccIds.begin(), keepBoundNonAccIds.end());
    // Retain pure-global subtrees and drop pure-unreachable subtrees in O(1).
    out = store.filterNonAccessibleIds(keepBoundNonAccIds);
  }

  if (cache != nullptr) {
    auto &slot = cache->slot(cacheHash);
    slot.storeVersion = storeVer;
    slot.argSets.assign(keyArgSets.begin(), keyArgSets.end());
    slot.pruned = didPrune;
    slot.outStore = didPrune ? out : Store();
  }
  return didPrune;
}

// Main entry point for store pruning.
Store StorePruner::pruneStore(const Store &store, const ProgramPoint &pp) {
  Store out;
  if (!pruneStoreIfNeeded(store, pp, out))
    return store;
  return out;
}

} // namespace tpa