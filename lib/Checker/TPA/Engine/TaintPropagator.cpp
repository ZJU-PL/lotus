#include "Checker/TPA/Engine/TaintPropagator.h"

#include "Checker/TPA/Engine/EvalResult.h"
#include "Checker/TPA/Engine/WorkList.h"
#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/TaintMemo.h"

#include <algorithm>

#include <llvm/Support/raw_ostream.h>
using namespace util::io;

namespace tpa::taint {

void TaintPropagator::propagateObjFrom(const ProgramPoint &targetPP,
                                       const tpa::MemoryObject *obj,
                                       TaintLattice val) {
  if (val == TaintLattice::Unknown)
    return;
  const auto *succs = targetPP.getDefUseInstruction()->findMemSuccs(obj);
  if (!succs || succs->empty())
    return;
  memo.registerObject(obj);
  const auto *ctx = targetPP.getContext();
  for (auto *succ : *succs) {
    ProgramPoint succPP(ctx, succ);
    if (memo.insert(succPP, obj, val))
      workList.enqueue(succPP);
  }
}

void TaintPropagator::applyChunkBatchToSuccs(
    const context::Context *ctx, const NodeSet &succs,
    llvm::ArrayRef<std::pair<uint32_t, uint64_t>> chunkVals) {
  if (chunkVals.empty())
    return;
  for (auto *succ : succs) {
    ProgramPoint succPP(ctx, succ);
    auto &entry = memo.getOrCreateEntry(succPP);
    if (entry.store.empty() && chunkVals.size() > 4)
      entry.store.reserveChunks(chunkVals.size());
    bool changed = false;
    for (const auto &cv : chunkVals)
      changed |= memo.insertChunkIntoEntry(entry, cv.first, cv.second);
    if (changed)
      workList.enqueue(succPP);
  }
}

void TaintPropagator::flushBatches(const context::Context *ctx) {
  for (const auto &batch : batches) {
    if (!batch.succs || batch.succs->empty() || batch.chunks.empty())
      continue;
    scratchChunks.clear();
    scratchChunks.reserve(batch.chunks.size());
    for (const auto &ce : batch.chunks)
      if (ce.second != 0ULL)
        scratchChunks.emplace_back(ce.first, ce.second);
    applyChunkBatchToSuccs(ctx, *batch.succs, scratchChunks);
  }
  succsToBatch.clear();
  batches.clear();
}

void TaintPropagator::propagate(const EvalResult &evalResult,
                                llvm::ArrayRef<uint32_t> dirtyObjIds) {
  for (const auto &pp : evalResult.getTopSuccessors())
    workList.enqueue(pp);

  const auto &overrides = evalResult.getOverrides();
  for (const auto &entry : overrides)
    memo.registerObject(entry.first);

  const auto &directProps = evalResult.getDirectMemProps();
  if (directProps.size() == 1) {
    const auto &prop = directProps[0];
    propagateObjFrom(prop.targetPP, prop.obj, evalResult.lookup(prop.obj));
  } else if (directProps.size() > 1) {
    const auto *firstCtx = directProps[0].targetPP.getContext();
    bool sameCtx = true;
    for (size_t i = 1; i < directProps.size(); ++i) {
      if (directProps[i].targetPP.getContext() != firstCtx) {
        sameCtx = false;
        break;
      }
    }
    if (sameCtx) {
      succsToBatch.clear();
      batches.clear();
      for (const auto &prop : directProps) {
        const TaintLattice val = evalResult.lookup(prop.obj);
        if (val == TaintLattice::Unknown)
          continue;
        const auto *succs =
            prop.targetPP.getDefUseInstruction()->findMemSuccs(prop.obj);
        if (!succs || succs->empty())
          continue;
        memo.registerObject(prop.obj);
        auto res = succsToBatch.try_emplace(
            succs, static_cast<uint32_t>(batches.size()));
        if (res.second) {
          batches.emplace_back();
          batches.back().succs = succs;
        }
        const uint32_t id = prop.obj->getId();
        batches[res.first->second].chunks[TaintStore::chunkIndex(id)] |=
            static_cast<uint64_t>(val) << TaintStore::chunkShift(id);
      }
      flushBatches(firstCtx);
    } else {
      for (const auto &prop : directProps)
        propagateObjFrom(prop.targetPP, prop.obj, evalResult.lookup(prop.obj));
    }
  }

  const auto &broadcasts = evalResult.getMemBroadcasts();
  if (broadcasts.empty())
    return;

  const auto *base = evalResult.getBaseStore();
  overrideChunks.clear();
  if (!overrides.empty()) {
    for (const auto &entry : overrides) {
      const uint32_t id = entry.first->getId();
      const uint32_t cIdx = TaintStore::chunkIndex(id);
      const unsigned shift = TaintStore::chunkShift(id);
      auto &slot = overrideChunks[cIdx];
      slot.first |= (3ULL << shift);
      slot.second |= (static_cast<uint64_t>(entry.second) << shift);
    }
  }

  auto addObjToBatch = [&](const DefUseInstruction *duInst,
                           const tpa::MemoryObject *obj, TaintLattice val) {
    if (val == TaintLattice::Unknown)
      return;
    const auto *succs = duInst->findMemSuccs(obj);
    if (!succs || succs->empty())
      return;
    auto res = succsToBatch.try_emplace(succs,
                                        static_cast<uint32_t>(batches.size()));
    if (res.second) {
      batches.emplace_back();
      batches.back().succs = succs;
    }
    const uint32_t id = obj->getId();
    batches[res.first->second].chunks[TaintStore::chunkIndex(id)] |=
        static_cast<uint64_t>(val) << TaintStore::chunkShift(id);
  };

  for (const auto &bc : broadcasts) {
    const auto *duInst = bc.targetPP.getDefUseInstruction();
    const size_t numSuccObjs = duInst->numMemSuccObjects();
    if (numSuccObjs == 0)
      continue;
    const auto *ctx = bc.targetPP.getContext();

    if (bc.full) {
      if (!base && overrideChunks.empty())
        continue;

      const bool hasUngrouped = duInst->hasUngroupedMemSuccs();
      const auto *memGroups = duInst->getMemoryGroups();
      const size_t baseUpperBound =
          (base ? base->numChunks() * 32 : 0) + overrides.size();

      if (!hasUngrouped && memGroups &&
          baseUpperBound < duInst->numMemSuccGroups()) {
        succsToBatch.clear();
        batches.clear();
        if (base) {
          base->forEachNonZero([&](uint32_t objId, TaintLattice val) {
            const auto *obj = memo.getObjectById(objId);
            if (!overrides.empty() && overrides.findValue(obj))
              return;
            addObjToBatch(duInst, obj, val);
          });
        }
        for (const auto &entry : overrides)
          addObjToBatch(duInst, entry.first, entry.second);
        flushBatches(ctx);
      } else {
        if (memGroups) {
          for (const auto &entry : duInst->group_mem_succs()) {
            const auto &succs = entry.second;
            if (succs.empty())
              continue;
            const auto &chunkMasks =
                memGroups->getGroupChunkMasks(entry.first);
            scratchChunks.clear();
            if (overrideChunks.empty() && base &&
                base->numChunks() * 4 < chunkMasks.size()) {
              for (const auto &ce : base->chunks()) {
                auto it = std::lower_bound(
                    chunkMasks.begin(), chunkMasks.end(), ce.first,
                    [](const MemoryGroupTable::ChunkMask &cm, uint32_t key) {
                      return cm.chunkIdx < key;
                    });
                if (it != chunkMasks.end() && it->chunkIdx == ce.first) {
                  const uint64_t w = ce.second & it->mask;
                  if (w != 0ULL)
                    scratchChunks.emplace_back(ce.first, w);
                }
              }
            } else {
              for (const auto &cm : chunkMasks) {
                uint64_t w =
                    base ? (base->lookupChunk(cm.chunkIdx) & cm.mask) : 0ULL;
                if (!overrideChunks.empty()) {
                  auto ov = overrideChunks.find(cm.chunkIdx);
                  if (ov != overrideChunks.end()) {
                    const uint64_t ovMask = ov->second.first & cm.mask;
                    w = (w & ~ovMask) | (ov->second.second & cm.mask);
                  }
                }
                if (w != 0ULL)
                  scratchChunks.emplace_back(cm.chunkIdx, w);
              }
            }
            applyChunkBatchToSuccs(ctx, succs, scratchChunks);
          }
        }
        if (hasUngrouped) {
          for (const auto &entry : duInst->mem_succs()) {
            const TaintLattice val = evalResult.lookup(entry.first);
            if (val == TaintLattice::Unknown)
              continue;
            memo.registerObject(entry.first);
            for (auto *succ : entry.second) {
              ProgramPoint succPP(ctx, succ);
              if (memo.insert(succPP, entry.first, val))
                workList.enqueue(succPP);
            }
          }
        }
      }
    } else {
      if (dirtyObjIds.size() == 1 && overrides.empty()) {
        const auto *obj = memo.getObjectById(dirtyObjIds[0]);
        propagateObjFrom(bc.targetPP, obj, evalResult.lookup(obj));
        continue;
      }
      if (dirtyObjIds.empty() && overrides.empty())
        continue;

      succsToBatch.clear();
      batches.clear();
      for (uint32_t objId : dirtyObjIds) {
        const auto *obj = memo.getObjectById(objId);
        addObjToBatch(duInst, obj, evalResult.lookup(obj));
      }
      if (!overrides.empty()) {
        for (const auto &entry : overrides)
          addObjToBatch(duInst, entry.first, entry.second);
      }
      flushBatches(ctx);
    }
  }
}

} // namespace tpa::taint