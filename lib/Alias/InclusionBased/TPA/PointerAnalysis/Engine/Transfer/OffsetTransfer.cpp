#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

namespace tpa {

PtsSet TransferFunction::offsetMemory(const MemoryObject *srcObj, size_t offset,
                                      bool isArrayRef) {
  assert(srcObj != nullptr);

  auto &memManager = globalState.getMemoryManager();

  // We have two cases here:
  // - For non-array reference, just access the variable with the given offset
  // - For array reference, examine all offsets base + k * offset < objSize,
  //   using exact modular period folding over the block's ArrayLayout and
  //   interning the resulting PtsSet in one batch.
  if (isArrayRef && offset != 0)
    return PtsSet::fromObjects(memManager.offsetMemoryArray(srcObj, offset));

  return PtsSet::getSingletonSet(memManager.offsetMemory(srcObj, offset));
}

bool TransferFunction::copyWithOffset(const Pointer *dst, const Pointer *src,
                                      size_t offset, bool isArrayRef) {
  assert(dst != nullptr && src != nullptr);

  auto &env = globalState.getEnv();
  auto srcSet = env.lookup(src);
  if (srcSet.empty())
    return false;

  std::vector<PtsSet> srcPtsSets;
  srcPtsSets.reserve(srcSet.size());

  for (const auto *srcObj : srcSet) {
    // Universal object: GEP on an unknown pointer yields an unknown pointer.
    if (srcObj->isUniversalObject()) {
      srcPtsSets.emplace_back(
          PtsSet::getSingletonSet(MemoryManager::getUniversalObject()));
      break;
    }

    // Null object: GEP on null is undefined behaviour; skip it.
    // Previously this was silently skipped and then promoted to Universal when
    // the result set was empty, which introduced spurious aliasing. Now we
    // simply skip null objects and let the result remain empty (or be merged
    // with other non-null contributions). A future improvement could report a
    // potential null-pointer dereference here.
    if (srcObj->isNullObject())
      continue;

    auto pSet = offsetMemory(srcObj, offset, isArrayRef);
    srcPtsSets.emplace_back(pSet);
  }

  PtsSet resSet = PtsSet::mergeAll(srcPtsSets);

  if (resSet.empty())
    return false;

  return env.strongUpdate(dst, resSet);
}

void TransferFunction::evalOffsetNode(const ProgramPoint &pp,
                                      EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &offsetNode = static_cast<const OffsetCFGNode &>(*pp.getCFGNode());
  auto &ptrManager = globalState.getPointerManager();

  const auto *srcPtr = ptrManager.getPointer(ctx, offsetNode.getSrc());
  if (srcPtr == nullptr)
    return;
  const auto *dstPtr = ptrManager.getOrCreatePointer(ctx, offsetNode.getDest());

  auto envChanged = copyWithOffset(dstPtr, srcPtr, offsetNode.getOffset(),
                                   offsetNode.isArrayRef());

  if (envChanged)
    addTopLevelSuccessors(pp, evalResult);
}

} // namespace tpa
