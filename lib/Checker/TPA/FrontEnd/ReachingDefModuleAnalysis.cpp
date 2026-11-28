#include "Checker/TPA/FrontEnd/ReachingDefModuleAnalysis.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/FrontEnd/ModRefModuleSummary.h"
#include "Checker/TPA/Program/DefUseFunction.h"
#include "Checker/TPA/Program/Reachability.h"
#include "Utils/LLVM/FIFOWorkList.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;
using namespace tpa;

namespace tpa::taint {

namespace {

const Instruction *getNextInstruction(const Instruction *inst) {
  auto itr = inst->getIterator();
  ++itr;
  assert(itr != inst->getParent()->end());
  return &*itr;
}

class EvalVisitor : public InstVisitor<EvalVisitor> {
private:
  const SemiSparsePointerAnalysis &ptrAnalysis;
  UnmodeledPolicy unmodeledPolicy;
  const ModRefModuleSummary &summaryMap;
  const ExternalModRefTable &modRefTable;

  using StoreType = ReachingDefStore<Instruction>;
  StoreType store;

  void evalStore(Instruction &inst, Value *ptr) {
    auto pSet = ptrAnalysis.getPtsSet(ptr);
    bool needWeekUpdate = true;
    if (pSet.size() == 1) {
      const auto *obj = *pSet.begin();
      if (!obj->isSummaryObject()) {
        store.updateBinding(obj, &inst);
        needWeekUpdate = false;
      }
    }
    if (needWeekUpdate) {
      for (const auto *obj : pSet)
        store.insertBinding(obj, &inst);
    }
  }

  void modValue(const Value *v, const Instruction *inst,
                bool isReachableMemory) {
    auto pSet = ptrAnalysis.getPtsSet(v);
    for (const auto *loc : pSet) {
      if (isReachableMemory) {
        for (const auto *oLoc :
             ptrAnalysis.getMemoryManager().getReachableMemoryObjects(loc))
          store.insertBinding(oLoc, inst);
      } else {
        store.insertBinding(loc, inst);
      }
    }
  }

  void evalExternalCall(CallBase *cs, const Function *f) {
    const auto &summary = lookupModRefSummary(modRefTable, *f, unmodeledPolicy);

    for (auto const &effect : summary) {
      if (effect.isModEffect()) {
        auto const &pos = effect.getPosition();
        if (pos.isReturnPosition()) {
          modValue(cs->stripPointerCasts(), cs, effect.onReachableMemory());
        } else {
          auto const &argPos = pos.getAsArgPosition();
          unsigned idx = argPos.getArgIndex();

          if (!argPos.isAfterArgPosition()) {
            if (idx < cs->arg_size())
              modValue(cs->getArgOperand(idx)->stripPointerCasts(), cs,
                       effect.onReachableMemory());
          } else {
            for (auto i = idx, e = cs->arg_size(); i < e; ++i)
              if (cs->getArgOperand(i)->getType()->isPointerTy())
                modValue(cs->getArgOperand(i)->stripPointerCasts(), cs,
                         effect.onReachableMemory());
          }
        }
      }
    }
  }

public:
  EvalVisitor(const SemiSparsePointerAnalysis &p, const ModRefModuleSummary &sm,
              const ExternalModRefTable &e, const StoreType &s,
              UnmodeledPolicy policy)
      : ptrAnalysis(p), unmodeledPolicy(policy), summaryMap(sm), modRefTable(e),
        store(s) {}
  const StoreType &getStore() const { return store; }

  // Default case
  void visitInstruction(Instruction &) {}

  void visitAllocaInst(AllocaInst &allocInst) {
    auto pSet = ptrAnalysis.getPtsSet(&allocInst);
    for (const auto *obj : pSet)
      store.insertBinding(obj, &allocInst);
  }

  void visitStoreInst(StoreInst &storeInst) {
    auto *ptr = storeInst.getPointerOperand();
    evalStore(storeInst, ptr);
  }

  void visitAtomicRMWInst(AtomicRMWInst &inst) {
    evalStore(inst, inst.getPointerOperand());
  }
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &inst) {
    evalStore(inst, inst.getPointerOperand());
  }

  void visitCallBase(CallBase &call) { visitTaintCall(&call); }

  void visitTaintCall(CallBase *cs) {
    if (isDebugIntrinsic(*cs))
      return;
    auto callTgts = resolveCallTargets(ptrAnalysis, *cs, unmodeledPolicy);
    for (const auto *callee : callTgts) {
      if (callee->isDeclaration()) {
        evalExternalCall(cs, callee);
      } else {
        auto &summary = summaryMap.getSummary(callee);
        for (const auto *loc : summary.mem_writes())
          store.insertBinding(loc, cs);
      }
    }
  }
};

} // end of anonymous namespace

ReachingDefMap<Instruction>
ReachingDefModuleAnalysis::runOnFunction(const Function &func) {
  ReachingDefMap<Instruction> rdMap;

  const auto *entryInst = &func.getEntryBlock().front();
  auto &summary = summaryMap.getSummary(&func);
  auto &initStore = rdMap.getReachingDefStore(entryInst);
  for (const auto *loc : summary.mem_reads())
    initStore.insertBinding(loc, nullptr);
  // Write-only locations still need an entry definition on paths where no
  // write executes, so the caller's prior contents survive those paths.
  for (const auto *loc : summary.mem_writes())
    initStore.insertBinding(loc, nullptr);

  util::FIFOWorkList<const Instruction *> workList;
  workList.enqueue(entryInst);

  while (!workList.empty()) {
    const auto *inst = workList.dequeue();

    EvalVisitor evalVisitor(ptrAnalysis, summaryMap, modRefTable,
                            rdMap.getReachingDefStore(inst), unmodeledPolicy);
    evalVisitor.visit(const_cast<Instruction &>(*inst));

    if (inst->isTerminator()) {
      for (auto i = 0u, e = inst->getNumSuccessors(); i != e; ++i) {
        auto *succInst = &inst->getSuccessor(i)->front();
        if (rdMap.update(succInst, evalVisitor.getStore()))
          workList.enqueue(succInst);
      }
    } else {
      const auto *nextInst = getNextInstruction(inst);
      if (rdMap.update(nextInst, evalVisitor.getStore()))
        workList.enqueue(nextInst);
    }
  }

  return rdMap;
}

void ReachingDefModuleAnalysis::buildMemoryDefUseEdges(
    DefUseFunction &duFunc) const {
  const auto &func = duFunc.getFunction();
  if (func.empty())
    return;

  // Phase A: Collect reachable basic blocks in Reverse Post-Order (RPO).
  llvm::ReversePostOrderTraversal<const Function *> rpot(&func);
  std::vector<const BasicBlock *> rpoBlocks(rpot.begin(), rpot.end());
  const uint32_t numBB = static_cast<uint32_t>(rpoBlocks.size());
  if (numBB == 0)
    return;

  llvm::DenseMap<const BasicBlock *, uint32_t> bbToIdx;
  bbToIdx.reserve(numBB);
  for (uint32_t b = 0; b < numBB; ++b)
    bbToIdx.try_emplace(rpoBlocks[b], b);

  std::vector<llvm::SmallVector<uint32_t, 4>> bbSuccs(numBB);
  for (uint32_t b = 0; b < numBB; ++b) {
    auto &succs = bbSuccs[b];
    for (const auto *succBB : llvm::successors(rpoBlocks[b])) {
      auto itr = bbToIdx.find(succBB);
      if (itr != bbToIdx.end())
        succs.push_back(itr->second);
    }
    std::sort(succs.begin(), succs.end());
    succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
  }

  // Phase B: Single linear scan over reachable instructions to intern memory
  // objects and precompute all read/write sites once.
  const auto &funcSummary = summaryMap.getSummary(&func);
  const auto &memManager = ptrAnalysis.getMemoryManager();

  std::vector<const MemoryObject *> idToLoc;
  llvm::DenseMap<const MemoryObject *, uint32_t> locToId;
  std::vector<uint8_t> hasEntryDef;
  std::vector<uint8_t> isReadOrExit;
  std::vector<uint8_t> isExitWrite;
  std::vector<uint32_t> readStamp;
  std::vector<uint32_t> writeStamp;
  uint32_t curReadStamp = 0;
  uint32_t curWriteStamp = 0;

  auto getOrCreateLoc = [&](const MemoryObject *loc) -> uint32_t {
    auto res = locToId.try_emplace(loc, static_cast<uint32_t>(idToLoc.size()));
    if (res.second) {
      idToLoc.push_back(loc);
      hasEntryDef.push_back(0);
      isReadOrExit.push_back(0);
      isExitWrite.push_back(0);
      readStamp.push_back(0);
      writeStamp.push_back(0);
    }
    return res.first->second;
  };

  for (const auto *loc : funcSummary.mem_reads()) {
    uint32_t id = getOrCreateLoc(loc);
    hasEntryDef[id] = 1;
  }
  const bool hasExits = !duFunc.getMemoryExitInsts().empty();
  for (const auto *loc : funcSummary.mem_writes()) {
    uint32_t id = getOrCreateLoc(loc);
    hasEntryDef[id] = 1;
    if (hasExits) {
      isReadOrExit[id] = 1;
      isExitWrite[id] = 1;
    }
  }

  DefUseInstruction *entryDu = duFunc.getEntryInst();
  std::vector<DefUseInstruction *> idToDefDu;
  idToDefDu.push_back(entryDu); // defId 0 = function entry definition

  struct ReadSite {
    uint32_t instIdx;
    DefUseInstruction *dstDu;
    uint32_t begin;
    uint32_t end;
  };
  struct WriteSite {
    uint32_t instIdx;
    uint32_t defId;
    bool isStrong;
    uint32_t begin;
    uint32_t end;
  };
  struct CalleeSlice {
    uint32_t rBegin = 0;
    uint32_t rEnd = 0;
    uint32_t wBegin = 0;
    uint32_t wEnd = 0;
  };
  struct CalleeListHash {
    size_t operator()(const std::vector<const Function *> &v) const {
      size_t h = v.size();
      for (const Function *f : v) {
        size_t x = reinterpret_cast<uintptr_t>(f) >> 3;
        h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      }
      return h;
    }
  };

  std::vector<uint32_t> readLocPool;
  std::vector<uint32_t> writeLocPool;
  std::vector<std::vector<ReadSite>> bbReads(numBB);
  std::vector<std::vector<WriteSite>> bbWrites(numBB);
  std::vector<DefUseInstruction *> bbExitDu(numBB, nullptr);
  llvm::DenseMap<const Function *, CalleeSlice> calleeSliceCache;
  std::unordered_map<std::vector<const Function *>, CalleeSlice, CalleeListHash>
      multiCalleeSliceCache;

  auto pushReadLoc = [&](uint32_t id) {
    if (readStamp[id] != curReadStamp) {
      readStamp[id] = curReadStamp;
      readLocPool.push_back(id);
    }
  };
  auto pushWriteLoc = [&](uint32_t id) {
    if (writeStamp[id] != curWriteStamp) {
      writeStamp[id] = curWriteStamp;
      writeLocPool.push_back(id);
    }
  };

  auto appendPtsLocs = [&](const Value *v, bool isReachable, bool isWrite) {
    auto pSet = ptrAnalysis.getPtsSet(v);
    for (const auto *obj : pSet) {
      if (isReachable) {
        for (const auto *oLoc : memManager.getReachableMemoryObjects(obj)) {
          uint32_t id = getOrCreateLoc(oLoc);
          if (isWrite)
            pushWriteLoc(id);
          else
            pushReadLoc(id);
        }
      } else {
        uint32_t id = getOrCreateLoc(obj);
        if (isWrite)
          pushWriteLoc(id);
        else
          pushReadLoc(id);
      }
    }
  };

  auto getOrBuildCalleeSlice = [&](const Function *callee) -> CalleeSlice {
    auto itr = calleeSliceCache.find(callee);
    if (itr != calleeSliceCache.end())
      return itr->second;
    const auto &calleeSummary = summaryMap.getSummary(callee);
    CalleeSlice slice;
    ++curReadStamp;
    ++curWriteStamp;
    slice.rBegin = static_cast<uint32_t>(readLocPool.size());
    slice.wBegin = static_cast<uint32_t>(writeLocPool.size());
    for (const auto *obj : calleeSummary.mem_reads())
      pushReadLoc(getOrCreateLoc(obj));
    for (const auto *obj : calleeSummary.mem_writes()) {
      uint32_t id = getOrCreateLoc(obj);
      pushReadLoc(id);
      pushWriteLoc(id);
    }
    slice.rEnd = static_cast<uint32_t>(readLocPool.size());
    slice.wEnd = static_cast<uint32_t>(writeLocPool.size());
    for (uint32_t i = slice.rBegin; i < slice.rEnd; ++i)
      isReadOrExit[readLocPool[i]] = 1;
    calleeSliceCache.try_emplace(callee, slice);
    return slice;
  };

  for (uint32_t b = 0; b < numBB; ++b) {
    uint32_t instIdx = 0;
    for (const auto &inst : *rpoBlocks[b]) {
      const uint32_t curIdx = instIdx++;
      if (isDebugIntrinsic(inst))
        continue;
      DefUseInstruction *duInst = duFunc.getDefUseInstruction(&inst);
      if (!duInst)
        continue;

      if (isa<ReturnInst>(&inst) || isa<ResumeInst>(&inst)) {
        bbExitDu[b] = duInst;
        continue;
      }

      if (const auto *allocInst = dyn_cast<AllocaInst>(&inst)) {
        ++curWriteStamp;
        uint32_t wBegin = static_cast<uint32_t>(writeLocPool.size());
        appendPtsLocs(allocInst, false, true);
        uint32_t wEnd = static_cast<uint32_t>(writeLocPool.size());
        if (wEnd > wBegin) {
          uint32_t defId = static_cast<uint32_t>(idToDefDu.size());
          idToDefDu.push_back(duInst);
          bbWrites[b].push_back({curIdx, defId, false, wBegin, wEnd});
        }
        continue;
      }

      if (const auto *loadInst = dyn_cast<LoadInst>(&inst)) {
        ++curReadStamp;
        uint32_t rBegin = static_cast<uint32_t>(readLocPool.size());
        appendPtsLocs(loadInst->getPointerOperand(), false, false);
        uint32_t rEnd = static_cast<uint32_t>(readLocPool.size());
        if (rEnd > rBegin) {
          for (uint32_t i = rBegin; i < rEnd; ++i)
            isReadOrExit[readLocPool[i]] = 1;
          bbReads[b].push_back({curIdx, duInst, rBegin, rEnd});
        }
        continue;
      }

      if (const auto *storeInst = dyn_cast<StoreInst>(&inst)) {
        auto pSet = ptrAnalysis.getPtsSet(storeInst->getPointerOperand());
        if (!pSet.empty()) {
          ++curWriteStamp;
          uint32_t wBegin = static_cast<uint32_t>(writeLocPool.size());
          bool strong =
              (pSet.size() == 1 && !(*pSet.begin())->isSummaryObject());
          for (const auto *obj : pSet)
            pushWriteLoc(getOrCreateLoc(obj));
          uint32_t wEnd = static_cast<uint32_t>(writeLocPool.size());
          uint32_t defId = static_cast<uint32_t>(idToDefDu.size());
          idToDefDu.push_back(duInst);
          bbWrites[b].push_back({curIdx, defId, strong, wBegin, wEnd});
        }
        continue;
      }

      if (isa<AtomicRMWInst>(&inst) || isa<AtomicCmpXchgInst>(&inst)) {
        const Value *ptr =
            isa<AtomicRMWInst>(&inst)
                ? cast<AtomicRMWInst>(&inst)->getPointerOperand()
                : cast<AtomicCmpXchgInst>(&inst)->getPointerOperand();
        auto pSet = ptrAnalysis.getPtsSet(ptr);
        if (!pSet.empty()) {
          ++curReadStamp;
          ++curWriteStamp;
          uint32_t rBegin = static_cast<uint32_t>(readLocPool.size());
          uint32_t wBegin = static_cast<uint32_t>(writeLocPool.size());
          bool strong =
              (pSet.size() == 1 && !(*pSet.begin())->isSummaryObject());
          for (const auto *obj : pSet) {
            uint32_t id = getOrCreateLoc(obj);
            pushReadLoc(id);
            pushWriteLoc(id);
          }
          uint32_t rEnd = static_cast<uint32_t>(readLocPool.size());
          uint32_t wEnd = static_cast<uint32_t>(writeLocPool.size());
          for (uint32_t i = rBegin; i < rEnd; ++i)
            isReadOrExit[readLocPool[i]] = 1;
          bbReads[b].push_back({curIdx, duInst, rBegin, rEnd});
          uint32_t defId = static_cast<uint32_t>(idToDefDu.size());
          idToDefDu.push_back(duInst);
          bbWrites[b].push_back({curIdx, defId, strong, wBegin, wEnd});
        }
        continue;
      }

      if (const auto *cs = dyn_cast<CallBase>(&inst)) {
        auto callTgts = resolveCallTargets(ptrAnalysis, *cs, unmodeledPolicy);
        if (callTgts.empty())
          continue;

        if (callTgts.size() == 1 && !callTgts[0]->isDeclaration()) {
          CalleeSlice slice = getOrBuildCalleeSlice(callTgts[0]);
          if (slice.rEnd > slice.rBegin)
            bbReads[b].push_back({curIdx, duInst, slice.rBegin, slice.rEnd});
          if (slice.wEnd > slice.wBegin) {
            uint32_t defId = static_cast<uint32_t>(idToDefDu.size());
            idToDefDu.push_back(duInst);
            bbWrites[b].push_back(
                {curIdx, defId, false, slice.wBegin, slice.wEnd});
          }
          continue;
        }

        bool allInternal = true;
        for (const auto *callee : callTgts) {
          if (callee->isDeclaration()) {
            allInternal = false;
            break;
          }
        }
        if (allInternal && callTgts.size() > 1) {
          std::sort(callTgts.begin(), callTgts.end());
          callTgts.erase(std::unique(callTgts.begin(), callTgts.end()),
                         callTgts.end());
          auto mItr = multiCalleeSliceCache.find(callTgts);
          CalleeSlice slice;
          if (mItr != multiCalleeSliceCache.end()) {
            slice = mItr->second;
          } else {
            // Materialize each callee slice first so readLocPool/writeLocPool
            // ranges do not interleave with our merged slice.
            llvm::SmallVector<CalleeSlice, 8> childSlices;
            childSlices.reserve(callTgts.size());
            for (const auto *callee : callTgts)
              childSlices.push_back(getOrBuildCalleeSlice(callee));
            ++curReadStamp;
            ++curWriteStamp;
            slice.rBegin = static_cast<uint32_t>(readLocPool.size());
            slice.wBegin = static_cast<uint32_t>(writeLocPool.size());
            for (const auto &child : childSlices) {
              for (uint32_t i = child.rBegin; i < child.rEnd; ++i)
                pushReadLoc(readLocPool[i]);
              for (uint32_t i = child.wBegin; i < child.wEnd; ++i)
                pushWriteLoc(writeLocPool[i]);
            }
            slice.rEnd = static_cast<uint32_t>(readLocPool.size());
            slice.wEnd = static_cast<uint32_t>(writeLocPool.size());
            for (uint32_t i = slice.rBegin; i < slice.rEnd; ++i)
              isReadOrExit[readLocPool[i]] = 1;
            multiCalleeSliceCache.emplace(std::move(callTgts), slice);
          }
          if (slice.rEnd > slice.rBegin)
            bbReads[b].push_back({curIdx, duInst, slice.rBegin, slice.rEnd});
          if (slice.wEnd > slice.wBegin) {
            uint32_t defId = static_cast<uint32_t>(idToDefDu.size());
            idToDefDu.push_back(duInst);
            bbWrites[b].push_back(
                {curIdx, defId, false, slice.wBegin, slice.wEnd});
          }
          continue;
        }

        // Mixed or external call targets: pre-build any internal callee slices
        // before opening the merged read/write pool slices.
        llvm::SmallVector<CalleeSlice, 4> internalSlices;
        for (const auto *callee : callTgts)
          if (!callee->isDeclaration())
            internalSlices.push_back(getOrBuildCalleeSlice(callee));

        ++curReadStamp;
        ++curWriteStamp;
        uint32_t rBegin = static_cast<uint32_t>(readLocPool.size());
        uint32_t wBegin = static_cast<uint32_t>(writeLocPool.size());

        for (const auto &child : internalSlices) {
          for (uint32_t i = child.rBegin; i < child.rEnd; ++i)
            pushReadLoc(readLocPool[i]);
          for (uint32_t i = child.wBegin; i < child.wEnd; ++i)
            pushWriteLoc(writeLocPool[i]);
        }

        for (const auto *callee : callTgts) {
          if (!callee->isDeclaration())
            continue;
          const auto &extSummary =
              lookupModRefSummary(modRefTable, *callee, unmodeledPolicy);
          for (const auto &effect : extSummary) {
            const auto &pos = effect.getPosition();
            if (effect.isRefEffect()) {
              if (pos.isReturnPosition())
                continue;
              const auto &argPos = pos.getAsArgPosition();
              unsigned idx = argPos.getArgIndex();
              if (!argPos.isAfterArgPosition()) {
                if (idx < cs->arg_size())
                  appendPtsLocs(cs->getArgOperand(idx),
                                effect.onReachableMemory(), false);
              } else {
                for (unsigned i = idx, e = cs->arg_size(); i < e; ++i)
                  if (cs->getArgOperand(i)->getType()->isPointerTy())
                    appendPtsLocs(cs->getArgOperand(i),
                                  effect.onReachableMemory(), false);
              }
            } else if (effect.isModEffect()) {
              if (pos.isReturnPosition()) {
                appendPtsLocs(cs->stripPointerCasts(),
                              effect.onReachableMemory(), true);
              } else {
                const auto &argPos = pos.getAsArgPosition();
                unsigned idx = argPos.getArgIndex();
                if (!argPos.isAfterArgPosition()) {
                  if (idx < cs->arg_size())
                    appendPtsLocs(cs->getArgOperand(idx)->stripPointerCasts(),
                                  effect.onReachableMemory(), true);
                } else {
                  for (unsigned i = idx, e = cs->arg_size(); i < e; ++i)
                    if (cs->getArgOperand(i)->getType()->isPointerTy())
                      appendPtsLocs(
                          cs->getArgOperand(i)->stripPointerCasts(),
                          effect.onReachableMemory(), true);
                }
              }
            }
          }
        }

        uint32_t rEnd = static_cast<uint32_t>(readLocPool.size());
        if (rEnd > rBegin) {
          for (uint32_t i = rBegin; i < rEnd; ++i)
            isReadOrExit[readLocPool[i]] = 1;
          bbReads[b].push_back({curIdx, duInst, rBegin, rEnd});
        }
        uint32_t wEnd = static_cast<uint32_t>(writeLocPool.size());
        if (wEnd > wBegin) {
          uint32_t defId = static_cast<uint32_t>(idToDefDu.size());
          idToDefDu.push_back(duInst);
          bbWrites[b].push_back({curIdx, defId, false, wBegin, wEnd});
        }
      }
    }
  }

  const uint32_t numLocs = static_cast<uint32_t>(idToLoc.size());
  if (numLocs == 0)
    return;

  // Phase B2: Exact linear-time Paige-Tarjan partition refinement over
  // (hasEntryDef, isExitWrite, all WriteSites, all ReadSites). Locations in
  // the same final group have identical def-use edges everywhere in `func`, so
  // Phases C and D run once per group and emit one shared edge set per group.
  // When `enableMemoryGrouping` is false (ablation mode), every live location
  // remains an individual ungrouped singleton.
  constexpr uint32_t kDeadGroup = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> locGroup(numLocs, kDeadGroup);
  std::vector<uint32_t> groupSize;
  std::vector<uint8_t> groupEntryDef;
  std::vector<uint8_t> groupExitWrite;
  std::vector<const MemoryObject *> singleGroupLoc;

  if (enableMemoryGrouping) {
    uint32_t initGroup[4] = {kDeadGroup, kDeadGroup, kDeadGroup, kDeadGroup};
    for (uint32_t locId = 0; locId < numLocs; ++locId) {
      if (!isReadOrExit[locId])
        continue;
      uint32_t key =
          (hasEntryDef[locId] ? 1u : 0u) | (isExitWrite[locId] ? 2u : 0u);
      if (initGroup[key] == kDeadGroup) {
        initGroup[key] = static_cast<uint32_t>(groupSize.size());
        groupSize.push_back(0);
        groupEntryDef.push_back(hasEntryDef[locId]);
        groupExitWrite.push_back(isExitWrite[locId]);
      }
      uint32_t g = initGroup[key];
      locGroup[locId] = g;
      ++groupSize[g];
    }
    if (groupSize.empty())
      return;

    std::vector<uint32_t> touchedCount(groupSize.size(), 0);
    std::vector<uint32_t> splitTarget(groupSize.size(), kDeadGroup);
    std::vector<uint32_t> touchedGroups;

    auto refineBySlice = [&](const std::vector<uint32_t> &pool, uint32_t begin,
                             uint32_t end) {
      touchedGroups.clear();
      for (uint32_t i = begin; i < end; ++i) {
        uint32_t g = locGroup[pool[i]];
        if (g == kDeadGroup)
          continue;
        if (touchedCount[g] == 0)
          touchedGroups.push_back(g);
        ++touchedCount[g];
      }
      bool anySplit = false;
      for (uint32_t g : touchedGroups) {
        if (touchedCount[g] < groupSize[g]) {
          uint32_t newG = static_cast<uint32_t>(groupSize.size());
          groupSize.push_back(touchedCount[g]);
          groupSize[g] -= touchedCount[g];
          groupEntryDef.push_back(groupEntryDef[g]);
          groupExitWrite.push_back(groupExitWrite[g]);
          touchedCount.push_back(0);
          splitTarget.push_back(kDeadGroup);
          splitTarget[g] = newG;
          anySplit = true;
        }
      }
      if (anySplit) {
        for (uint32_t i = begin; i < end; ++i) {
          uint32_t locId = pool[i];
          uint32_t g = locGroup[locId];
          if (g != kDeadGroup && splitTarget[g] != kDeadGroup)
            locGroup[locId] = splitTarget[g];
        }
      }
      for (uint32_t g : touchedGroups) {
        touchedCount[g] = 0;
        splitTarget[g] = kDeadGroup;
      }
    };

    llvm::DenseSet<std::pair<uint32_t, uint32_t>> refinedWriteSlices;
    llvm::DenseSet<std::pair<uint32_t, uint32_t>> refinedReadSlices;
    for (uint32_t b = 0; b < numBB; ++b) {
      for (const auto &ws : bbWrites[b])
        if (refinedWriteSlices.insert({ws.begin, ws.end}).second)
          refineBySlice(writeLocPool, ws.begin, ws.end);
      for (const auto &rs : bbReads[b])
        if (refinedReadSlices.insert({rs.begin, rs.end}).second)
          refineBySlice(readLocPool, rs.begin, rs.end);
    }
  } else {
    groupSize.reserve(numLocs);
    groupEntryDef.reserve(numLocs);
    groupExitWrite.reserve(numLocs);
    singleGroupLoc.reserve(numLocs);
    for (uint32_t locId = 0; locId < numLocs; ++locId) {
      if (!isReadOrExit[locId])
        continue;
      uint32_t g = static_cast<uint32_t>(groupSize.size());
      locGroup[locId] = g;
      groupSize.push_back(1);
      groupEntryDef.push_back(hasEntryDef[locId]);
      groupExitWrite.push_back(isExitWrite[locId]);
      singleGroupLoc.push_back(idToLoc[locId]);
    }
    if (groupSize.empty())
      return;
  }

  const uint32_t numGroups = static_cast<uint32_t>(groupSize.size());
  auto &memGroups = duFunc.getMemoryGroups();
  memGroups.clear();
  if (enableMemoryGrouping) {
    memGroups.groupLocs.resize(numGroups);
    memGroups.locToGroup.reserve(numLocs);
    for (uint32_t locId = 0; locId < numLocs; ++locId) {
      uint32_t g = locGroup[locId];
      if (g == kDeadGroup)
        continue;
      const MemoryObject *loc = idToLoc[locId];
      memGroups.locToGroup.try_emplace(loc, g);
      memGroups.groupLocs[g].push_back(loc);
    }
    memGroups.buildChunkMasks();
  }

  // Collapse every WriteSite and ReadSite from location IDs to group IDs,
  // caching shared pool slices so repeated calls collapse only once.
  std::vector<uint32_t> writeGroupPool;
  std::vector<uint32_t> readGroupPool;
  {
    std::vector<uint32_t> groupStamp(numGroups, 0);
    uint32_t stamp = 0;
    llvm::DenseMap<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>>
        writeCollapseCache;
    llvm::DenseMap<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>>
        readCollapseCache;
    for (uint32_t b = 0; b < numBB; ++b) {
      for (auto &ws : bbWrites[b]) {
        auto key = std::make_pair(ws.begin, ws.end);
        auto itr = writeCollapseCache.find(key);
        if (itr != writeCollapseCache.end()) {
          ws.begin = itr->second.first;
          ws.end = itr->second.second;
          continue;
        }
        ++stamp;
        uint32_t gBegin = static_cast<uint32_t>(writeGroupPool.size());
        for (uint32_t i = ws.begin; i < ws.end; ++i) {
          uint32_t g = locGroup[writeLocPool[i]];
          if (g != kDeadGroup && groupStamp[g] != stamp) {
            groupStamp[g] = stamp;
            writeGroupPool.push_back(g);
          }
        }
        uint32_t gEnd = static_cast<uint32_t>(writeGroupPool.size());
        writeCollapseCache.try_emplace(key, std::make_pair(gBegin, gEnd));
        ws.begin = gBegin;
        ws.end = gEnd;
      }
      for (auto &rs : bbReads[b]) {
        auto key = std::make_pair(rs.begin, rs.end);
        auto itr = readCollapseCache.find(key);
        if (itr != readCollapseCache.end()) {
          rs.begin = itr->second.first;
          rs.end = itr->second.second;
          continue;
        }
        ++stamp;
        uint32_t gBegin = static_cast<uint32_t>(readGroupPool.size());
        for (uint32_t i = rs.begin; i < rs.end; ++i) {
          uint32_t g = locGroup[readLocPool[i]];
          if (g != kDeadGroup && groupStamp[g] != stamp) {
            groupStamp[g] = stamp;
            readGroupPool.push_back(g);
          }
        }
        uint32_t gEnd = static_cast<uint32_t>(readGroupPool.size());
        readCollapseCache.try_emplace(key, std::make_pair(gBegin, gEnd));
        rs.begin = gBegin;
        rs.end = gEnd;
      }
    }
  }
  std::vector<uint32_t> exitWriteGroups;
  for (uint32_t g = 0; g < numGroups; ++g)
    if (groupExitWrite[g])
      exitWriteGroups.push_back(g);

  // Phase C: Build per-BB kill/gen summaries per equivalence group, coalesce
  // groups with identical write signatures, and solve exact BB-level reaching
  // definitions with packed uint64_t bitvectors.
  struct BlockWrite {
    uint32_t bbId;
    bool kill;
    llvm::SmallVector<uint32_t, 4> genDefs;

    bool operator==(const BlockWrite &rhs) const {
      return bbId == rhs.bbId && kill == rhs.kill && genDefs == rhs.genDefs;
    }
  };
  struct LocSignature {
    bool entryDef;
    std::vector<BlockWrite> writes;

    bool operator==(const LocSignature &rhs) const {
      return entryDef == rhs.entryDef && writes == rhs.writes;
    }
  };
  struct LocSignatureHash {
    size_t operator()(const LocSignature &sig) const {
      size_t h = sig.entryDef ? 0x9e3779b97f4a7c15ULL : 0ULL;
      auto mix = [&](size_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      };
      mix(sig.writes.size());
      for (const auto &bw : sig.writes) {
        mix((static_cast<size_t>(bw.bbId) << 1) | (bw.kill ? 1u : 0u));
        mix(bw.genDefs.size());
        for (uint32_t d : bw.genDefs)
          mix(d);
      }
      return h;
    }
  };

  std::vector<std::vector<BlockWrite>> groupBBWrites(numGroups);
  {
    std::vector<uint8_t> touched(numGroups, 0);
    std::vector<uint8_t> killFlag(numGroups, 0);
    std::vector<llvm::SmallVector<uint32_t, 4>> genTmp(numGroups);
    std::vector<uint32_t> touchedList;

    for (uint32_t b = 0; b < numBB; ++b) {
      if (bbWrites[b].empty())
        continue;
      for (const auto &ws : bbWrites[b]) {
        for (uint32_t i = ws.begin; i < ws.end; ++i) {
          uint32_t g = writeGroupPool[i];
          if (!touched[g]) {
            touched[g] = 1;
            touchedList.push_back(g);
          }
          if (ws.isStrong) {
            killFlag[g] = 1;
            genTmp[g].clear();
            genTmp[g].push_back(ws.defId);
          } else {
            genTmp[g].push_back(ws.defId);
          }
        }
      }
      for (uint32_t g : touchedList) {
        auto &defs = genTmp[g];
        defs.erase(std::unique(defs.begin(), defs.end()), defs.end());
        groupBBWrites[g].push_back({b, killFlag[g] != 0, std::move(defs)});
        touched[g] = 0;
        killFlag[g] = 0;
        genTmp[g].clear();
      }
      touchedList.clear();
    }
  }

  constexpr uint32_t kNoWriteClass = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> groupToWriteClass(numGroups, kNoWriteClass);

  struct ClassSolution {
    std::vector<DefUseInstruction *> bitToDu;
    size_t wordsPerBB = 0;
    std::vector<uint64_t> bbIn; // size = numBB * wordsPerBB
  };
  std::vector<ClassSolution> classSolutions;
  std::unordered_map<LocSignature, uint32_t, LocSignatureHash> sigToClass;

  std::vector<int32_t> bbWriteIdx(numBB, -1);
  std::vector<uint8_t> dirty(numBB, 0);
  std::vector<uint64_t> outBuf;

  for (uint32_t g = 0; g < numGroups; ++g) {
    if (groupBBWrites[g].empty())
      continue;

    LocSignature sig{groupEntryDef[g] != 0, std::move(groupBBWrites[g])};
    if (enableMemoryGrouping) {
      auto itr = sigToClass.find(sig);
      if (itr != sigToClass.end()) {
        groupToWriteClass[g] = itr->second;
        continue;
      }
    }

    const uint32_t classId = static_cast<uint32_t>(classSolutions.size());
    groupToWriteClass[g] = classId;

    ClassSolution sol;
    llvm::DenseMap<uint32_t, uint32_t> defToBit;
    if (sig.entryDef) {
      defToBit.try_emplace(0u, 0u);
      sol.bitToDu.push_back(entryDu);
    }
    for (const auto &bw : sig.writes) {
      for (uint32_t d : bw.genDefs) {
        auto res =
            defToBit.try_emplace(d, static_cast<uint32_t>(sol.bitToDu.size()));
        if (res.second)
          sol.bitToDu.push_back(idToDefDu[d]);
      }
    }

    const size_t numBits = sol.bitToDu.size();
    const size_t W = (numBits + 63) / 64;
    sol.wordsPerBB = W;
    sol.bbIn.assign(static_cast<size_t>(numBB) * W, 0ULL);

    const size_t numWriteBBs = sig.writes.size();
    std::vector<uint64_t> genBits(numWriteBBs * W, 0ULL);
    for (size_t idx = 0; idx < numWriteBBs; ++idx) {
      const auto &bw = sig.writes[idx];
      bbWriteIdx[bw.bbId] = static_cast<int32_t>(idx);
      uint64_t *gPtr = &genBits[idx * W];
      for (uint32_t d : bw.genDefs) {
        uint32_t bit = defToBit.find(d)->second;
        gPtr[bit >> 6] |= (1ULL << (bit & 63u));
      }
      dirty[bw.bbId] = 1;
    }
    if (sig.entryDef) {
      sol.bbIn[0] |= 1ULL;
      dirty[0] = 1;
    }

    outBuf.resize(W);
    bool anyDirty = true;
    while (anyDirty) {
      anyDirty = false;
      for (uint32_t b = 0; b < numBB; ++b) {
        if (!dirty[b])
          continue;
        dirty[b] = 0;

        const uint64_t *inPtr = &sol.bbIn[static_cast<size_t>(b) * W];
        const int32_t wIdx = bbWriteIdx[b];
        if (wIdx >= 0) {
          const uint64_t *genPtr = &genBits[static_cast<size_t>(wIdx) * W];
          if (sig.writes[wIdx].kill) {
            for (size_t w = 0; w < W; ++w)
              outBuf[w] = genPtr[w];
          } else {
            for (size_t w = 0; w < W; ++w)
              outBuf[w] = inPtr[w] | genPtr[w];
          }
        } else {
          for (size_t w = 0; w < W; ++w)
            outBuf[w] = inPtr[w];
        }

        for (uint32_t s : bbSuccs[b]) {
          uint64_t *sIn = &sol.bbIn[static_cast<size_t>(s) * W];
          uint64_t diff = 0ULL;
          for (size_t w = 0; w < W; ++w) {
            uint64_t prev = sIn[w];
            uint64_t next = prev | outBuf[w];
            sIn[w] = next;
            diff |= (next ^ prev);
          }
          if (diff) {
            dirty[s] = 1;
            anyDirty = true;
          }
        }
      }
    }

    for (const auto &bw : sig.writes)
      bbWriteIdx[bw.bbId] = -1;

    classSolutions.push_back(std::move(sol));
    if (enableMemoryGrouping)
      sigToClass.emplace(std::move(sig), classId);
  }

  // Phase D: Stream each reachable BB once over equivalence groups and emit
  // one shared edge set per group directly into `duFunc`.
  const size_t numClasses = classSolutions.size();
  std::vector<llvm::SmallVector<DefUseInstruction *, 8>> decodedIn(numClasses);
  std::vector<uint32_t> decodedStamp(numClasses, 0);

  std::vector<uint8_t> localTouched(numGroups, 0);
  std::vector<uint8_t> localKilled(numGroups, 0);
  std::vector<llvm::SmallVector<uint32_t, 4>> localDefs(numGroups);
  std::vector<uint32_t> touchedGroupsList;

  auto getDecodedIn =
      [&](uint32_t classId,
          uint32_t b) -> const llvm::SmallVector<DefUseInstruction *, 8> & {
    auto &vec = decodedIn[classId];
    if (decodedStamp[classId] == b + 1)
      return vec;
    decodedStamp[classId] = b + 1;
    vec.clear();
    const auto &sol = classSolutions[classId];
    const size_t W = sol.wordsPerBB;
    const uint64_t *inPtr = &sol.bbIn[static_cast<size_t>(b) * W];
    for (size_t w = 0; w < W; ++w) {
      uint64_t bits = inPtr[w];
      while (bits) {
        unsigned tz = static_cast<unsigned>(__builtin_ctzll(bits));
        vec.push_back(sol.bitToDu[(w << 6) + tz]);
        bits &= (bits - 1);
      }
    }
    return vec;
  };

  auto emitEdge = [&](DefUseInstruction *srcDu, uint32_t g,
                      DefUseInstruction *dstDu) {
    if (enableMemoryGrouping)
      srcDu->appendGroupMemSuccEdge(g, dstDu);
    else
      srcDu->appendMemSuccEdge(singleGroupLoc[g], dstDu);
  };

  auto emitForGroup = [&](uint32_t g, uint32_t b, DefUseInstruction *dstDu) {
    const uint32_t c = groupToWriteClass[g];
    if (c == kNoWriteClass) {
      emitEdge(entryDu, g, dstDu);
      return;
    }

    bool emitted = false;
    if (!localKilled[g]) {
      const auto &inDefs = getDecodedIn(c, b);
      for (DefUseInstruction *srcDu : inDefs) {
        emitEdge(srcDu, g, dstDu);
        emitted = true;
      }
    }
    for (uint32_t d : localDefs[g]) {
      emitEdge(idToDefDu[d], g, dstDu);
      emitted = true;
    }
    if (!emitted)
      emitEdge(entryDu, g, dstDu);
  };

  for (uint32_t b = 0; b < numBB; ++b) {
    const auto &reads = bbReads[b];
    const auto &writes = bbWrites[b];
    size_t ri = 0, wi = 0;

    while (ri < reads.size() || wi < writes.size()) {
      // Reads at an instruction observe the reaching-def state BEFORE any
      // writes performed by the same instruction (e.g., atomics and calls).
      if (ri < reads.size() &&
          (wi == writes.size() || reads[ri].instIdx <= writes[wi].instIdx)) {
        const auto &rs = reads[ri++];
        for (uint32_t i = rs.begin; i < rs.end; ++i)
          emitForGroup(readGroupPool[i], b, rs.dstDu);
      } else {
        const auto &ws = writes[wi++];
        for (uint32_t i = ws.begin; i < ws.end; ++i) {
          uint32_t g = writeGroupPool[i];
          if (!localTouched[g]) {
            localTouched[g] = 1;
            touchedGroupsList.push_back(g);
          }
          if (ws.isStrong) {
            localKilled[g] = 1;
            localDefs[g].clear();
            localDefs[g].push_back(ws.defId);
          } else {
            localDefs[g].push_back(ws.defId);
          }
        }
      }
    }

    if (DefUseInstruction *exitDu = bbExitDu[b]) {
      for (uint32_t g : exitWriteGroups)
        emitForGroup(g, b, exitDu);
    }

    for (uint32_t g : touchedGroupsList) {
      localTouched[g] = 0;
      localKilled[g] = 0;
      localDefs[g].clear();
    }
    touchedGroupsList.clear();
  }

  duFunc.finalizeMemSuccEdges();
}

} // namespace tpa::taint