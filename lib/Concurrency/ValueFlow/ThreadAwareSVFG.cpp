#include "Concurrency/ValueFlow/ThreadAwareSVFG.h"

#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/Thread/ThreadCreationTree.h"
#include "Concurrency/ValueFlow/LockSpanReasoner.h"

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

namespace lotus::analysis {

namespace {

/// Edges of the underlying, thread-agnostic value-flow graph. Interference
/// edges (this overlay's own product) are excluded so every span query is
/// answered against the base graph only.
bool isBaseValueFlowEdge(const SVFGEdge *edge) {
  if (!edge)
    return false;
  const SVFGEdgeK kind = edge->getEdgeKind();
  return isIndirectVFGEdge(kind) && !isThreadMHPVFGEdge(kind);
}

} // namespace

ThreadAwareSVFGBuilder::ThreadAwareSVFGBuilder(
    SVFG &graph, const mhp::IMHPAnalysis &mhp,
    const mhp::LockSetAnalysis *locks, const FilteredSVFGView *scope,
    const ThreadCreationTree *threadTree)
    : ThreadAwareSVFGBuilder(graph, mhp, locks, scope, threadTree, Config{}) {}

ThreadAwareSVFGBuilder::ThreadAwareSVFGBuilder(
    SVFG &graph, const mhp::IMHPAnalysis &mhp,
    const mhp::LockSetAnalysis *locks, const FilteredSVFGView *scope,
    const ThreadCreationTree *threadTree, Config config)
    : graph_(&graph), mhp_(&mhp), locks_(locks), scope_(scope),
      threadTree_(threadTree), config_(config) {
  assert((!scope_ || &scope_->source() == graph_) &&
         "filtered SVFG scope must view the overlay's base graph");
  if (locks_)
    spans_ = std::make_unique<LockSpanReasoner>(*locks_);
}

ThreadAwareSVFGBuilder::~ThreadAwareSVFGBuilder() { clear(); }

bool ThreadAwareSVFGBuilder::inScope(const SVFGNode *node) const {
  return node && (!scope_ || scope_->contains(node));
}

SVFGNodeBS ThreadAwareSVFGBuilder::intersectTargets(const SVFGNode &lhs,
                                                    const SVFGNode &rhs) const {
  const SVFGNodeBS *lhsPts = lhs.getPointsTo();
  const SVFGNodeBS *rhsPts = rhs.getPointsTo();
  if (!lhsPts || !rhsPts || lhsPts->empty() || rhsPts->empty())
    return {};

  const bool lhsUnknown =
      std::any_of(lhsPts->begin(), lhsPts->end(),
                  [&](uint32_t id) { return graph_->isUnknownObject(id); });
  const bool rhsUnknown =
      std::any_of(rhsPts->begin(), rhsPts->end(),
                  [&](uint32_t id) { return graph_->isUnknownObject(id); });
  if (lhsUnknown || rhsUnknown) {
    SVFGNodeBS guard;
    for (uint32_t id : *lhsPts)
      if (graph_->isUnknownObject(id))
        guard.insert(id);
    for (uint32_t id : *rhsPts)
      if (graph_->isUnknownObject(id))
        guard.insert(id);
    return guard;
  }

  const SVFGNodeBS *small = lhsPts;
  const SVFGNodeBS *large = rhsPts;
  if (large->size() < small->size())
    std::swap(small, large);

  SVFGNodeBS result;
  for (uint32_t id : *small)
    if (large->count(id) != 0)
      result.insert(id);
  return result;
}

bool ThreadAwareSVFGBuilder::addInterferenceEdge(SVFGNode &source,
                                                 SVFGNode &destination,
                                                 const SVFGNodeBS &guard) {
  for (SVFGEdge *edge : source.getOutEdges()) {
    if (edge && edge->getDstNode() == &destination &&
        edge->getEdgeKind() == SVFGEdgeK::ThreadMHPIndirectVF) {
      edge->addPointsTo(guard);
      return false;
    }
  }

  SVFGEdge *edge = graph_->addEdge(
      &source, &destination, SVFGEdgeK::ThreadMHPIndirectVF, nullptr, guard);
  if (!edge)
    return false;
  overlayEdges_.insert(edge);
  ++stats_.edgesAdded;
  return true;
}

bool ThreadAwareSVFGBuilder::addOwnedEdge(SVFGNode &source,
                                          SVFGNode &destination, SVFGEdgeK kind,
                                          const CallBase *callSite,
                                          const SVFGNodeBS &guard) {
  for (SVFGEdge *edge : source.getOutEdges())
    if (edge && edge->getDstNode() == &destination &&
        edge->getEdgeKind() == kind && edge->getCallSite() == callSite) {
      edge->addPointsTo(guard);
      return false;
    }
  SVFGEdge *edge =
      graph_->addEdge(&source, &destination, kind, callSite, guard);
  if (!edge)
    return false;
  overlayEdges_.insert(edge);
  return true;
}

void ThreadAwareSVFGBuilder::connectForkFlow() {
  if (!threadTree_)
    return;
  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();

  auto isStrongUpdateStoreForObject = [&](const StoreSVFGNode *store,
                                          uint32_t obj) -> bool {
    if (!store || !inScope(store))
      return false;
    const SVFGNodeBS *pts = store->getPointsTo();
    if (!pts || pts->size() != 1 || *pts->begin() != obj)
      return false;
    const auto *si = dyn_cast_or_null<StoreInst>(store->getInstruction());
    if (!si || !si->getValueOperand()->getType()->isPointerTy())
      return false;
    const SVFG::ObjectInfo *info = graph_->getObjectInfo(obj);
    if (!info || !info->isSingleton || info->isUnknown || info->isHeap ||
        info->isArray || info->isFieldInsensitive)
      return false;
    if (info->baseObjId != 0 &&
        graph_->isFieldInsensitiveObject(info->baseObjId))
      return false;
    return true;
  };

  struct FunctionDefIndex {
    std::unordered_map<const Instruction *, std::vector<SVFGNode *>> defAtInst;
    std::unordered_map<uint32_t, DenseSet<const Instruction *>> killersByObj;
    std::vector<SVFGNode *> formalIns;
  };
  std::unordered_map<const Function *, FunctionDefIndex> fnIndexCache;

  auto getFnIndex = [&](const Function *F) -> const FunctionDefIndex & {
    auto it = fnIndexCache.find(F);
    if (it != fnIndexCache.end())
      return it->second;
    FunctionDefIndex idx;
    for (SVFGNode *formalIn : graph_->getFormalIns(F)) {
      if (formalIn && inScope(formalIn))
        idx.formalIns.push_back(formalIn);
    }
    for (auto &entry : *graph_) {
      SVFGNode *node = entry.second;
      if (!node || !inScope(node))
        continue;
      if (auto *store = dyn_cast<StoreSVFGNode>(node)) {
        const Instruction *inst = store->getInstruction();
        if (!inst || inst->getFunction() != F)
          continue;
        idx.defAtInst[inst].push_back(store);
        const SVFGNodeBS *pts = store->getPointsTo();
        if (pts && pts->size() == 1) {
          uint32_t obj = *pts->begin();
          if (isStrongUpdateStoreForObject(store, obj))
            idx.killersByObj[obj].insert(inst);
        }
      } else if (auto *aout = dyn_cast<ActualOutSVFGNode>(node)) {
        const Instruction *cs = aout->getCallSite();
        if (!cs || cs->getFunction() != F)
          continue;
        idx.defAtInst[cs].push_back(aout);
      }
    }
    return fnIndexCache.emplace(F, std::move(idx)).first->second;
  };

  auto collectReachingDefs = [&](const CallBase *fork,
                                 const FunctionDefIndex &fnIdx,
                                 const DenseSet<const Instruction *> *killers,
                                 std::vector<SVFGNode *> &outDefs) {
    outDefs.clear();
    const Function *F = fork->getFunction();
    const BasicBlock *forkBB = fork->getParent();
    if (!F || !forkBB)
      return;

    std::unordered_set<SVFGNode *> seenDefs;
    auto recordDefsAt = [&](const Instruction *inst) {
      if (inst == fork)
        return;
      auto it = fnIdx.defAtInst.find(inst);
      if (it == fnIdx.defAtInst.end())
        return;
      for (SVFGNode *def : it->second) {
        if (seenDefs.insert(def).second)
          outDefs.push_back(def);
      }
    };
    auto recordFormalIns = [&]() {
      for (SVFGNode *fi : fnIdx.formalIns) {
        if (seenDefs.insert(fi).second)
          outDefs.push_back(fi);
      }
    };

    // Walk backward in forkBB strictly before fork.
    bool killedBeforeFork = false;
    for (auto it = std::next(fork->getReverseIterator()), end = forkBB->rend();
         it != end; ++it) {
      const Instruction *inst = &*it;
      recordDefsAt(inst);
      if (killers && killers->count(inst)) {
        killedBeforeFork = true;
        break;
      }
    }
    if (killedBeforeFork)
      return;

    if (forkBB == &F->getEntryBlock())
      recordFormalIns();

    SmallVector<const BasicBlock *, 16> worklist;
    DenseSet<const BasicBlock *> visited;
    for (const BasicBlock *pred : predecessors(forkBB)) {
      if (visited.insert(pred).second)
        worklist.push_back(pred);
    }

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.pop_back_val();
      bool killed = false;
      for (auto it = bb->rbegin(), end = bb->rend(); it != end; ++it) {
        const Instruction *inst = &*it;
        if (bb == forkBB && inst == fork) {
          killed = true;
          break;
        }
        recordDefsAt(inst);
        if (killers && killers->count(inst)) {
          killed = true;
          break;
        }
      }
      if (killed)
        continue;
      if (bb == &F->getEntryBlock())
        recordFormalIns();
      for (const BasicBlock *pred : predecessors(bb)) {
        if (visited.insert(pred).second)
          worklist.push_back(pred);
      }
    }
  };

  for (const ThreadCreationTree::ForkRelation &relation :
       threadTree_->forkRelations()) {
    const CallBase *fork = relation.site;
    const Function *target = relation.target;
    if (!fork || !target || !fork->getFunction())
      continue;

    const Value *payload = threadAPI->getActualParmAtForkSite(fork);
    SVFGNode *actual = payload ? graph_->getValueNode(payload) : nullptr;
    if (actual && inScope(actual)) {
      for (SVFGNode *formal : graph_->getFormalParms(target)) {
        auto *parameter = dyn_cast<FormalParmSVFGNode>(formal);
        if (!parameter || parameter->getParamIndex() != 0 || !inScope(formal))
          continue;
        if (addOwnedEdge(*actual, *formal, SVFGEdgeK::CallDir, fork))
          ++stats_.forkParameterEdges;
      }
    }

    const FunctionDefIndex &fnIdx = getFnIndex(fork->getFunction());
    std::vector<SVFGNode *> unkilledDefs;
    bool computedUnkilledDefs = false;
    std::unordered_map<uint32_t, std::vector<SVFGNode *>> killedDefsByObj;

    for (SVFGNode *formal : graph_->getFormalIns(target)) {
      if (!formal || !inScope(formal))
        continue;
      const SVFGNodeBS *formalPointsTo = formal->getPointsTo();
      if (!formalPointsTo || formalPointsTo->empty())
        continue;

      // ActualIn nodes at fork already reflect MSSA versions at the call site.
      for (SVFGNode *actualIn : graph_->getActualIns(fork)) {
        if (!actualIn || !inScope(actualIn))
          continue;
        SVFGNodeBS guard = intersectTargets(*actualIn, *formal);
        if (!guard.empty() &&
            addOwnedEdge(*actualIn, *formal, SVFGEdgeK::CallAIn, fork, guard))
          ++stats_.forkMemoryEdges;
      }

      // Partition formal's objects into those with strong-update killers in the
      // forking function vs. those without.
      SVFGNodeBS unkilledObjs;
      std::vector<uint32_t> killedObjs;
      for (uint32_t obj : *formalPointsTo) {
        if (fnIdx.killersByObj.count(obj))
          killedObjs.push_back(obj);
        else
          unkilledObjs.insert(obj);
      }

      std::unordered_map<SVFGNode *, SVFGNodeBS> sourceGuards;

      if (!unkilledObjs.empty()) {
        if (!computedUnkilledDefs) {
          collectReachingDefs(fork, fnIdx, nullptr, unkilledDefs);
          computedUnkilledDefs = true;
        }
        for (SVFGNode *source : unkilledDefs) {
          SVFGNodeBS guard = intersectTargets(*source, *formal);
          if (guard.empty())
            continue;
          SVFGNodeBS filtered;
          for (uint32_t obj : guard) {
            if (unkilledObjs.count(obj) || graph_->isUnknownObject(obj))
              filtered.insert(obj);
          }
          if (!filtered.empty()) {
            SVFGNodeBS &acc = sourceGuards[source];
            acc.insert(filtered.begin(), filtered.end());
          }
        }
      }

      for (uint32_t obj : killedObjs) {
        auto it = killedDefsByObj.find(obj);
        if (it == killedDefsByObj.end()) {
          std::vector<SVFGNode *> defs;
          const auto &killers = fnIdx.killersByObj.at(obj);
          collectReachingDefs(fork, fnIdx, &killers, defs);
          it = killedDefsByObj.emplace(obj, std::move(defs)).first;
        }
        for (SVFGNode *source : it->second) {
          const SVFGNodeBS *srcPts = source->getPointsTo();
          if (!srcPts || srcPts->empty())
            continue;
          bool matches = srcPts->count(obj) != 0;
          if (!matches) {
            for (uint32_t sObj : *srcPts) {
              if (graph_->isUnknownObject(sObj)) {
                matches = true;
                break;
              }
            }
          }
          if (matches)
            sourceGuards[source].insert(obj);
        }
      }

      for (const auto &entry : sourceGuards) {
        if (entry.second.empty())
          continue;
        if (addOwnedEdge(*entry.first, *formal, SVFGEdgeK::CallAIn, fork,
                         entry.second))
          ++stats_.forkMemoryEdges;
      }
    }
  }
}

void ThreadAwareSVFGBuilder::connectJoinFlow() {
  if (!threadTree_ || !graph_->getICFG())
    return;

  auto isStrongUpdateStoreForObject = [&](const StoreSVFGNode *store,
                                          uint32_t obj) -> bool {
    if (!store || !inScope(store))
      return false;
    const SVFGNodeBS *pts = store->getPointsTo();
    if (!pts || pts->size() != 1 || *pts->begin() != obj)
      return false;
    const auto *si = dyn_cast_or_null<StoreInst>(store->getInstruction());
    if (!si || !si->getValueOperand()->getType()->isPointerTy())
      return false;
    const SVFG::ObjectInfo *info = graph_->getObjectInfo(obj);
    if (!info || !info->isSingleton || info->isUnknown || info->isHeap ||
        info->isArray || info->isFieldInsensitive)
      return false;
    if (info->baseObjId != 0 &&
        graph_->isFieldInsensitiveObject(info->baseObjId))
      return false;
    return true;
  };

  struct FunctionUseIndex {
    std::unordered_map<const Instruction *, std::vector<SVFGNode *>> useAtInst;
    std::unordered_map<uint32_t, DenseSet<const Instruction *>> killersByObj;
  };
  std::unordered_map<const Function *, FunctionUseIndex> fnUseCache;

  auto getFnUseIndex = [&](const Function *F) -> const FunctionUseIndex & {
    auto it = fnUseCache.find(F);
    if (it != fnUseCache.end())
      return it->second;
    FunctionUseIndex idx;
    for (auto &entry : *graph_) {
      SVFGNode *node = entry.second;
      if (!node || !inScope(node))
        continue;
      if (isa<LoadSVFGNode, StoreSVFGNode>(node)) {
        const Instruction *inst = node->getInstruction();
        if (!inst || inst->getFunction() != F)
          continue;
        idx.useAtInst[inst].push_back(node);
        if (auto *store = dyn_cast<StoreSVFGNode>(node)) {
          const SVFGNodeBS *pts = store->getPointsTo();
          if (pts && pts->size() == 1) {
            uint32_t obj = *pts->begin();
            if (isStrongUpdateStoreForObject(store, obj))
              idx.killersByObj[obj].insert(inst);
          }
        }
      } else if (auto *ain = dyn_cast<ActualInSVFGNode>(node)) {
        const Instruction *cs = ain->getCallSite();
        if (!cs || cs->getFunction() != F)
          continue;
        idx.useAtInst[cs].push_back(ain);
      }
    }
    return fnUseCache.emplace(F, std::move(idx)).first->second;
  };

  auto collectReachableUses = [&](const CallBase *join,
                                  const FunctionUseIndex &fnIdx,
                                  const DenseSet<const Instruction *> *killers,
                                  std::vector<SVFGNode *> &outUses) {
    outUses.clear();
    const BasicBlock *joinBB = join->getParent();
    if (!joinBB)
      return;

    std::unordered_set<SVFGNode *> seenUses;
    auto recordUsesAt = [&](const Instruction *inst) {
      if (inst == join)
        return;
      auto it = fnIdx.useAtInst.find(inst);
      if (it == fnIdx.useAtInst.end())
        return;
      for (SVFGNode *use : it->second) {
        if (seenUses.insert(use).second)
          outUses.push_back(use);
      }
    };

    bool killedInJoinBB = false;
    for (auto it = std::next(join->getIterator()), end = joinBB->end();
         it != end; ++it) {
      const Instruction *inst = &*it;
      recordUsesAt(inst);
      if (killers && killers->count(inst)) {
        killedInJoinBB = true;
        break;
      }
    }
    if (killedInJoinBB)
      return;

    SmallVector<const BasicBlock *, 16> worklist;
    DenseSet<const BasicBlock *> visited;
    for (const BasicBlock *succ : successors(joinBB)) {
      if (visited.insert(succ).second)
        worklist.push_back(succ);
    }

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.pop_back_val();
      bool killed = false;
      for (const Instruction &instRef : *bb) {
        const Instruction *inst = &instRef;
        if (bb == joinBB && inst == join) {
          killed = true;
          break;
        }
        recordUsesAt(inst);
        if (killers && killers->count(inst)) {
          killed = true;
          break;
        }
      }
      if (killed)
        continue;
      for (const BasicBlock *succ : successors(bb)) {
        if (visited.insert(succ).second)
          worklist.push_back(succ);
      }
    }
  };

  for (const CallBase *join : threadTree_->threadCallGraph().joinSites()) {
    std::vector<const Function *> joined = threadTree_->joinedFunctions(join);
    if (joined.empty() || !join || !join->getFunction())
      continue;
    ICFGNode *joinNode =
        const_cast<ICFG *>(graph_->getICFG())->getRetICFGNode(join);
    const FunctionUseIndex &fnIdx = getFnUseIndex(join->getFunction());
    std::vector<SVFGNode *> unkilledUses;
    bool computedUnkilledUses = false;
    std::unordered_map<uint32_t, std::vector<SVFGNode *>> killedUsesByObj;

    for (const Function *target : joined) {
      for (SVFGNode *formal : graph_->getFormalOuts(target)) {
        auto *formalOut = dyn_cast<FormalOutSVFGNode>(formal);
        if (!formalOut || !inScope(formalOut))
          continue;
        ActualOutSVFGNode *actualOut = nullptr;
        for (SVFGNode *existing : graph_->getActualOuts(join)) {
          auto *candidate = dyn_cast<ActualOutSVFGNode>(existing);
          if (candidate && candidate->getMemReg() == formalOut->getMemReg()) {
            actualOut = candidate;
            break;
          }
        }
        if (!actualOut) {
          actualOut = new ActualOutSVFGNode(
              graph_->getNextNodeId(), joinNode, join, formalOut->getMemReg(),
              formalOut->getDefSVFVars(), formalOut->getSSAVersion());
          graph_->addNode(actualOut);
          graph_->addActualOut(join, actualOut);
          ++stats_.joinMemoryNodes;
        }
        if (addOwnedEdge(*formalOut, *actualOut, SVFGEdgeK::RetAOut, join,
                         formalOut->getDefSVFVars()))
          ++stats_.joinMemoryEdges;

        const SVFGNodeBS *aoutPts = actualOut->getPointsTo();
        if (!aoutPts || aoutPts->empty())
          continue;

        SVFGNodeBS unkilledObjs;
        std::vector<uint32_t> killedObjs;
        for (uint32_t obj : *aoutPts) {
          if (fnIdx.killersByObj.count(obj))
            killedObjs.push_back(obj);
          else
            unkilledObjs.insert(obj);
        }

        std::unordered_map<SVFGNode *, SVFGNodeBS> destGuards;

        if (!unkilledObjs.empty()) {
          if (!computedUnkilledUses) {
            collectReachableUses(join, fnIdx, nullptr, unkilledUses);
            computedUnkilledUses = true;
          }
          for (SVFGNode *destination : unkilledUses) {
            if (!destination || destination == actualOut)
              continue;
            SVFGNodeBS guard = intersectTargets(*actualOut, *destination);
            if (guard.empty())
              continue;
            SVFGNodeBS filtered;
            for (uint32_t obj : guard) {
              if (unkilledObjs.count(obj) || graph_->isUnknownObject(obj))
                filtered.insert(obj);
            }
            if (!filtered.empty()) {
              SVFGNodeBS &acc = destGuards[destination];
              acc.insert(filtered.begin(), filtered.end());
            }
          }
        }

        for (uint32_t obj : killedObjs) {
          auto it = killedUsesByObj.find(obj);
          if (it == killedUsesByObj.end()) {
            std::vector<SVFGNode *> uses;
            const auto &killers = fnIdx.killersByObj.at(obj);
            collectReachableUses(join, fnIdx, &killers, uses);
            it = killedUsesByObj.emplace(obj, std::move(uses)).first;
          }
          for (SVFGNode *destination : it->second) {
            if (!destination || destination == actualOut)
              continue;
            const SVFGNodeBS *dstPts = destination->getPointsTo();
            if (!dstPts || dstPts->empty())
              continue;
            bool matches = dstPts->count(obj) != 0;
            if (!matches) {
              for (uint32_t dObj : *dstPts) {
                if (graph_->isUnknownObject(dObj)) {
                  matches = true;
                  break;
                }
              }
            }
            if (matches)
              destGuards[destination].insert(obj);
          }
        }

        for (const auto &entry : destGuards) {
          if (entry.second.empty())
            continue;
          if (addOwnedEdge(*actualOut, *entry.first, SVFGEdgeK::IntraIndirect,
                           nullptr, entry.second))
            ++stats_.joinMemoryEdges;
        }
      }
    }
  }
}

void ThreadAwareSVFGBuilder::connectForkJoinMemoryFlow() {
  connectJoinFlow();
  connectForkFlow();
}

const std::vector<StoreSVFGNode *> &
ThreadAwareSVFGBuilder::baseSuccessorStores(SVFGNode &node) {
  auto cached = baseSuccessorStores_.find(&node);
  if (cached != baseSuccessorStores_.end())
    return cached->second;

  std::vector<StoreSVFGNode *> stores;
  std::unordered_set<SVFGNode *> visited{&node};
  std::vector<SVFGNode *> worklist;
  auto push = [&](const SVFGEdge *edge) {
    SVFGNode *next = edge ? edge->getDstNode() : nullptr;
    if (next && !visited.count(next) && inScope(next)) {
      visited.insert(next);
      worklist.push_back(next);
    }
  };
  for (SVFGEdge *edge : node.getOutEdges())
    if (isBaseValueFlowEdge(edge))
      push(edge);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.back();
    worklist.pop_back();
    if (auto *store = dyn_cast<StoreSVFGNode>(current)) {
      stores.push_back(store);
      // Statement nodes terminate the walk: the value-flow chain stops where
      // the memory version becomes a plain SSA value again.
      continue;
    }
    if (isa<LoadSVFGNode>(current))
      continue;
    for (SVFGEdge *edge : current->getOutEdges())
      if (isBaseValueFlowEdge(edge))
        push(edge);
  }

  return baseSuccessorStores_.emplace(&node, std::move(stores)).first->second;
}

const std::vector<StoreSVFGNode *> &
ThreadAwareSVFGBuilder::basePredecessorStores(SVFGNode &node) {
  auto cached = basePredecessorStores_.find(&node);
  if (cached != basePredecessorStores_.end())
    return cached->second;

  std::vector<StoreSVFGNode *> stores;
  std::unordered_set<SVFGNode *> visited{&node};
  std::vector<SVFGNode *> worklist;
  auto push = [&](const SVFGEdge *edge) {
    SVFGNode *previous = edge ? edge->getSrcNode() : nullptr;
    if (previous && !visited.count(previous) && inScope(previous)) {
      visited.insert(previous);
      worklist.push_back(previous);
    }
  };
  for (SVFGEdge *edge : node.getInEdges())
    if (isBaseValueFlowEdge(edge))
      push(edge);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.back();
    worklist.pop_back();
    if (auto *store = dyn_cast<StoreSVFGNode>(current)) {
      stores.push_back(store);
      continue;
    }
    if (isa<LoadSVFGNode>(current))
      continue;
    for (SVFGEdge *edge : current->getInEdges())
      if (isBaseValueFlowEdge(edge))
        push(edge);
  }

  return basePredecessorStores_.emplace(&node, std::move(stores)).first->second;
}

const SVFGNodeBS &ThreadAwareSVFGBuilder::baseIncomingObjects(SVFGNode &node) {
  auto cached = baseIncomingObjects_.find(&node);
  if (cached != baseIncomingObjects_.end())
    return cached->second;

  SVFGNodeBS objects;
  for (SVFGEdge *edge : node.getInEdges())
    if (isBaseValueFlowEdge(edge)) {
      const SVFGNodeBS &guard = edge->getPointsTo();
      objects.insert(guard.begin(), guard.end());
    }
  return baseIncomingObjects_.emplace(&node, std::move(objects)).first->second;
}

SVFGNodeBS ThreadAwareSVFGBuilder::refineGuardForSpan(StoreSVFGNode &source,
                                                      SVFGNode &destination,
                                                      const SVFGNodeBS &guard) {
  if (!spans_ || guard.empty())
    return guard;

  const Instruction *sourceInst = source.getInstruction();
  const Instruction *destinationInst = destination.getInstruction();
  if (!sourceInst || !destinationInst)
    return guard;

  SVFGNodeBS pruned;

  // (1) Source side: the stored value dies inside its own critical section.
  //
  //     If a later store `killer` in the same span overwrites object `o`, and
  //     every path leaving `source` executes `killer` before the lock is
  //     released, then no other span can ever observe the value `source` wrote
  //     to `o`: the next holder of the lock necessarily sees `killer`'s value.
  //     The edge is redundant rather than wrong, because `killer` is itself a
  //     legal interference source for `destination` (it aliases `o` and may run
  //     in parallel with it), so `killer --> destination` is added by this very
  //     loop. Following that chain of overwrites ends at the last store of the
  //     span, which is never pruned here.
  if (config_.pruneOverwrittenStores) {
    for (StoreSVFGNode *killer : baseSuccessorStores(source)) {
      if (!killer || killer == &source || killer == &destination)
        continue;
      const Instruction *killerInst = killer->getInstruction();
      const SVFGNodeBS *killerObjects = killer->getPointsTo();
      if (!killerInst || !killerObjects || killerObjects->empty())
        continue;
      // Unconditional overwrite: every path from the source reaches it.
      if (!spans_->mustExecuteAfter(sourceInst, killerInst))
        continue;
      if (!spans_->inSameSpan(sourceInst, killerInst))
        continue;
      // The overwrite is only a replacement witness if the killer can itself
      // be observed by the destination.
      if (!mhp_->mayHappenInParallel(killerInst, destinationInst))
        continue;
      for (uint32_t id : guard)
        if (killerObjects->count(id) != 0)
          pruned.insert(id);
    }
  }

  // (2) Sink side: the destination already observes a store from its own span.
  //
  //     If a store `shadow` that dominates `destination` writes object `o`
  //     inside the destination's own span, then every execution of the
  //     destination reads `shadow`'s version of `o` (or a later one), never the
  //     one published by `source`. Dropping the label is only safe when the
  //     base graph already carries an incoming value-flow edge for `o`, which
  //     is exactly what makes the in-span flow explicit.
  if (config_.pruneShadowedSinks) {
    const SVFGNodeBS &incoming = baseIncomingObjects(destination);
    for (StoreSVFGNode *shadow : basePredecessorStores(destination)) {
      if (!shadow || shadow == &destination || incoming.empty())
        continue;
      const Instruction *shadowInst = shadow->getInstruction();
      const SVFGNodeBS *shadowObjects = shadow->getPointsTo();
      if (!shadowInst || !shadowObjects || shadowObjects->empty())
        continue;
      if (!spans_->mustExecuteBefore(shadowInst, destinationInst))
        continue;
      if (!spans_->inSameSpan(shadowInst, destinationInst))
        continue;
      for (uint32_t id : guard)
        if (shadowObjects->count(id) != 0 && incoming.count(id) != 0)
          pruned.insert(id);
    }
  }

  if (pruned.empty())
    return guard;

  stats_.spanPrunedObjects += pruned.size();
  SVFGNodeBS surviving;
  for (uint32_t id : guard)
    if (!pruned.count(id))
      surviving.insert(id);
  return surviving;
}

const ThreadAwareSVFGBuilder::Statistics &ThreadAwareSVFGBuilder::build() {
  clear();
  stats_ = {};
  baseSuccessorStores_.clear();
  basePredecessorStores_.clear();
  baseIncomingObjects_.clear();
  connectForkJoinMemoryFlow();

  std::vector<StoreSVFGNode *> stores;
  std::vector<LoadSVFGNode *> loads;
  for (auto &entry : *graph_) {
    SVFGNode *node = entry.second;
    if (!inScope(node))
      continue;
    if (auto *store = dyn_cast<StoreSVFGNode>(node))
      stores.push_back(store);
    else if (auto *load = dyn_cast<LoadSVFGNode>(node))
      loads.push_back(load);
  }
  stats_.stores = stores.size();
  stats_.loads = loads.size();

  auto process = [&](StoreSVFGNode &source, SVFGNode &destination,
                     mhp::MemoryAccessKind destinationKind) {
    const Instruction *sourceInst = source.getInstruction();
    const Instruction *destinationInst = destination.getInstruction();
    if (!sourceInst || !destinationInst || sourceInst == destinationInst)
      return;

    SVFGNodeBS guard = intersectTargets(source, destination);
    if (guard.empty())
      return;
    ++stats_.aliasCandidates;

    if (!mhp_->mayHappenInParallel(sourceInst, destinationInst))
      return;
    ++stats_.parallelCandidates;

    // A common exclusive lock serializes the two accesses, so a race is
    // impossible; the only flow left is the value published by the critical
    // section and observed by a later one. Lock-span reasoning removes the
    // pairs where the published value provably cannot survive that far.
    if (locks_ &&
        locks_->mustMutuallyExclude(sourceInst, mhp::MemoryAccessKind::Write,
                                    destinationInst, destinationKind)) {
      ++stats_.lockProtectedCandidates;
      guard = refineGuardForSpan(source, destination, guard);
      if (guard.empty()) {
        ++stats_.spanPrunedPairs;
        return;
      }
    }
    addInterferenceEdge(source, destination, guard);
  };

  for (StoreSVFGNode *store : stores)
    for (LoadSVFGNode *load : loads)
      process(*store, *load, mhp::MemoryAccessKind::Read);

  for (std::size_t i = 0; i < stores.size(); ++i) {
    for (std::size_t j = i + 1; j < stores.size(); ++j) {
      process(*stores[i], *stores[j], mhp::MemoryAccessKind::Write);
      process(*stores[j], *stores[i], mhp::MemoryAccessKind::Write);
    }
  }

  return stats_;
}

void ThreadAwareSVFGBuilder::clear() {
  if (!graph_)
    return;
  for (SVFGEdge *edge : overlayEdges_)
    graph_->removeEdge(edge);
  overlayEdges_.clear();
}

} // namespace lotus::analysis
