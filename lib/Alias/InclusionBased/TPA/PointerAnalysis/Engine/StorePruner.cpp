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
  std::unordered_map<Block, size_t> scheduledMin, expandedMin;
  auto note = [&](const MemoryObject *object) {
    if (object->isSpecialObject() ||
        (!store.isOverlay() && isAccessible(object)))
      return;
    auto result =
        scheduledMin.emplace(object->getMemoryBlock(), object->getOffset());
    if (!result.second)
      result.first->second =
          std::min(result.first->second, object->getOffset());
  };
  std::vector<const MemoryObject *> work(reachableSet.begin(),
                                         reachableSet.end());
  for (const auto *object : work)
    note(object);
  auto enqueue = [&](const MemoryObject *object) {
    // For an owning store, every globally accessible binding is already a
    // root. An unbound global/function object adds neither bindings nor edges.
    if (!store.isOverlay() && isAccessible(object) && !store.hasBinding(object))
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
      if (offset == scheduledMin.at(block) &&
          (expanded == expandedMin.end() || offset < expanded->second)) {
        expandedMin[block] = offset;
        for (const auto *field :
             memManager.getReachablePointerObjects(object, false))
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
  for (auto const &mapping : store) {
    if (reachableSet.count(mapping.first))
      ret.strongUpdate(mapping.first, mapping.second);
    // else: skip unreachable objects (pruning)
  }
  return ret;
}

// Main entry point for store pruning.
Store StorePruner::pruneStore(const Store &store, const ProgramPoint &pp) {
  assert(pp.getCFGNode()->isCallNode() &&
         "Pruning can only happen on call node!");

  // Every owning-store binding is already a root in this common case.
  // There is nothing to remove, independently of argument reachability.
  if (!store.isOverlay() &&
      std::all_of(store.begin(), store.end(), [](const auto &binding) {
        return isAccessible(binding.first);
      }))
    return store;

  auto reachableSet = getRootSet(store, pp);
  findAllReachableObjects(store, reachableSet);
  return filterStore(store, reachableSet);
}

} // namespace tpa