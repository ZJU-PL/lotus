#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/Core/ConcurrencyContext.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/LockSet/LockSetAnalysisSupport.h"

#include <queue>
#include <set>
#include <stack>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace mhp;

void LockSetAnalysis::computeInterproceduralLockSets() {
  if (!m_call_graph) {
    errs() << "Warning: CallGraph not available. Skipping interprocedural "
              "analysis.\n";
    return;
  }

  errs() << "Computing interprocedural lock sets using CallGraph...\n";

  bottomUpTraversal();

  errs() << "Interprocedural lock set analysis complete.\n";
}

std::set<Function *> LockSetAnalysis::getCallees(const CallBase *call) const {
  std::set<Function *> callees;

  if (!call) {
    return callees;
  }

  if (m_context) {
    const auto &resolved = m_context->resolveCallees(call);
    for (const Function *fn : resolved.targets) {
      if (fn && !fn->isDeclaration()) {
        callees.insert(const_cast<Function *>(fn));
      }
    }
    return callees;
  }

  if (Function *direct_callee = call->getCalledFunction()) {
    if (!direct_callee->isDeclaration()) {
      callees.insert(direct_callee);
    }
    return callees;
  }

  if (const Value *called = call->getCalledOperand()) {
    if (const Function *direct_target =
            dyn_cast<Function>(called->stripPointerCasts())) {
      if (!direct_target->isDeclaration()) {
        callees.insert(const_cast<Function *>(direct_target));
      }
      return callees;
    }

    std::unordered_set<const Value *> visited_values;
    detail::collectDefinedFunctionTargets(called, callees, visited_values);

    if (m_alias_analysis) {
      std::vector<const Value *> pts;
      if (m_alias_analysis->getPointsToSet(called, pts)) {
        for (const Value *target : pts) {
          const auto *fn =
              dyn_cast_or_null<Function>(target ? target->stripPointerCasts()
                                                : nullptr);
          if (!fn || fn->isDeclaration()) {
            continue;
          }
          const FunctionType *fnTy = fn->getFunctionType();
          if (fnTy && (fnTy->isVarArg() ? call->arg_size() >= fnTy->getNumParams()
                                        : call->arg_size() == fnTy->getNumParams())) {
            callees.insert(const_cast<Function *>(fn));
          }
        }
      }
    }
  }

  if (m_call_graph) {
    Function *caller = const_cast<Function *>(call->getFunction());
    if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
      for (auto &callRecord : *cgNode) {
        if (!callRecord.first.hasValue() ||
            dyn_cast_or_null<CallBase>(*callRecord.first) != call) {
          continue;
        }
        if (Function *callee = callRecord.second->getFunction()) {
          if (!callee->isDeclaration()) {
            callees.insert(callee);
          }
        }
      }
    }
  }

  return callees;
}

bool LockSetAnalysis::shouldInvalidateMustLockState(
    const CallBase *call) const {
  if (!call) {
    return false;
  }

  const Function *direct = call->getCalledFunction();
  if (!direct) {
    if (const Value *called = call->getCalledOperand()) {
      direct = dyn_cast<Function>(called->stripPointerCasts());
    }
  }
  if (direct &&
      (direct->isIntrinsic() ||
       concurrency::ConcurrencyContext::isKnownSyncFreeExternalFunction(
           direct))) {
    return false;
  }

  if (call->doesNotAccessMemory() || call->onlyReadsMemory()) {
    return false;
  }

  if (m_context) {
    const auto &resolved = m_context->resolveCallees(call);
    return resolved.has_unresolved || resolved.targets.empty();
  }

  return getCallees(call).empty() || hasUnresolvedCalleeTarget(call);
}

bool LockSetAnalysis::hasUnresolvedCalleeTarget(const CallBase *call) const {
  if (!call) {
    return false;
  }

  if (m_context) {
    return m_context->resolveCallees(call).has_unresolved;
  }

  if (Function *direct = call->getCalledFunction()) {
    return direct->isDeclaration() && !direct->isIntrinsic() &&
           !concurrency::ConcurrencyContext::isKnownSyncFreeExternalFunction(
               direct);
  }

  if (const Value *called = call->getCalledOperand()) {
    if (const auto *direct_target =
            dyn_cast<Function>(called->stripPointerCasts())) {
      return direct_target->isDeclaration() && !direct_target->isIntrinsic() &&
             !concurrency::ConcurrencyContext::isKnownSyncFreeExternalFunction(
                 direct_target);
    }
  }

  if (!m_call_graph) {
    return true;
  }

  Function *caller = const_cast<Function *>(call->getFunction());
  if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
    bool matched_record = false;
    for (auto &callRecord : *cgNode) {
      if (!callRecord.first.hasValue() ||
          dyn_cast_or_null<CallBase>(*callRecord.first) != call) {
        continue;
      }
      matched_record = true;
      CallGraphNode *callee_node = callRecord.second;
      if (!callee_node) {
        return true;
      }
      Function *callee = callee_node->getFunction();
      if (!callee || (callee->isDeclaration() && !callee->isIntrinsic())) {
        return true;
      }
    }
    return !matched_record;
  }

  return true;
}

void LockSetAnalysis::computeFunctionSummary(Function *func) {
  if (!func || func->isDeclaration()) {
    return;
  }

  auto &summary = m_function_summaries[func];

  computeIntraproceduralLockSets(func);

  summary.may_acquire_delta.clear();
  summary.may_read_acquire_delta.clear();
  summary.may_write_acquire_delta.clear();
  summary.must_acquire_delta.clear();
  summary.must_read_acquire_delta.clear();
  summary.must_write_acquire_delta.clear();
  summary.may_release_delta.clear();
  summary.must_release_delta.clear();
  summary.exceptional_may_acquire_delta.clear();
  summary.exceptional_may_read_acquire_delta.clear();
  summary.exceptional_may_write_acquire_delta.clear();
  summary.exceptional_must_acquire_delta.clear();
  summary.exceptional_must_read_acquire_delta.clear();
  summary.exceptional_must_write_acquire_delta.clear();
  summary.exceptional_may_release_delta.clear();
  summary.exceptional_must_release_delta.clear();
  summary.may_recursive_acquire_delta.clear();
  summary.must_recursive_acquire_delta.clear();
  summary.may_recursive_release_delta.clear();
  summary.must_recursive_release_delta.clear();
  summary.exceptional_may_recursive_acquire_delta.clear();
  summary.exceptional_must_recursive_acquire_delta.clear();
  summary.exceptional_may_recursive_release_delta.clear();
  summary.exceptional_must_recursive_release_delta.clear();
  summary.has_exceptional_exit = false;

  auto matchesLock = [this](LockID lhs, LockID rhs) {
    const LockID clhs = getCanonicalLock(lhs);
    const LockID crhs = getCanonicalLock(rhs);
    if (clhs && crhs && clhs == crhs) {
      return true;
    }
    return m_alias_analysis && clhs && crhs &&
           m_alias_analysis->mustAlias(clhs, crhs);
  };

  auto isDefinitelyHeldAt = [&](const Instruction *inst, LockID lock) {
    auto it = m_must_locksets_entry.find(inst);
    return it != m_must_locksets_entry.end() &&
           llvm::any_of(it->second,
                        [&](LockID held) { return matchesLock(held, lock); });
  };

  auto isPossiblyHeldAt = [&](const Instruction *inst, LockID lock) {
    auto it = m_may_locksets_entry.find(inst);
    return it != m_may_locksets_entry.end() &&
           llvm::any_of(it->second,
                        [&](LockID held) { return matchesLock(held, lock); });
  };

  std::unique_ptr<DominatorTree> dom;
  auto getDom = [&]() -> DominatorTree & {
    if (!dom) {
      dom = std::make_unique<DominatorTree>(*func);
    }
    return *dom;
  };
  auto intersectMustSets = [&](const LockSet &lhs, const LockSet &rhs) {
    return merge({lhs, rhs}, true);
  };

  std::vector<const ReturnInst *> returns;
  std::vector<const Instruction *> exceptional_exits;
  for (const BasicBlock &bb : *func) {
    if (const ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
      returns.push_back(ret);
    } else if (const auto *resume = dyn_cast<ResumeInst>(bb.getTerminator())) {
      exceptional_exits.push_back(resume);
    } else if (const auto *cleanup =
                   dyn_cast<CleanupReturnInst>(bb.getTerminator())) {
      if (cleanup->unwindsToCaller()) {
        exceptional_exits.push_back(cleanup);
      }
    } else if (const auto *catch_switch =
                   dyn_cast<CatchSwitchInst>(bb.getTerminator())) {
      if (catch_switch->unwindsToCaller()) {
        exceptional_exits.push_back(catch_switch);
      }
    }
  }
  summary.has_exceptional_exit = !exceptional_exits.empty();

  auto exitMayObserveUnmatchedRelease =
      [&](const Instruction *exit,
          const std::vector<const Instruction *> &release_sites) {
        if (!exit) {
          return false;
        }
        auto blockCanReach = [](const BasicBlock *from, const BasicBlock *to) {
          if (!from || !to) {
            return false;
          }
          if (from == to) {
            return true;
          }
          std::queue<const BasicBlock *> worklist;
          std::set<const BasicBlock *> visited;
          worklist.push(from);
          visited.insert(from);
          while (!worklist.empty()) {
            const BasicBlock *current = worklist.front();
            worklist.pop();
            for (const BasicBlock *succ : successors(current)) {
              if (!visited.insert(succ).second) {
                continue;
              }
              if (succ == to) {
                return true;
              }
              worklist.push(succ);
            }
          }
          return false;
        };
        for (const Instruction *release_inst : release_sites) {
          if (!release_inst || release_inst->getFunction() != func) {
            continue;
          }
          if (release_inst->getParent() == exit->getParent()) {
            if (release_inst->comesBefore(exit)) {
              return true;
            }
            continue;
          }
          if (blockCanReach(release_inst->getParent(), exit->getParent())) {
            return true;
          }
        }
        return false;
      };

  auto exitMustObserveUnmatchedRelease =
      [&](const Instruction *exit,
          const std::vector<const Instruction *> &release_sites) {
        if (!exit) {
          return false;
        }
        for (const Instruction *release_inst : release_sites) {
          if (!release_inst || release_inst->getFunction() != func) {
            continue;
          }
          if (release_inst->getParent() == exit->getParent()) {
            if (release_inst->comesBefore(exit)) {
              return true;
            }
            continue;
          }
          if (getDom().dominates(release_inst->getParent(),
                                 exit->getParent())) {
            return true;
          }
        }
        return false;
      };

  if (!returns.empty()) {
    bool seeded_must_read = false;
    bool seeded_must_write = false;
    LockSet must_read_intersection;
    LockSet must_write_intersection;
    for (const auto *ret : returns) {
      auto it_read = m_may_read_locks_exit.find(ret);
      if (it_read != m_may_read_locks_exit.end()) {
        for (LockID lock : it_read->second) {
          summary.may_read_acquire_delta.insert(lock);
        }
      }

      auto it_write = m_may_write_locks_exit.find(ret);
      if (it_write != m_may_write_locks_exit.end()) {
        for (LockID lock : it_write->second) {
          summary.may_write_acquire_delta.insert(lock);
        }
      }

      auto it_must_read = m_must_read_locks_exit.find(ret);
      if (it_must_read != m_must_read_locks_exit.end()) {
        if (!seeded_must_read) {
          must_read_intersection = it_must_read->second;
          seeded_must_read = true;
        } else {
          must_read_intersection =
              intersectMustSets(must_read_intersection, it_must_read->second);
        }
      } else {
        must_read_intersection.clear();
        seeded_must_read = true;
      }

      auto it_must_write = m_must_write_locks_exit.find(ret);
      if (it_must_write != m_must_write_locks_exit.end()) {
        if (!seeded_must_write) {
          must_write_intersection = it_must_write->second;
          seeded_must_write = true;
        } else {
          must_write_intersection =
              intersectMustSets(must_write_intersection, it_must_write->second);
        }
      } else {
        must_write_intersection.clear();
        seeded_must_write = true;
      }
    }

    summary.may_acquire_delta = summary.may_read_acquire_delta;
    summary.may_acquire_delta.insert(summary.may_write_acquire_delta.begin(),
                                     summary.may_write_acquire_delta.end());

    if (seeded_must_read) {
      for (LockID lock : must_read_intersection) {
        summary.must_read_acquire_delta.insert(lock);
      }
    }
    if (seeded_must_write) {
      for (LockID lock : must_write_intersection) {
        summary.must_write_acquire_delta.insert(lock);
      }
    }

    summary.must_acquire_delta = summary.must_read_acquire_delta;
    summary.must_acquire_delta.insert(summary.must_write_acquire_delta.begin(),
                                      summary.must_write_acquire_delta.end());

    bool seeded_must_depth = false;
    for (const ReturnInst *ret : returns) {
      auto may_depth_it = m_may_recursive_depth_exit.find(ret);
      if (may_depth_it != m_may_recursive_depth_exit.end()) {
        for (const auto &[lock, depth] : may_depth_it->second) {
          summary.may_recursive_acquire_delta[lock] =
              std::max(summary.may_recursive_acquire_delta[lock], depth);
        }
      }
      auto must_depth_it = m_must_recursive_depth_exit.find(ret);
      if (!seeded_must_depth) {
        if (must_depth_it != m_must_recursive_depth_exit.end()) {
          summary.must_recursive_acquire_delta = must_depth_it->second;
        }
        seeded_must_depth = true;
      } else if (must_depth_it == m_must_recursive_depth_exit.end()) {
        summary.must_recursive_acquire_delta.clear();
      } else {
        for (auto it = summary.must_recursive_acquire_delta.begin();
             it != summary.must_recursive_acquire_delta.end();) {
          auto other = must_depth_it->second.find(it->first);
          if (other == must_depth_it->second.end()) {
            it = summary.must_recursive_acquire_delta.erase(it);
          } else {
            it->second = std::min(it->second, other->second);
            ++it;
          }
        }
      }
    }
  }

  if (!exceptional_exits.empty()) {
    bool seeded_must_read = false;
    bool seeded_must_write = false;
    LockSet must_read_intersection;
    LockSet must_write_intersection;
    for (const Instruction *exit : exceptional_exits) {
      auto it_read = m_may_read_locks_exit.find(exit);
      if (it_read != m_may_read_locks_exit.end()) {
        for (LockID lock : it_read->second) {
          summary.exceptional_may_read_acquire_delta.insert(lock);
        }
      }
      auto it_write = m_may_write_locks_exit.find(exit);
      if (it_write != m_may_write_locks_exit.end()) {
        for (LockID lock : it_write->second) {
          summary.exceptional_may_write_acquire_delta.insert(lock);
        }
      }

      auto it_must_read = m_must_read_locks_exit.find(exit);
      if (it_must_read != m_must_read_locks_exit.end()) {
        must_read_intersection = seeded_must_read
                                     ? intersectMustSets(must_read_intersection,
                                                         it_must_read->second)
                                     : it_must_read->second;
        seeded_must_read = true;
      } else {
        must_read_intersection.clear();
        seeded_must_read = true;
      }

      auto it_must_write = m_must_write_locks_exit.find(exit);
      if (it_must_write != m_must_write_locks_exit.end()) {
        must_write_intersection =
            seeded_must_write ? intersectMustSets(must_write_intersection,
                                                  it_must_write->second)
                              : it_must_write->second;
        seeded_must_write = true;
      } else {
        must_write_intersection.clear();
        seeded_must_write = true;
      }
    }

    summary.exceptional_may_acquire_delta =
        summary.exceptional_may_read_acquire_delta;
    summary.exceptional_may_acquire_delta.insert(
        summary.exceptional_may_write_acquire_delta.begin(),
        summary.exceptional_may_write_acquire_delta.end());
    if (seeded_must_read) {
      for (LockID lock : must_read_intersection) {
        summary.exceptional_must_read_acquire_delta.insert(lock);
      }
    }
    if (seeded_must_write) {
      for (LockID lock : must_write_intersection) {
        summary.exceptional_must_write_acquire_delta.insert(lock);
      }
    }
    summary.exceptional_must_acquire_delta =
        summary.exceptional_must_read_acquire_delta;
    summary.exceptional_must_acquire_delta.insert(
        summary.exceptional_must_write_acquire_delta.begin(),
        summary.exceptional_must_write_acquire_delta.end());

    bool seeded_must_depth = false;
    for (const Instruction *exit : exceptional_exits) {
      auto may_depth_it = m_may_recursive_depth_exit.find(exit);
      if (may_depth_it != m_may_recursive_depth_exit.end()) {
        for (const auto &[lock, depth] : may_depth_it->second) {
          summary.exceptional_may_recursive_acquire_delta[lock] = std::max(
              summary.exceptional_may_recursive_acquire_delta[lock], depth);
        }
      }
      auto must_depth_it = m_must_recursive_depth_exit.find(exit);
      if (!seeded_must_depth) {
        if (must_depth_it != m_must_recursive_depth_exit.end()) {
          summary.exceptional_must_recursive_acquire_delta =
              must_depth_it->second;
        }
        seeded_must_depth = true;
      } else if (must_depth_it == m_must_recursive_depth_exit.end()) {
        summary.exceptional_must_recursive_acquire_delta.clear();
      } else {
        for (auto it =
                 summary.exceptional_must_recursive_acquire_delta.begin();
             it != summary.exceptional_must_recursive_acquire_delta.end();) {
          auto other = must_depth_it->second.find(it->first);
          if (other == must_depth_it->second.end()) {
            it = summary.exceptional_must_recursive_acquire_delta.erase(it);
          } else {
            it->second = std::min(it->second, other->second);
            ++it;
          }
        }
      }
    }
  }

  std::unordered_map<LockID, std::vector<const Instruction *>>
      maybe_unmatched_releases;
  std::unordered_map<LockID, std::vector<const Instruction *>>
      must_unmatched_releases;
  LockSet recursive_release_locks;
  for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
    Instruction *inst = &*I;
    if (const auto *call = dyn_cast<CallBase>(inst)) {
      const Function *callee = m_thread_api->getCallee(call);
      if (callee && callee->getName().contains("recursive_mutex") &&
          m_thread_api->isTDRelease(inst)) {
        if (LockID lock = getLockValue(inst)) {
          recursive_release_locks.insert(lock);
        }
      }
    }
    std::vector<LockID> maybe_released =
        getRAIILocksReleasedAt(inst, true);
    std::vector<LockID> definitely_released =
        getRAIILocksReleasedAt(inst, false);
    if (!maybe_released.empty() || !definitely_released.empty()) {
      for (LockID lock : maybe_released) {
        if (!isDefinitelyHeldAt(inst, lock)) {
          maybe_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
      for (LockID lock : definitely_released) {
        if (!isPossiblyHeldAt(inst, lock)) {
          must_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
      continue;
    }
    if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
      continue;
    }
    if (m_thread_api->isTDRelease(inst) && !m_thread_api->isTDAcquire(inst)) {
      if (LockID lock = getLockValue(inst)) {
        if (!isDefinitelyHeldAt(inst, lock)) {
          maybe_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
        if (!isPossiblyHeldAt(inst, lock)) {
          must_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
    }
  }

  for (const auto &entry : maybe_unmatched_releases) {
    bool reaches_any_return = false;
    for (const ReturnInst *ret : returns) {
      reaches_any_return |= exitMayObserveUnmatchedRelease(ret, entry.second);
    }
    if (reaches_any_return) {
      summary.may_release_delta.insert(entry.first);
    }
  }

  for (const auto &entry : must_unmatched_releases) {
    bool covers_all_returns = !returns.empty();
    for (const ReturnInst *ret : returns) {
      covers_all_returns &= exitMustObserveUnmatchedRelease(ret, entry.second);
    }
    if (covers_all_returns) {
      summary.must_release_delta.insert(entry.first);
    }
  }

  auto isRecursiveReleaseLock = [&](LockID lock) {
    return llvm::any_of(recursive_release_locks, [&](LockID candidate) {
      return matchesLock(lock, candidate);
    });
  };
  for (LockID lock : summary.may_release_delta) {
    if (isRecursiveReleaseLock(lock)) {
      summary.may_recursive_release_delta[lock] = 1;
    }
  }
  for (LockID lock : summary.must_release_delta) {
    if (isRecursiveReleaseLock(lock)) {
      summary.must_recursive_release_delta[lock] = 1;
    }
  }
  for (const auto &entry : maybe_unmatched_releases) {
    bool reaches_any_exit = false;
    for (const Instruction *exit : exceptional_exits) {
      reaches_any_exit |= exitMayObserveUnmatchedRelease(exit, entry.second);
    }
    if (reaches_any_exit) {
      summary.exceptional_may_release_delta.insert(entry.first);
    }
  }
  for (const auto &entry : must_unmatched_releases) {
    bool covers_all_exits = !exceptional_exits.empty();
    for (const Instruction *exit : exceptional_exits) {
      covers_all_exits &= exitMustObserveUnmatchedRelease(exit, entry.second);
    }
    if (covers_all_exits) {
      summary.exceptional_must_release_delta.insert(entry.first);
    }
  }
  for (LockID lock : summary.exceptional_may_release_delta) {
    if (isRecursiveReleaseLock(lock)) {
      summary.exceptional_may_recursive_release_delta[lock] = 1;
    }
  }
  for (LockID lock : summary.exceptional_must_release_delta) {
    if (isRecursiveReleaseLock(lock)) {
      summary.exceptional_must_recursive_release_delta[lock] = 1;
    }
  }

  for (LockID lock : getImpreciseRAIILocksInFunction(func)) {
    summary.may_acquire_delta.erase(lock);
    summary.may_read_acquire_delta.erase(lock);
    summary.may_write_acquire_delta.erase(lock);
    summary.must_acquire_delta.erase(lock);
    summary.must_read_acquire_delta.erase(lock);
    summary.must_write_acquire_delta.erase(lock);
    summary.exceptional_may_acquire_delta.erase(lock);
    summary.exceptional_may_read_acquire_delta.erase(lock);
    summary.exceptional_may_write_acquire_delta.erase(lock);
    summary.exceptional_must_acquire_delta.erase(lock);
    summary.exceptional_must_read_acquire_delta.erase(lock);
    summary.exceptional_must_write_acquire_delta.erase(lock);
  }

  summary.is_analyzed = true;
}

bool LockSetAnalysis::applyExceptionalFunctionSummary(
    const CallBase *call, const Function *callee, LockSet &may_locks,
    LockSet &must_locks) const {
  if (!call || !callee) {
    return false;
  }
  auto it = m_function_summaries.find(callee);
  if (it == m_function_summaries.end() || !it->second.is_analyzed ||
      !it->second.has_exceptional_exit) {
    return false;
  }

  const FunctionSummary &summary = it->second;
  for (LockID lock : summary.exceptional_may_acquire_delta) {
    if (LockID instantiated = instantiateSummaryLock(call, callee, lock)) {
      may_locks.insert(instantiated);
    }
  }
  for (LockID lock : summary.exceptional_must_acquire_delta) {
    if (LockID instantiated = instantiateSummaryLock(call, callee, lock)) {
      must_locks.insert(instantiated);
    }
  }
  for (LockID summary_lock : summary.exceptional_may_release_delta) {
    LockID lock = instantiateSummaryLock(call, callee, summary_lock);
    if (!lock) {
      continue;
    }
    LockSet to_remove;
    for (LockID held : must_locks) {
      if (mayAlias(held, lock)) {
        to_remove.insert(held);
      }
    }
    for (LockID held : to_remove) {
      must_locks.erase(held);
    }
  }
  for (LockID summary_lock : summary.exceptional_must_release_delta) {
    LockID lock = instantiateSummaryLock(call, callee, summary_lock);
    if (!lock) {
      continue;
    }
    may_locks.erase(lock);
    LockSet to_remove;
    for (LockID held : must_locks) {
      if (mayAlias(held, lock)) {
        to_remove.insert(held);
      }
    }
    for (LockID held : to_remove) {
      must_locks.erase(held);
    }
  }
  return true;
}

void LockSetAnalysis::applyFunctionSummary(const CallBase *call,
                                           const Function *callee,
                                           LockSet &may_locks,
                                           LockSet &must_locks) const {
  if (!call || !callee) {
    return;
  }

  auto it = m_function_summaries.find(callee);
  if (it == m_function_summaries.end() || !it->second.is_analyzed) {
    return;
  }

  const FunctionSummary &summary = it->second;

  for (LockID lock : summary.may_acquire_delta) {
    if (LockID instantiated = instantiateSummaryLock(call, callee, lock)) {
      may_locks.insert(instantiated);
    }
  }
  for (LockID lock : summary.must_acquire_delta) {
    if (LockID instantiated = instantiateSummaryLock(call, callee, lock)) {
      must_locks.insert(instantiated);
    }
  }

  for (LockID summary_lock : summary.may_release_delta) {
    LockID lock = instantiateSummaryLock(call, callee, summary_lock);
    if (!lock) {
      continue;
    }
    must_locks.erase(lock);
    LockSet to_remove;
    for (const auto *l : must_locks) {
      if (mayAlias(l, lock)) {
        to_remove.insert(l);
      }
    }
    for (const auto *l : to_remove) {
      must_locks.erase(l);
    }
  }

  for (LockID summary_lock : summary.must_release_delta) {
    LockID lock = instantiateSummaryLock(call, callee, summary_lock);
    if (!lock) {
      continue;
    }
    may_locks.erase(lock);
    must_locks.erase(lock);
    LockSet to_remove;
    for (const auto *l : must_locks) {
      if (mayAlias(l, lock)) {
        to_remove.insert(l);
      }
    }
    for (const auto *l : to_remove) {
      must_locks.erase(l);
    }
  }
}

LockID LockSetAnalysis::instantiateSummaryLock(const CallBase *call,
                                               const Function *callee,
                                               LockID lock) const {
  if (!call || !callee || !lock) {
    return nullptr;
  }

  lock = lock->stripPointerCasts();
  if (const auto *argument = dyn_cast<Argument>(lock)) {
    if (argument->getParent() == callee &&
        argument->getArgNo() < call->arg_size()) {
      return getCanonicalLock(call->getArgOperand(argument->getArgNo()));
    }
  }

  if (const auto *gep = dyn_cast<GetElementPtrInst>(lock)) {
    const Value *base = gep->getPointerOperand()->stripPointerCasts();
    if (const auto *argument = dyn_cast<Argument>(base)) {
      if (argument->getParent() == callee &&
          argument->getArgNo() < call->arg_size()) {
        const Value *actual =
            call->getArgOperand(argument->getArgNo())->stripPointerCasts();
        if (const auto *constant_actual = dyn_cast<Constant>(actual)) {
          SmallVector<Constant *, 4> indices;
          bool constant_indices = true;
          for (const Value *index : gep->indices()) {
            const auto *constant_index = dyn_cast<Constant>(index);
            if (!constant_index) {
              constant_indices = false;
              break;
            }
            indices.push_back(const_cast<Constant *>(constant_index));
          }
          if (constant_indices) {
            Constant *projected = ConstantExpr::getGetElementPtr(
                gep->getSourceElementType(),
                const_cast<Constant *>(constant_actual), indices,
                gep->isInBounds());
            return getCanonicalLock(projected);
          }
        }
        auto cached_projection = m_summary_lock_projections.find({call, lock});
        if (cached_projection != m_summary_lock_projections.end()) {
          return getCanonicalLock(cached_projection->second.get());
        }
        SmallVector<Value *, 4> indices;
        for (const Value *index : gep->indices()) {
          indices.push_back(const_cast<Value *>(index));
        }
        if (!indices.empty()) {
          auto projected = std::unique_ptr<GetElementPtrInst>(
              GetElementPtrInst::Create(
                  gep->getSourceElementType(), const_cast<Value *>(actual),
                  indices, "lock.summary.projection"));
          projected->setIsInBounds(gep->isInBounds());
          LockID raw = projected.get();
          m_summary_lock_projections[{call, lock}] = std::move(projected);
          return getCanonicalLock(raw);
        }
        return getCanonicalLock(actual);
      }
    }
  }

  return getCanonicalLock(lock);
}

void LockSetAnalysis::bottomUpTraversal() {
  if (!m_call_graph) {
    return;
  }

  errs() << "Performing bottom-up call graph traversal...\n";

  std::vector<Function *> post_order;
  std::set<Function *> visited;
  std::stack<std::pair<Function *, bool>> stack;

  for (Function &func : *m_module) {
    if (func.isDeclaration() || !m_lock_relevant_functions.count(&func) ||
        visited.find(&func) != visited.end()) {
      continue;
    }
    stack.push({&func, false});

    while (!stack.empty()) {
      auto [current, children_visited] = stack.top();
      stack.pop();

      if (children_visited) {
        post_order.push_back(current);
        continue;
      }

      if (visited.find(current) != visited.end()) {
        continue;
      }

      visited.insert(current);
      stack.push({current, true});

      if (CallGraphNode *cgNode = (*m_call_graph)[current]) {
        for (auto &callRecord : *cgNode) {
          if (Function *callee = callRecord.second->getFunction()) {
            if (callee && !callee->isDeclaration() &&
                m_lock_relevant_functions.count(callee) &&
                visited.find(callee) == visited.end()) {
              stack.push({callee, false});
            }
          }
        }
      }
    }
  }

  errs() << "Processing " << post_order.size()
         << " functions in bottom-up order\n";

  auto summariesEqual = [](const FunctionSummary &lhs,
                           const FunctionSummary &rhs) {
    return lhs.is_analyzed == rhs.is_analyzed &&
           lhs.may_acquire_delta == rhs.may_acquire_delta &&
           lhs.may_read_acquire_delta == rhs.may_read_acquire_delta &&
           lhs.may_write_acquire_delta == rhs.may_write_acquire_delta &&
           lhs.must_acquire_delta == rhs.must_acquire_delta &&
           lhs.must_read_acquire_delta == rhs.must_read_acquire_delta &&
           lhs.must_write_acquire_delta == rhs.must_write_acquire_delta &&
           lhs.may_release_delta == rhs.may_release_delta &&
           lhs.must_release_delta == rhs.must_release_delta &&
           lhs.exceptional_may_acquire_delta ==
               rhs.exceptional_may_acquire_delta &&
           lhs.exceptional_may_read_acquire_delta ==
               rhs.exceptional_may_read_acquire_delta &&
           lhs.exceptional_may_write_acquire_delta ==
               rhs.exceptional_may_write_acquire_delta &&
           lhs.exceptional_must_acquire_delta ==
               rhs.exceptional_must_acquire_delta &&
           lhs.exceptional_must_read_acquire_delta ==
               rhs.exceptional_must_read_acquire_delta &&
           lhs.exceptional_must_write_acquire_delta ==
               rhs.exceptional_must_write_acquire_delta &&
           lhs.exceptional_may_release_delta ==
               rhs.exceptional_may_release_delta &&
           lhs.exceptional_must_release_delta ==
               rhs.exceptional_must_release_delta &&
           lhs.may_recursive_acquire_delta ==
               rhs.may_recursive_acquire_delta &&
           lhs.must_recursive_acquire_delta ==
               rhs.must_recursive_acquire_delta &&
           lhs.may_recursive_release_delta ==
               rhs.may_recursive_release_delta &&
           lhs.must_recursive_release_delta ==
               rhs.must_recursive_release_delta &&
           lhs.exceptional_may_recursive_acquire_delta ==
               rhs.exceptional_may_recursive_acquire_delta &&
           lhs.exceptional_must_recursive_acquire_delta ==
               rhs.exceptional_must_recursive_acquire_delta &&
           lhs.exceptional_may_recursive_release_delta ==
               rhs.exceptional_may_recursive_release_delta &&
           lhs.exceptional_must_recursive_release_delta ==
               rhs.exceptional_must_recursive_release_delta &&
           lhs.has_exceptional_exit == rhs.has_exceptional_exit;
  };

  bool changed = false;
  size_t iteration = 0;
  do {
    changed = false;
    ++iteration;
    for (Function *func : post_order) {
      const FunctionSummary previous = m_function_summaries[func];
      computeFunctionSummary(func);
      changed |= !summariesEqual(previous, m_function_summaries[func]);
    }
  } while (changed);

  errs() << "Bottom-up traversal reached summary fixpoint in " << iteration
         << " iterations\n";
}
