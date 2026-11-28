// Implementation of Store transfer functions.
//
// Handles the evaluation of store instructions (*p = q).
// Updates the memory Store based on the points-to sets of the pointers
// involved.
//
// Key Concepts:
// - Strong Update: Completely overwrites the points-to set of a memory object.
//   Possible only when the pointer points to a SINGLE, PRECISE memory object.
// - Weak Update: Adds new relations to the existing points-to set (union).
//   Used when the pointer may point to multiple objects or summary objects.

#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

namespace tpa {

// Performs a strong update on the store.
// Overwrites the content of 'obj' with 'pSet'.
void TransferFunction::strongUpdateStore(const MemoryObject *obj, PtsSet pSet,
                                         Store &store) {
  if (!obj->isSpecialObject())
    store.strongUpdate(obj, pSet);
  // TODO: in the else branch, report NULL-pointer dereference to the user
}

// Performs a weak update on the store.
// Merges 'srcSet' into the existing content of all objects in 'dstSet'.
void TransferFunction::weakUpdateStore(PtsSet dstSet, PtsSet srcSet,
                                       Store &store) {
  for (const auto *updateObj : dstSet) {
    if (!updateObj->isSpecialObject())
      store.weakUpdate(updateObj, srcSet);
  }
}

// Core logic for evaluating a store operation *dst = src.
void TransferFunction::evalStore(const Pointer *dst, const Pointer *src,
                                 const ProgramPoint &pp,
                                 EvalResult &evalResult) {
  auto &env = globalState.getEnv();

  // Look up what 'dst' points to (the location being written to)
  auto dstSet = env.lookup(dst);
  if (dstSet.empty()) {
    addMemLevelSuccessors(pp, *localState, evalResult);
    return;
  }

  // Look up what 'src' points to (the value being stored)
  PtsSet srcSet =
      src != nullptr ? env.lookup(src) : PtsSet::getEmptySet();
  if (srcSet.empty()) {
    // Preserve strong-update monotonicity: if the store definitely overwrites
    // a single non-summary object, kill its previous value rather than leaking
    // the pre-store binding to successors before `src` resolves.
    if (dstSet.size() == 1) {
      const auto *dstObj = *dstSet.begin();
      if (!dstObj->isSpecialObject() && !dstObj->isSummaryObject() &&
          !localState->lookup(dstObj).empty()) {
        auto &store = evalResult.getNewStore(*localState);
        strongUpdateStore(dstObj, PtsSet::getEmptySet(), store);
        addMemLevelSuccessors(pp, store, evalResult);
        return;
      }
    }
    addMemLevelSuccessors(pp, *localState, evalResult);
    return;
  }

  // Persistent Store copy is O(1) (root retain) and updates only path-copy the
  // touched O(log_16 N) path; unchanged writes keep the exact same root pointer
  // so Memo::update skips them in O(1).
  auto &store = evalResult.getNewStore(*localState);

  const auto *dstObj = *dstSet.begin();
  if (dstSet.size() == 1 && !dstObj->isSummaryObject())
    strongUpdateStore(dstObj, srcSet, store);
  else
    weakUpdateStore(dstSet, srcSet, store);

  addMemLevelSuccessors(pp, store, evalResult);
}

// Visitor method for Store nodes.
void TransferFunction::evalStoreNode(const ProgramPoint &pp,
                                     EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &storeNode = static_cast<const StoreCFGNode &>(*pp.getCFGNode());

  auto &ptrManager = globalState.getPointerManager();
  const auto *dstPtr = ptrManager.getPointer(ctx, storeNode.getDest());
  if (dstPtr == nullptr) {
    addMemLevelSuccessors(pp, *localState, evalResult);
    return;
  }
  const auto *srcPtr = ptrManager.getPointer(ctx, storeNode.getSrc());

  evalStore(dstPtr, srcPtr, pp, evalResult);
}

} // namespace tpa
