#include "Checker/TPA/FrontEnd/ModRefModuleAnalysis.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Program/Reachability.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;
using namespace tpa;

namespace tpa::taint {

using CallGraphMap =
    llvm::DenseMap<const Function *, llvm::SmallVector<const Function *, 8>>;

namespace {

inline const Function *getStackOwnerFunction(const MemoryObject *loc) {
  auto const &allocSite = loc->getAllocSite();
  if (allocSite.getAllocType() != AllocSiteTag::Stack)
    return nullptr;
  if (const auto *allocInst = dyn_cast<AllocaInst>(allocSite.getLocalValue()))
    return allocInst->getParent()->getParent();
  return nullptr;
}

inline bool isLocalStackLocation(const MemoryObject *loc, const Function *f) {
  auto const &allocSite = loc->getAllocSite();

  switch (allocSite.getAllocType()) {
  // NullObj and UniversalObj are counted as local stack location because we
  // don't want them to appear in the mod-ref summaries
  case AllocSiteTag::Null:
  case AllocSiteTag::Universal:
    return true;
  case AllocSiteTag::Global:
  case AllocSiteTag::Function:
  case AllocSiteTag::Heap:
    return false;
  case AllocSiteTag::Stack: {
    if (const auto *allocInst = dyn_cast<AllocaInst>(allocSite.getLocalValue()))
      return allocInst->getParent()->getParent() == f;
    else
      return false;
  }
  }
}

class SummaryInstVisitor : public InstVisitor<SummaryInstVisitor> {
private:
  const SemiSparsePointerAnalysis &ptrAnalysis;

  ModRefFunctionSummary &summary;

public:
  SummaryInstVisitor(const SemiSparsePointerAnalysis &p,
                     ModRefFunctionSummary &s)
      : ptrAnalysis(p), summary(s) {}

  void visitLoadInst(LoadInst &loadInst) {
    auto *ptrOp = loadInst.getPointerOperand();
    if (isa<GlobalValue>(ptrOp))
      summary.addValueRead(ptrOp->stripPointerCasts());
    auto pSet = ptrAnalysis.getPtsSet(ptrOp);
    for (const auto *obj : pSet) {
      if (!isLocalStackLocation(obj, loadInst.getParent()->getParent()))
        summary.addMemoryRead(obj);
    }
  }

  void visitStoreInst(StoreInst &storeInst) {
    auto *storeSrc = storeInst.getValueOperand();
    auto *storeDst = storeInst.getPointerOperand();

    if (isa<GlobalValue>(storeSrc))
      summary.addValueRead(storeSrc->stripPointerCasts());
    if (isa<GlobalValue>(storeDst))
      summary.addValueRead(storeDst->stripPointerCasts());

    auto pSet = ptrAnalysis.getPtsSet(storeDst);
    for (const auto *obj : pSet) {
      if (!isLocalStackLocation(obj, storeInst.getParent()->getParent()))
        summary.addMemoryWrite(obj);
    }
  }

  void summarizeAtomic(Instruction &inst, Value *ptr) {
    if (isa<GlobalValue>(ptr))
      summary.addValueRead(ptr->stripPointerCasts());
    for (const auto *object : ptrAnalysis.getPtsSet(ptr))
      if (!isLocalStackLocation(object, inst.getFunction())) {
        summary.addMemoryRead(object);
        summary.addMemoryWrite(object);
      }
  }
  void visitAtomicRMWInst(AtomicRMWInst &inst) {
    summarizeAtomic(inst, inst.getPointerOperand());
  }
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &inst) {
    summarizeAtomic(inst, inst.getPointerOperand());
  }

  void visitInstruction(Instruction &inst) {
    for (auto &use : inst.operands()) {
      auto *value = use.get();
      if (isa<GlobalValue>(value))
        summary.addValueRead(value->stripPointerCasts());
    }
  }
};

bool addExternalMemoryWrite(const Value *v, ModRefFunctionSummary &summary,
                            const Function *caller,
                            const SemiSparsePointerAnalysis &ptrAnalysis,
                            bool isReachableMemory) {
  auto changed = false;
  auto pSet = ptrAnalysis.getPtsSet(v);
  for (const auto *loc : pSet) {
    if (isLocalStackLocation(loc, caller))
      continue;
    if (isReachableMemory) {
      for (const auto *oLoc :
           ptrAnalysis.getMemoryManager().getReachableMemoryObjects(loc))
        changed |= summary.addMemoryWrite(oLoc);
    } else {
      changed |= summary.addMemoryWrite(loc);
    }
  }
  return changed;
}

bool updateSummaryForModEffect(const Instruction *inst,
                               ModRefFunctionSummary &summary,
                               const SemiSparsePointerAnalysis &ptrAnalysis,
                               const ModRefEffect &modEffect) {
  assert(modEffect.isModEffect());

  auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  assert(cs);
  const auto *caller = inst->getParent()->getParent();

  auto const &pos = modEffect.getPosition();
  if (pos.isReturnPosition())
    return addExternalMemoryWrite(inst, summary, caller, ptrAnalysis,
                                  modEffect.onReachableMemory());
  else {
    auto const &argPos = pos.getAsArgPosition();
    unsigned idx = argPos.getArgIndex();

    if (!argPos.isAfterArgPosition()) {
      if (idx >= cs->arg_size())
        return false;
      return addExternalMemoryWrite(cs->getArgOperand(idx)->stripPointerCasts(),
                                    summary, caller, ptrAnalysis,
                                    modEffect.onReachableMemory());
    } else {
      bool changed = false;
      for (auto i = idx, e = cs->arg_size(); i < e; ++i)
        if (cs->getArgOperand(i)->getType()->isPointerTy())
          changed |= addExternalMemoryWrite(
              cs->getArgOperand(i)->stripPointerCasts(), summary, caller,
              ptrAnalysis, modEffect.onReachableMemory());
      return changed;
    }
  }
}

bool addExternalMemoryRead(const Value *v, ModRefFunctionSummary &summary,
                           const Function *caller,
                           const SemiSparsePointerAnalysis &ptrAnalysis,
                           bool isReachableMemory) {
  bool changed = false;
  auto pSet = ptrAnalysis.getPtsSet(v);
  for (const auto *loc : pSet) {
    if (isLocalStackLocation(loc, caller))
      continue;
    if (isReachableMemory) {
      for (const auto *oLoc :
           ptrAnalysis.getMemoryManager().getReachableMemoryObjects(loc))
        changed |= summary.addMemoryRead(oLoc);
    } else {
      changed |= summary.addMemoryRead(loc);
    }
  }
  return changed;
}

bool updateSummaryForRefEffect(const Instruction *inst,
                               ModRefFunctionSummary &summary,
                               const SemiSparsePointerAnalysis &ptrAnalysis,
                               const ModRefEffect &refEffect) {
  assert(refEffect.isRefEffect());

  auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  assert(cs);
  const auto *caller = inst->getParent()->getParent();

  auto const &pos = refEffect.getPosition();
  assert(!pos.isReturnPosition() &&
         "It doesn't make any sense to ref a return position!");

  auto const &argPos = pos.getAsArgPosition();
  unsigned idx = argPos.getArgIndex();

  if (!argPos.isAfterArgPosition()) {
    if (idx >= cs->arg_size())
      return false;
    return addExternalMemoryRead(cs->getArgOperand(idx)->stripPointerCasts(),
                                 summary, caller, ptrAnalysis,
                                 refEffect.onReachableMemory());
  } else {
    bool changed = false;
    for (auto i = idx, e = cs->arg_size(); i < e; ++i) {
      if (!cs->getArgOperand(i)->getType()->isPointerTy())
        continue;
      changed |= addExternalMemoryRead(
          cs->getArgOperand(i)->stripPointerCasts(), summary, caller,
          ptrAnalysis, refEffect.onReachableMemory());
    }
    return changed;
  }
}

bool updateSummaryForExternalCall(const Instruction *inst, const Function *f,
                                  ModRefFunctionSummary &summary,
                                  const SemiSparsePointerAnalysis &ptrAnalysis,
                                  const ExternalModRefTable &modRefTable,
                                  UnmodeledPolicy policy) {
  const auto &modRefSummary = lookupModRefSummary(modRefTable, *f, policy);

  bool changed = false;
  for (auto const &effect : modRefSummary) {
    if (effect.isModEffect())
      changed |= updateSummaryForModEffect(inst, summary, ptrAnalysis, effect);
    else
      changed |= updateSummaryForRefEffect(inst, summary, ptrAnalysis, effect);
  }
  return changed;
}

void collectCallEffectsAndGraph(CallGraphMap &callGraph, const Function &f,
                                ModRefFunctionSummary &callerSummary,
                                const SemiSparsePointerAnalysis &ptrAnalysis,
                                const ExternalModRefTable &modRefTable,
                                UnmodeledPolicy policy) {
  llvm::DenseSet<const Function *> seenCallees;
  auto &callees = callGraph[&f];
  for (const auto *bb : llvm::depth_first(&f.getEntryBlock())) {
    for (auto const &inst : *bb) {
      auto *cs = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (!cs || isDebugIntrinsic(*cs))
        continue;

      auto callTgts = resolveCallTargets(ptrAnalysis, *cs, policy);
      for (const auto *callee : callTgts) {
        if (callee->isDeclaration()) {
          // External mod/ref summaries are static; apply once directly into
          // the caller instead of re-visiting them in propagation.
          updateSummaryForExternalCall(cs, callee, callerSummary, ptrAnalysis,
                                       modRefTable, policy);
        } else if (callee != &f && seenCallees.insert(callee).second) {
          callees.push_back(callee);
        }
      }
    }
  }
}

inline void orWords(uint64_t *dst, const uint64_t *src, size_t W) {
  for (size_t w = 0; w < W; ++w)
    dst[w] |= src[w];
}

inline size_t popcountWords(const uint64_t *words, size_t W) {
  size_t total = 0;
  for (size_t w = 0; w < W; ++w)
    total += static_cast<size_t>(__builtin_popcountll(words[w]));
  return total;
}

// Computes exact transitive Mod/Ref closure in bottom-up Tarjan SCC order
// using packed uint64_t bitvectors. Within any SCC, non-SCC-local memory
// objects and value reads are exact single-pass unions; only stack objects
// allocated by functions inside the SCC itself require an SCC-internal
// masked fixpoint (typically 0-1 words).
void propagateSummary(ModRefModuleSummary &moduleSummary,
                      const std::vector<const Function *> &funcs,
                      const CallGraphMap &callGraph) {
  const uint32_t numFuncs = static_cast<uint32_t>(funcs.size());
  if (numFuncs == 0)
    return;

  llvm::DenseMap<const Function *, uint32_t> funcToIdx;
  funcToIdx.reserve(numFuncs);
  for (uint32_t i = 0; i < numFuncs; ++i)
    funcToIdx.try_emplace(funcs[i], i);

  std::vector<llvm::SmallVector<uint32_t, 8>> adj(numFuncs);
  for (uint32_t u = 0; u < numFuncs; ++u) {
    auto itr = callGraph.find(funcs[u]);
    if (itr == callGraph.end())
      continue;
    auto &edges = adj[u];
    for (const auto *callee : itr->second) {
      auto cItr = funcToIdx.find(callee);
      if (cItr != funcToIdx.end() && cItr->second != u)
        edges.push_back(cItr->second);
    }
  }

  // Assign dense IDs to all MemoryObjects and GlobalValues present in local
  // summaries.
  std::vector<const MemoryObject *> idToMem;
  llvm::DenseMap<const MemoryObject *, uint32_t> memToId;
  std::vector<const Value *> idToVal;
  llvm::DenseMap<const Value *, uint32_t> valToId;

  auto getMemId = [&](const MemoryObject *obj) -> uint32_t {
    auto res = memToId.try_emplace(obj, static_cast<uint32_t>(idToMem.size()));
    if (res.second)
      idToMem.push_back(obj);
    return res.first->second;
  };
  auto getValId = [&](const Value *v) -> uint32_t {
    auto res = valToId.try_emplace(v, static_cast<uint32_t>(idToVal.size()));
    if (res.second)
      idToVal.push_back(v);
    return res.first->second;
  };

  for (uint32_t u = 0; u < numFuncs; ++u) {
    const auto &sum = moduleSummary.getSummary(funcs[u]);
    for (const auto *v : sum.value_reads())
      getValId(v);
    for (const auto *m : sum.mem_reads())
      getMemId(m);
    for (const auto *m : sum.mem_writes())
      getMemId(m);
  }

  const size_t numMems = idToMem.size();
  const size_t numVals = idToVal.size();
  if (numMems == 0 && numVals == 0)
    return;

  const size_t W_mem = (numMems + 63) / 64;
  const size_t W_val = (numVals + 63) / 64;

  // Map each function to the stack MemoryObject IDs it owns (which must be
  // filtered out when propagating summaries up into that function).
  std::vector<llvm::SmallVector<uint32_t, 4>> localStackIds(numFuncs);
  for (uint32_t mId = 0; mId < numMems; ++mId) {
    if (const Function *owner = getStackOwnerFunction(idToMem[mId])) {
      auto itr = funcToIdx.find(owner);
      if (itr != funcToIdx.end())
        localStackIds[itr->second].push_back(mId);
    }
  }

  std::vector<uint64_t> memReadBits(static_cast<size_t>(numFuncs) * W_mem, 0);
  std::vector<uint64_t> memWriteBits(static_cast<size_t>(numFuncs) * W_mem, 0);
  std::vector<uint64_t> valReadBits(static_cast<size_t>(numFuncs) * W_val, 0);

  for (uint32_t u = 0; u < numFuncs; ++u) {
    const auto &sum = moduleSummary.getSummary(funcs[u]);
    uint64_t *vDst = W_val ? &valReadBits[static_cast<size_t>(u) * W_val] : nullptr;
    uint64_t *rDst = W_mem ? &memReadBits[static_cast<size_t>(u) * W_mem] : nullptr;
    uint64_t *wDst = W_mem ? &memWriteBits[static_cast<size_t>(u) * W_mem] : nullptr;
    for (const auto *v : sum.value_reads()) {
      uint32_t id = valToId.find(v)->second;
      vDst[id >> 6] |= (1ULL << (id & 63u));
    }
    for (const auto *m : sum.mem_reads()) {
      uint32_t id = memToId.find(m)->second;
      rDst[id >> 6] |= (1ULL << (id & 63u));
    }
    for (const auto *m : sum.mem_writes()) {
      uint32_t id = memToId.find(m)->second;
      wDst[id >> 6] |= (1ULL << (id & 63u));
    }
  }

  // Iterative Tarjan's SCC over `adj` (caller -> callee) emits SCCs in exact
  // bottom-up topological order (callee SCCs before caller SCCs).
  constexpr uint32_t kUnvisited = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> dfn(numFuncs, kUnvisited);
  std::vector<uint32_t> low(numFuncs, 0);
  std::vector<uint8_t> onStack(numFuncs, 0);
  std::vector<uint32_t> tarjanStack;
  tarjanStack.reserve(numFuncs);
  std::vector<std::vector<uint32_t>> sccs;
  uint32_t nextDfn = 0;

  struct Frame {
    uint32_t u;
    uint32_t edgeIdx;
  };
  std::vector<Frame> callStack;
  callStack.reserve(numFuncs);

  for (uint32_t root = 0; root < numFuncs; ++root) {
    if (dfn[root] != kUnvisited)
      continue;
    dfn[root] = low[root] = nextDfn++;
    tarjanStack.push_back(root);
    onStack[root] = 1;
    callStack.push_back({root, 0});

    while (!callStack.empty()) {
      auto &fr = callStack.back();
      const uint32_t u = fr.u;
      if (fr.edgeIdx < adj[u].size()) {
        const uint32_t v = adj[u][fr.edgeIdx++];
        if (dfn[v] == kUnvisited) {
          dfn[v] = low[v] = nextDfn++;
          tarjanStack.push_back(v);
          onStack[v] = 1;
          callStack.push_back({v, 0});
        } else if (onStack[v]) {
          low[u] = std::min(low[u], dfn[v]);
        }
      } else {
        if (low[u] == dfn[u]) {
          std::vector<uint32_t> scc;
          while (true) {
            uint32_t w = tarjanStack.back();
            tarjanStack.pop_back();
            onStack[w] = 0;
            scc.push_back(w);
            if (w == u)
              break;
          }
          sccs.push_back(std::move(scc));
        }
        callStack.pop_back();
        if (!callStack.empty()) {
          uint32_t parent = callStack.back().u;
          low[parent] = std::min(low[parent], low[u]);
        }
      }
    }
  }

  auto clearLocalBits = [&](uint32_t u) {
    if (!W_mem || localStackIds[u].empty())
      return;
    uint64_t *rDst = &memReadBits[static_cast<size_t>(u) * W_mem];
    uint64_t *wDst = &memWriteBits[static_cast<size_t>(u) * W_mem];
    for (uint32_t id : localStackIds[u]) {
      const uint64_t mask = ~(1ULL << (id & 63u));
      rDst[id >> 6] &= mask;
      wDst[id >> 6] &= mask;
    }
  };

  std::vector<uint32_t> sccStamp(numFuncs, 0);
  std::vector<uint32_t> sccLocalPos(numFuncs, 0);
  uint32_t curStamp = 0;
  std::vector<uint64_t> unionVal(W_val);
  std::vector<uint64_t> unionR(W_mem);
  std::vector<uint64_t> unionW(W_mem);

  for (const auto &scc : sccs) {
    if (scc.size() == 1) {
      const uint32_t u = scc[0];
      uint64_t *vDst = W_val ? &valReadBits[static_cast<size_t>(u) * W_val] : nullptr;
      uint64_t *rDst = W_mem ? &memReadBits[static_cast<size_t>(u) * W_mem] : nullptr;
      uint64_t *wDst = W_mem ? &memWriteBits[static_cast<size_t>(u) * W_mem] : nullptr;
      for (uint32_t v : adj[u]) {
        if (W_val)
          orWords(vDst, &valReadBits[static_cast<size_t>(v) * W_val], W_val);
        if (W_mem) {
          orWords(rDst, &memReadBits[static_cast<size_t>(v) * W_mem], W_mem);
          orWords(wDst, &memWriteBits[static_cast<size_t>(v) * W_mem], W_mem);
        }
      }
      clearLocalBits(u);
      continue;
    }

    // Multi-function SCC.
    ++curStamp;
    const uint32_t k = static_cast<uint32_t>(scc.size());
    for (uint32_t i = 0; i < k; ++i) {
      sccStamp[scc[i]] = curStamp;
      sccLocalPos[scc[i]] = i;
    }

    // 1. Merge already-finalized external callees into each SCC member.
    for (uint32_t i = 0; i < k; ++i) {
      const uint32_t u = scc[i];
      uint64_t *vDst = W_val ? &valReadBits[static_cast<size_t>(u) * W_val] : nullptr;
      uint64_t *rDst = W_mem ? &memReadBits[static_cast<size_t>(u) * W_mem] : nullptr;
      uint64_t *wDst = W_mem ? &memWriteBits[static_cast<size_t>(u) * W_mem] : nullptr;
      for (uint32_t v : adj[u]) {
        if (sccStamp[v] == curStamp)
          continue;
        if (W_val)
          orWords(vDst, &valReadBits[static_cast<size_t>(v) * W_val], W_val);
        if (W_mem) {
          orWords(rDst, &memReadBits[static_cast<size_t>(v) * W_mem], W_mem);
          orWords(wDst, &memWriteBits[static_cast<size_t>(v) * W_mem], W_mem);
        }
      }
      clearLocalBits(u);
    }

    // 2. Collect stack objects owned by members of this SCC.
    std::vector<uint32_t> sccStackMems;
    for (uint32_t u : scc)
      sccStackMems.insert(sccStackMems.end(), localStackIds[u].begin(),
                          localStackIds[u].end());

    // 3. Save per-member SCC-local stack bits before overwriting with SCC union.
    const size_t numSub = sccStackMems.size();
    const size_t W_sub = (numSub + 63) / 64;
    std::vector<uint64_t> subR, subW;
    if (W_sub) {
      subR.assign(static_cast<size_t>(k) * W_sub, 0ULL);
      subW.assign(static_cast<size_t>(k) * W_sub, 0ULL);
      for (uint32_t i = 0; i < k; ++i) {
        const uint32_t u = scc[i];
        const uint64_t *rSrc = &memReadBits[static_cast<size_t>(u) * W_mem];
        const uint64_t *wSrc = &memWriteBits[static_cast<size_t>(u) * W_mem];
        uint64_t *sr = &subR[static_cast<size_t>(i) * W_sub];
        uint64_t *sw = &subW[static_cast<size_t>(i) * W_sub];
        for (size_t b = 0; b < numSub; ++b) {
          const uint32_t mId = sccStackMems[b];
          if (rSrc[mId >> 6] & (1ULL << (mId & 63u)))
            sr[b >> 6] |= (1ULL << (b & 63u));
          if (wSrc[mId >> 6] & (1ULL << (mId & 63u)))
            sw[b >> 6] |= (1ULL << (b & 63u));
        }
      }
    }

    // 4. Single-pass union across the SCC for all values and non-SCC-local mems.
    if (W_val) {
      std::fill(unionVal.begin(), unionVal.end(), 0ULL);
      for (uint32_t u : scc)
        orWords(unionVal.data(), &valReadBits[static_cast<size_t>(u) * W_val],
                W_val);
      for (uint32_t u : scc)
        std::copy(unionVal.begin(), unionVal.end(),
                  &valReadBits[static_cast<size_t>(u) * W_val]);
    }
    if (W_mem) {
      std::fill(unionR.begin(), unionR.end(), 0ULL);
      std::fill(unionW.begin(), unionW.end(), 0ULL);
      for (uint32_t u : scc) {
        orWords(unionR.data(), &memReadBits[static_cast<size_t>(u) * W_mem],
                W_mem);
        orWords(unionW.data(), &memWriteBits[static_cast<size_t>(u) * W_mem],
                W_mem);
      }
      for (uint32_t mId : sccStackMems) {
        const uint64_t mask = ~(1ULL << (mId & 63u));
        unionR[mId >> 6] &= mask;
        unionW[mId >> 6] &= mask;
      }
      for (uint32_t u : scc) {
        std::copy(unionR.begin(), unionR.end(),
                  &memReadBits[static_cast<size_t>(u) * W_mem]);
        std::copy(unionW.begin(), unionW.end(),
                  &memWriteBits[static_cast<size_t>(u) * W_mem]);
      }
    }

    // 5. Tiny masked fixpoint strictly for stack objects owned inside this SCC.
    if (W_sub) {
      std::vector<llvm::SmallVector<uint32_t, 4>> revInSCC(k);
      for (uint32_t i = 0; i < k; ++i) {
        for (uint32_t v : adj[scc[i]]) {
          if (sccStamp[v] == curStamp)
            revInSCC[sccLocalPos[v]].push_back(i);
        }
      }
      std::vector<uint64_t> allowMask(static_cast<size_t>(k) * W_sub, ~0ULL);
      size_t bitOffset = 0;
      for (uint32_t i = 0; i < k; ++i) {
        const size_t cnt = localStackIds[scc[i]].size();
        uint64_t *am = &allowMask[static_cast<size_t>(i) * W_sub];
        for (size_t j = 0; j < cnt; ++j) {
          const size_t b = bitOffset + j;
          am[b >> 6] &= ~(1ULL << (b & 63u));
        }
        bitOffset += cnt;
      }

      std::vector<uint32_t> q;
      std::vector<uint8_t> inQ(k, 0);
      q.reserve(k);
      for (uint32_t i = 0; i < k; ++i) {
        bool nonZero = false;
        const uint64_t *sr = &subR[static_cast<size_t>(i) * W_sub];
        const uint64_t *sw = &subW[static_cast<size_t>(i) * W_sub];
        for (size_t w = 0; w < W_sub; ++w)
          if (sr[w] | sw[w]) {
            nonZero = true;
            break;
          }
        if (nonZero) {
          q.push_back(i);
          inQ[i] = 1;
        }
      }
      for (size_t head = 0; head < q.size(); ++head) {
        const uint32_t calleeIdx = q[head];
        inQ[calleeIdx] = 0;
        const uint64_t *sr = &subR[static_cast<size_t>(calleeIdx) * W_sub];
        const uint64_t *sw = &subW[static_cast<size_t>(calleeIdx) * W_sub];
        for (uint32_t callerIdx : revInSCC[calleeIdx]) {
          uint64_t *dr = &subR[static_cast<size_t>(callerIdx) * W_sub];
          uint64_t *dw = &subW[static_cast<size_t>(callerIdx) * W_sub];
          const uint64_t *am = &allowMask[static_cast<size_t>(callerIdx) * W_sub];
          uint64_t diff = 0ULL;
          for (size_t w = 0; w < W_sub; ++w) {
            const uint64_t nr = dr[w] | (sr[w] & am[w]);
            const uint64_t nw = dw[w] | (sw[w] & am[w]);
            diff |= (nr ^ dr[w]) | (nw ^ dw[w]);
            dr[w] = nr;
            dw[w] = nw;
          }
          if (diff && !inQ[callerIdx]) {
            inQ[callerIdx] = 1;
            q.push_back(callerIdx);
          }
        }
      }

      // Scatter final SCC-local stack bits back into memReadBits/memWriteBits.
      for (uint32_t i = 0; i < k; ++i) {
        const uint32_t u = scc[i];
        uint64_t *rDst = &memReadBits[static_cast<size_t>(u) * W_mem];
        uint64_t *wDst = &memWriteBits[static_cast<size_t>(u) * W_mem];
        const uint64_t *sr = &subR[static_cast<size_t>(i) * W_sub];
        const uint64_t *sw = &subW[static_cast<size_t>(i) * W_sub];
        for (size_t w = 0; w < W_sub; ++w) {
          uint64_t rb = sr[w];
          while (rb) {
            unsigned tz = static_cast<unsigned>(__builtin_ctzll(rb));
            uint32_t mId = sccStackMems[(w << 6) + tz];
            rDst[mId >> 6] |= (1ULL << (mId & 63u));
            rb &= (rb - 1);
          }
          uint64_t wb = sw[w];
          while (wb) {
            unsigned tz = static_cast<unsigned>(__builtin_ctzll(wb));
            uint32_t mId = sccStackMems[(w << 6) + tz];
            wDst[mId >> 6] |= (1ULL << (mId & 63u));
            wb &= (wb - 1);
          }
        }
      }
    }
  }

  // Materialize final ModRefFunctionSummary sets with exact pre-reservation.
  for (uint32_t u = 0; u < numFuncs; ++u) {
    auto &sum = moduleSummary.getSummary(funcs[u]);
    sum.clear();
    if (W_val) {
      const uint64_t *vSrc = &valReadBits[static_cast<size_t>(u) * W_val];
      const size_t cnt = popcountWords(vSrc, W_val);
      if (cnt) {
        sum.reserveValueReads(cnt);
        for (size_t w = 0; w < W_val; ++w) {
          uint64_t bits = vSrc[w];
          while (bits) {
            unsigned tz = static_cast<unsigned>(__builtin_ctzll(bits));
            sum.addValueRead(idToVal[(w << 6) + tz]);
            bits &= (bits - 1);
          }
        }
      }
    }
    if (W_mem) {
      const uint64_t *rSrc = &memReadBits[static_cast<size_t>(u) * W_mem];
      const size_t rCnt = popcountWords(rSrc, W_mem);
      if (rCnt) {
        sum.reserveMemoryReads(rCnt);
        for (size_t w = 0; w < W_mem; ++w) {
          uint64_t bits = rSrc[w];
          while (bits) {
            unsigned tz = static_cast<unsigned>(__builtin_ctzll(bits));
            sum.addMemoryRead(idToMem[(w << 6) + tz]);
            bits &= (bits - 1);
          }
        }
      }
      const uint64_t *wSrc = &memWriteBits[static_cast<size_t>(u) * W_mem];
      const size_t wCnt = popcountWords(wSrc, W_mem);
      if (wCnt) {
        sum.reserveMemoryWrites(wCnt);
        for (size_t w = 0; w < W_mem; ++w) {
          uint64_t bits = wSrc[w];
          while (bits) {
            unsigned tz = static_cast<unsigned>(__builtin_ctzll(bits));
            sum.addMemoryWrite(idToMem[(w << 6) + tz]);
            bits &= (bits - 1);
          }
        }
      }
    }
  }
}

} // end of anonymous namespace

void ModRefModuleAnalysis::collectProcedureSummary(
    const Function &f, ModRefFunctionSummary &summary) {
  SummaryInstVisitor summaryVisitor(ptrAnalysis, summary);
  for (const auto *bb : llvm::depth_first(&f.getEntryBlock()))
    summaryVisitor.visit(*const_cast<BasicBlock *>(bb));
}

ModRefModuleSummary ModRefModuleAnalysis::runOnModule(const Module &module) {
  auto reachable = reachableFunctions(module, ptrAnalysis, unmodeledPolicy);
  return runOnModule(module, reachable);
}

ModRefModuleSummary ModRefModuleAnalysis::runOnModule(
    const Module &module,
    const std::unordered_set<const Function *> &reachable) {
  (void)module;
  ModRefModuleSummary moduleSummary;
  CallGraphMap callGraph;
  std::vector<const Function *> funcs;
  funcs.reserve(reachable.size());

  for (const auto *function : reachable) {
    funcs.push_back(function);
    const auto &f = *function;
    auto &summary = moduleSummary.getSummary(&f);
    collectProcedureSummary(f, summary);
    collectCallEffectsAndGraph(callGraph, f, summary, ptrAnalysis, modRefTable,
                               unmodeledPolicy);
  }

  propagateSummary(moduleSummary, funcs, callGraph);

  return moduleSummary;
}

} // namespace tpa::taint