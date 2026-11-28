/**
 * @file LockSetAnalysis.cpp
 * @brief Implementation of Lock Set Analysis
 *
 * This analysis tracks the set of locks held at each program point.
 * It computes two sets for each instruction:
 * 1. May-Lock Set: Locks that MIGHT be held. Used for deadlock detection and
 * reducing false positives.
 *    - Join operator: Union
 *    - Try-lock: Assumed successful
 * 2. Must-Lock Set: Locks that MUST be held. Used for proving mutual exclusion
 * (safety).
 *    - Join operator: Intersection
 *    - Try-lock: Assumed failed (safe approximation)
 *    - Release: Removes all aliasing locks to ensure soundness.
 */

#include "Concurrency/LockSet/LockSetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/LockSet/LockSetAnalysisSupport.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>

#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace mhp::detail {

bool isNonBinarySemaphoreOp(const ThreadAPI *thread_api,
                            const Instruction *inst) {
  return thread_api && inst && thread_api->isSemaphoreOp(inst) &&
         !thread_api->isBinarySemaphoreOp(inst);
}

void collectDefinedFunctionTargets(const Value *called,
                                   std::set<Function *> &callees,
                                   std::unordered_set<const Value *> &visited) {
  if (!called) {
    return;
  }
  called = called->stripPointerCasts();
  if (!visited.insert(called).second) {
    return;
  }

  if (const auto *func = dyn_cast<Function>(called)) {
    if (!func->isDeclaration()) {
      callees.insert(const_cast<Function *>(func));
    }
    return;
  }
  if (const auto *select = dyn_cast<SelectInst>(called)) {
    collectDefinedFunctionTargets(select->getTrueValue(), callees, visited);
    collectDefinedFunctionTargets(select->getFalseValue(), callees, visited);
    return;
  }
  if (const auto *phi = dyn_cast<PHINode>(called)) {
    for (const Value *incoming : phi->incoming_values()) {
      collectDefinedFunctionTargets(incoming, callees, visited);
    }
    return;
  }
  if (const auto *ce = dyn_cast<ConstantExpr>(called)) {
    if (ce->isCast() && ce->getNumOperands() > 0) {
      collectDefinedFunctionTargets(ce->getOperand(0), callees, visited);
    }
  }
}

bool getConstantOffsetPointerInfo(const Value *ptr, const Module *module,
                                  const Value *&base, int64_t &offset,
                                  uint64_t &size) {
  if (!ptr || !module) {
    return false;
  }

  ptr = ptr->stripPointerCasts();
  const auto *gep = dyn_cast<GEPOperator>(ptr);
  if (!gep) {
    return false;
  }

  const DataLayout &dl = module->getDataLayout();
  APInt ap_offset(dl.getIndexTypeSizeInBits(gep->getType()), 0, true);
  if (!gep->accumulateConstantOffset(dl, ap_offset)) {
    return false;
  }

  const Value *ptr_base = gep->getPointerOperand()->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(ptr_base, 32)) {
    ptr_base = underlying->stripPointerCasts();
  }

  Type *pointee_ty = gep->getResultElementType();
  if (!pointee_ty || !pointee_ty->isSized()) {
    return false;
  }

  base = ptr_base;
  offset = ap_offset.getSExtValue();
  size = dl.getTypeStoreSize(pointee_ty);
  return true;
}

bool areDisjointConstantOffsetPointers(const Value *lhs, const Value *rhs,
                                       const Module *module) {
  const Value *lhs_base = nullptr;
  const Value *rhs_base = nullptr;
  int64_t lhs_offset = 0;
  int64_t rhs_offset = 0;
  uint64_t lhs_size = 0;
  uint64_t rhs_size = 0;
  if (!getConstantOffsetPointerInfo(lhs, module, lhs_base, lhs_offset,
                                    lhs_size) ||
      !getConstantOffsetPointerInfo(rhs, module, rhs_base, rhs_offset,
                                    rhs_size)) {
    return false;
  }

  if (lhs_base != rhs_base) {
    return false;
  }

  const int64_t lhs_end = lhs_offset + static_cast<int64_t>(lhs_size);
  const int64_t rhs_end = rhs_offset + static_cast<int64_t>(rhs_size);
  return lhs_end <= rhs_offset || rhs_end <= lhs_offset;
}

bool instructionPrecedesOrEquals(const Instruction *lhs, const Instruction *rhs,
                                 const Function *func) {
  if (!lhs || !rhs || !func) {
    return false;
  }
  if (lhs == rhs) {
    return true;
  }

  for (const Instruction &inst : instructions(func)) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }

  return false;
}

} // namespace mhp::detail

// ============================================================================
// Construction and Analysis
// ============================================================================

LockSetAnalysis::LockSetAnalysis(Module &module)
    : m_module(&module), m_single_function(nullptr),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr),
      m_call_graph(nullptr) {}

LockSetAnalysis::LockSetAnalysis(Function &func)
    : m_module(nullptr), m_single_function(&func),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr),
      m_call_graph(nullptr) {}

void LockSetAnalysis::analyze() {
  errs() << "Starting Lock Set Analysis...\n";

  m_may_locksets_entry.clear();
  m_may_locksets_exit.clear();
  m_must_locksets_entry.clear();
  m_must_locksets_exit.clear();
  m_may_read_locks_entry.clear();
  m_may_read_locks_exit.clear();
  m_may_write_locks_entry.clear();
  m_may_write_locks_exit.clear();
  m_must_read_locks_entry.clear();
  m_must_read_locks_exit.clear();
  m_must_write_locks_entry.clear();
  m_must_write_locks_exit.clear();
  m_may_recursive_depth_entry.clear();
  m_may_recursive_depth_exit.clear();
  m_must_recursive_depth_entry.clear();
  m_must_recursive_depth_exit.clear();
  m_may_raii_ownership_entry.clear();
  m_may_raii_ownership_exit.clear();
  m_must_raii_ownership_entry.clear();
  m_must_raii_ownership_exit.clear();
  m_all_locks.clear();
  m_lock_acquires.clear();
  m_lock_releases.clear();
  m_lock_try_acquires.clear();
  m_observed_lock_orders.clear();
  m_deadlock_wait_orders.clear();
  m_reentrant_locks.clear();
  m_raii_locks.clear();
  m_function_summaries.clear();
  m_invoke_normal_exit.clear();
  m_invoke_unwind_exit.clear();
  m_trylock_edge_refinements.clear();
  m_summary_lock_projections.clear();
  m_canonical_lock_cache.clear();
  m_recursive_acquire_limit = 1;
  m_functions_with_direct_lock_ops.clear();
  m_functions_with_raii_locks.clear();
  m_functions_with_trylocks.clear();
  m_lock_relevant_functions.clear();

  if (m_module && !m_uses_external_call_graph) {
    m_owned_call_graph = std::make_unique<CallGraph>(*m_module);
    m_call_graph = m_owned_call_graph.get();
  }

  prescanLockOperations();

  if (m_functions_with_direct_lock_ops.empty()) {
    errs() << "Lock Set Analysis Complete!\n";
    errs() << "Found 0 locks\n";
    return;
  }

  if (m_module) {
    for (const Function *raii_func : m_functions_with_raii_locks) {
      RAIILock::RAIILockTracker raii_tracker;
      raii_tracker.analyzeFunction(raii_func);
      m_raii_locks[raii_func] = raii_tracker.getAllLockLifetimes();
    }
    for (const Function *trylock_func : m_functions_with_trylocks) {
      detectTryLockSuccessBranches(const_cast<Function *>(trylock_func));
    }
    computeInterproceduralLockSets();
  } else if (m_single_function) {
    // Single function analysis
    analyzeFunction(m_single_function);
  }

  // Identify locks after RAII lifetimes and final summaries have been computed.
  identifyLocks();

  // Track lock ordering for deadlock detection
  trackLockOrdering();

  errs() << "Lock Set Analysis Complete!\n";
  errs() << "Found " << m_all_locks.size() << " locks\n";
}

void LockSetAnalysis::prescanLockOperations() {
  unsigned recursive_acquires = 0;
  std::unordered_map<const Function *, std::vector<const Function *>>
      reverse_call_edges;

  auto hasExceptionalExit = [](const Function &func) {
    for (const BasicBlock &bb : func) {
      const Instruction *term = bb.getTerminator();
      if (isa<ResumeInst>(term)) {
        return true;
      }
      if (const auto *cleanup = dyn_cast<CleanupReturnInst>(term)) {
        if (cleanup->unwindsToCaller()) {
          return true;
        }
      } else if (const auto *catch_switch = dyn_cast<CatchSwitchInst>(term)) {
        if (catch_switch->unwindsToCaller()) {
          return true;
        }
      }
    }
    return false;
  };

  auto scanFunction = [&](Function &func) {
    bool has_lock_op = false;
    bool has_raii = false;
    bool has_trylock = false;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
      if (type != ThreadAPI::TD_DUMMY) {
        const ThreadAPI::LockSemantics sem =
            m_thread_api->describeLockSemantics(&inst);
        if (sem.is_acquire || sem.is_release || isLockOperation(&inst) ||
            ((type == ThreadAPI::TD_UNIQUE_LOCK_DTOR ||
              type == ThreadAPI::TD_SHARED_LOCK_DTOR) &&
             m_thread_api->cppWrapperDestructorReleases(&inst))) {
          has_lock_op = true;
        }
        if (type == ThreadAPI::TD_CPP_LOCK_RELEASE ||
            type == ThreadAPI::TD_CPP_LOCK_MOVE_CTOR ||
            type == ThreadAPI::TD_CPP_LOCK_MOVE_ASSIGN ||
            type == ThreadAPI::TD_CPP_LOCK_SWAP ||
            type == ThreadAPI::TD_CPP_LOCK_TRY ||
            RAIILock::RAIILockTracker::isRAIILockConstructor(call) ||
            RAIILock::RAIILockTracker::isRAIILockDestructor(call)) {
          has_lock_op = true;
          has_raii = true;
        }
        if (m_thread_api->isConditionalLockAcquire(call)) {
          has_lock_op = true;
          has_trylock = true;
        }
      } else if (RAIILock::RAIILockTracker::isRAIILockConstructor(call) ||
                 RAIILock::RAIILockTracker::isRAIILockDestructor(call)) {
        has_lock_op = true;
        has_raii = true;
      }
      const Function *callee = m_thread_api->getCallee(call);
      if (callee && callee->getName().contains("recursive_mutex") &&
          m_thread_api->isTDAcquire(&inst) && !m_thread_api->isTryLock(&inst)) {
        ++recursive_acquires;
      }
      if (m_module) {
        if (callee && !callee->isDeclaration()) {
          reverse_call_edges[callee].push_back(&func);
        } else if (!callee) {
          if (const Value *called = call->getCalledOperand()) {
            std::set<Function *> targets;
            std::unordered_set<const Value *> visited;
            detail::collectDefinedFunctionTargets(called, targets, visited);
            for (Function *target : targets) {
              reverse_call_edges[target].push_back(&func);
            }
          }
        }
      }
    }
    if (has_lock_op) {
      m_functions_with_direct_lock_ops.insert(&func);
    }
    if (has_raii) {
      m_functions_with_raii_locks.insert(&func);
    }
    if (has_trylock) {
      m_functions_with_trylocks.insert(&func);
    }
  };

  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        scanFunction(func);
      }
    }
    if (m_call_graph && !m_functions_with_direct_lock_ops.empty()) {
      for (Function &func : *m_module) {
        if (func.isDeclaration()) {
          continue;
        }
        if (CallGraphNode *cgNode = (*m_call_graph)[&func]) {
          for (auto &record : *cgNode) {
            if (record.second) {
              if (Function *callee = record.second->getFunction()) {
                if (!callee->isDeclaration()) {
                  reverse_call_edges[callee].push_back(&func);
                }
              }
            }
          }
        }
      }
    }

    // Propagate lock relevance backwards from direct lock functions to all
    // transitive callers.
    std::vector<const Function *> worklist(
        m_functions_with_direct_lock_ops.begin(),
        m_functions_with_direct_lock_ops.end());
    m_lock_relevant_functions.insert(worklist.begin(), worklist.end());
    size_t head = 0;
    while (head < worklist.size()) {
      const Function *curr = worklist[head++];
      auto it = reverse_call_edges.find(curr);
      if (it == reverse_call_edges.end()) {
        continue;
      }
      for (const Function *caller : it->second) {
        if (m_lock_relevant_functions.insert(caller).second) {
          worklist.push_back(caller);
        }
      }
    }

    // Seed exact empty summaries for lock-irrelevant functions so unwind edges
    // through throwing helpers still preserve caller must-locksets.
    if (!m_functions_with_direct_lock_ops.empty()) {
      for (Function &func : *m_module) {
        if (func.isDeclaration() || m_lock_relevant_functions.count(&func)) {
          continue;
        }
        FunctionSummary &summary = m_function_summaries[&func];
        summary.is_analyzed = true;
        summary.has_exceptional_exit = hasExceptionalExit(func);
      }
    }
  } else if (m_single_function && !m_single_function->isDeclaration()) {
    scanFunction(*m_single_function);
    if (!m_functions_with_direct_lock_ops.empty()) {
      m_lock_relevant_functions.insert(m_single_function);
    }
  }

  m_recursive_acquire_limit = std::max(1u, recursive_acquires);
}

// ============================================================================
// Query Interface
// ============================================================================

LockSet LockSetAnalysis::getMayLockSetAt(const Instruction *inst) const {
  if (inst && !isa<CallBase>(inst) && !getRAIILocksReleasedAt(inst).empty()) {
    auto it_exit = m_may_locksets_exit.find(inst);
    if (it_exit != m_may_locksets_exit.end()) {
      return it_exit->second;
    }
  }
  // The computed entry fact is authoritative, including an empty set. This is
  // important for edge refinements such as a proven try-lock failure.
  auto it = m_may_locksets_entry.find(inst);
  if (it != m_may_locksets_entry.end())
    return it->second;
  // Fallback: on a linear path, entry at inst = exit of prev (fixes worklist
  // order)
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_may_locksets_exit.find(prev);
    if (it_exit != m_may_locksets_exit.end())
      return it_exit->second;
  }
  // Fallback for block head: union of predecessors' terminator exit (fixes
  // merge/empty entry)
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term)
        continue;
      auto it_exit = m_may_locksets_exit.find(term);
      if (it_exit != m_may_locksets_exit.end())
        merged.insert(it_exit->second.begin(), it_exit->second.end());
    }
    if (!merged.empty())
      return merged;
  }
  return it != m_may_locksets_entry.end() ? it->second : LockSet();
}

LockSet LockSetAnalysis::getMayReadLockSetAt(const Instruction *inst) const {
  auto it = m_may_read_locks_entry.find(inst);
  if (it != m_may_read_locks_entry.end())
    return it->second;
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_may_read_locks_exit.find(prev);
    if (it_exit != m_may_read_locks_exit.end())
      return it_exit->second;
  }
  // Fallback for block head: union of predecessors' terminator exit
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term)
        continue;
      auto it_exit = m_may_read_locks_exit.find(term);
      if (it_exit != m_may_read_locks_exit.end())
        merged.insert(it_exit->second.begin(), it_exit->second.end());
    }
    if (!merged.empty())
      return merged;
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMayWriteLockSetAt(const Instruction *inst) const {
  auto it = m_may_write_locks_entry.find(inst);
  if (it != m_may_write_locks_entry.end())
    return it->second;
  // Fallback: entry at inst = exit of prev on linear path (fixes worklist
  // order)
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_may_write_locks_exit.find(prev);
    if (it_exit != m_may_write_locks_exit.end())
      return it_exit->second;
  }
  // Fallback for block head: union of predecessors' terminator exit (fixes
  // DCL/singleton pattern where critical section starts at block entry)
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term)
        continue;
      auto it_exit = m_may_write_locks_exit.find(term);
      if (it_exit != m_may_write_locks_exit.end())
        merged.insert(it_exit->second.begin(), it_exit->second.end());
    }
    if (!merged.empty())
      return merged;
  }
  return it != m_may_write_locks_entry.end() ? it->second : LockSet();
}

LockSet LockSetAnalysis::getMustLockSetAt(const Instruction *inst) const {
  auto applyImpreciseBoundary = [this, inst](LockSet lockset) {
    for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
      lockset.erase(lock);
    }
    return lockset;
  };
  if (inst && !isa<CallBase>(inst) && !getRAIILocksReleasedAt(inst).empty()) {
    auto it_exit = m_must_locksets_exit.find(inst);
    if (it_exit != m_must_locksets_exit.end()) {
      return applyImpreciseBoundary(it_exit->second);
    }
  }
  auto it = m_must_locksets_entry.find(inst);
  if (it != m_must_locksets_entry.end())
    return applyImpreciseBoundary(it->second);
  // Fallback: entry at inst = exit of prev on linear path (so double-lock sees
  // held lock)
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_must_locksets_exit.find(prev);
    if (it_exit != m_must_locksets_exit.end())
      return applyImpreciseBoundary(it_exit->second);
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMustReadLockSetAt(const Instruction *inst) const {
  auto it = m_must_read_locks_entry.find(inst);
  if (it != m_must_read_locks_entry.end()) {
    LockSet result = it->second;
    for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
      result.erase(lock);
    }
    return result;
  }
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_must_read_locks_exit.find(prev);
    if (it_exit != m_must_read_locks_exit.end()) {
      LockSet result = it_exit->second;
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        result.erase(lock);
      }
      return result;
    }
  }
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    bool initialized = false;
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term) {
        continue;
      }
      auto it_exit = m_must_read_locks_exit.find(term);
      if (it_exit == m_must_read_locks_exit.end()) {
        continue;
      }
      if (!initialized) {
        merged = it_exit->second;
        initialized = true;
      } else {
        LockSet intersection;
        std::set_intersection(
            merged.begin(), merged.end(), it_exit->second.begin(),
            it_exit->second.end(),
            std::inserter(intersection, intersection.begin()));
        merged = std::move(intersection);
      }
    }
    if (initialized) {
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        merged.erase(lock);
      }
      return merged;
    }
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMustWriteLockSetAt(const Instruction *inst) const {
  auto it = m_must_write_locks_entry.find(inst);
  if (it != m_must_write_locks_entry.end()) {
    LockSet result = it->second;
    for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
      result.erase(lock);
    }
    return result;
  }
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_must_write_locks_exit.find(prev);
    if (it_exit != m_must_write_locks_exit.end()) {
      LockSet result = it_exit->second;
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        result.erase(lock);
      }
      return result;
    }
  }
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    bool initialized = false;
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term) {
        continue;
      }
      auto it_exit = m_must_write_locks_exit.find(term);
      if (it_exit == m_must_write_locks_exit.end()) {
        continue;
      }
      if (!initialized) {
        merged = it_exit->second;
        initialized = true;
      } else {
        LockSet intersection;
        std::set_intersection(
            merged.begin(), merged.end(), it_exit->second.begin(),
            it_exit->second.end(),
            std::inserter(intersection, intersection.begin()));
        merged = std::move(intersection);
      }
    }
    if (initialized) {
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        merged.erase(lock);
      }
      return merged;
    }
  }
  return LockSet();
}

bool LockSetAnalysis::mayHoldLock(const Instruction *inst, LockID lock) const {
  auto lockset = getMayLockSetAt(inst);
  for (const auto *held_lock : lockset) {
    if (mayAlias(lock, held_lock)) {
      return true;
    }
  }
  return false;
}

bool LockSetAnalysis::mustHoldLock(const Instruction *inst, LockID lock) const {
  lock = getCanonicalLock(lock);
  auto lockset = getMustLockSetAt(inst);
  for (const auto *held_lock : lockset) {
    const LockID canonical_held = getCanonicalLock(held_lock);
    // Must queries require certainty. Accept either exact canonical equality or
    // a proven must-alias relation.
    if (canonical_held == lock)
      return true;
    if (m_alias_analysis && canonical_held && lock &&
        m_alias_analysis->mustAlias(canonical_held, lock))
      return true;
  }
  return false;
}

std::unordered_set<const Instruction *>
LockSetAnalysis::getInstructionsHoldingLock(LockID lock) const {
  std::unordered_set<const Instruction *> result;
  for (const auto &pair : m_may_locksets_entry) {
    if (pair.second.find(lock) != pair.second.end()) {
      result.insert(pair.first);
    }
  }
  return result;
}

bool LockSetAnalysis::mayHoldCommonLock(const Instruction *i1,
                                        const Instruction *i2) const {
  const Module *module =
      m_module ? m_module
               : (m_single_function ? m_single_function->getParent() : nullptr);
  auto commonWithDisjointFields = [this, module](const LockSet &a,
                                                 const LockSet &b) {
    for (const auto *lock : a) {
      if (b.find(lock) != b.end()) {
        return true;
      }
      for (const auto *lock2 : b) {
        if (detail::areDisjointConstantOffsetPointers(lock, lock2, module)) {
          continue;
        }
        if (mayAlias(lock, lock2)) {
          return true;
        }
      }
    }
    return false;
  };
  LockSet r1 = getMayReadLockSetAt(i1), r2 = getMayReadLockSetAt(i2);
  LockSet w1 = getMayWriteLockSetAt(i1), w2 = getMayWriteLockSetAt(i2);
  return commonWithDisjointFields(w1, w2) || commonWithDisjointFields(r1, r2);
}

bool LockSetAnalysis::locksMustMatch(LockID a, LockID b) const {
  const LockID ca = getCanonicalLock(a);
  const LockID cb = getCanonicalLock(b);
  if (ca && cb && ca == cb) {
    return true;
  }
  return m_alias_analysis && ca && cb && m_alias_analysis->mustAlias(ca, cb);
}

bool LockSetAnalysis::mustHoldCommonLock(const Instruction *i1,
                                         const Instruction *i2) const {
  return mustMutuallyExclude(i1, i2);
}

bool LockSetAnalysis::mustMutuallyExclude(const Instruction *i1,
                                          const Instruction *i2) const {
  return mustMutuallyExclude(i1, MemoryAccessKind::Write, i2,
                             MemoryAccessKind::Write);
}

bool LockSetAnalysis::mustMutuallyExclude(const Instruction *i1,
                                          MemoryAccessKind access1,
                                          const Instruction *i2,
                                          MemoryAccessKind access2) const {
  LockSet must_read_1 = getMustReadLockSetAt(i1);
  LockSet must_write_1 = getMustWriteLockSetAt(i1);
  LockSet must_read_2 = getMustReadLockSetAt(i2);
  LockSet must_write_2 = getMustWriteLockSetAt(i2);

  auto eligibleLocks = [](MemoryAccessKind access_kind,
                          const LockSet &read_locks,
                          const LockSet &write_locks) {
    LockSet eligible = write_locks;
    if (access_kind == MemoryAccessKind::Read) {
      eligible.insert(read_locks.begin(), read_locks.end());
    }
    return eligible;
  };

  const LockSet eligible_1 = eligibleLocks(access1, must_read_1, must_write_1);
  const LockSet eligible_2 = eligibleLocks(access2, must_read_2, must_write_2);

  for (LockID lock1 : eligible_1) {
    const bool first_is_exclusive = must_write_1.count(lock1) != 0;
    for (LockID lock2 : eligible_2) {
      if (!locksMustMatch(lock1, lock2)) {
        continue;
      }
      const bool second_is_exclusive = must_write_2.count(lock2) != 0;
      if (first_is_exclusive || second_is_exclusive) {
        return true;
      }
    }
  }

  return false;
}

LockSet LockSetAnalysis::getAllLocksInFunction(const Function *func) const {
  LockSet all_locks;
  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;
    if (isLockOperation(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) {
        all_locks.insert(lock);
      }
    }
  }
  return all_locks;
}

std::vector<const Instruction *>
LockSetAnalysis::getLockAcquires(LockID lock) const {
  auto it = m_lock_acquires.find(lock);
  if (it != m_lock_acquires.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

std::vector<const Instruction *>
LockSetAnalysis::getLockReleases(LockID lock) const {
  auto it = m_lock_releases.find(lock);
  if (it != m_lock_releases.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

// ============================================================================
// Advanced Queries
// ============================================================================

bool LockSetAnalysis::isReentrantLock(LockID lock) const {
  return m_reentrant_locks.find(lock) != m_reentrant_locks.end();
}

size_t LockSetAnalysis::getLockNestingDepth(const Instruction *inst) const {
  LockSet may_locks = getMayLockSetAt(inst);
  size_t depth = may_locks.size();
  auto depth_it = m_may_recursive_depth_entry.find(inst);
  if (depth_it == m_may_recursive_depth_entry.end()) {
    return depth;
  }
  for (const auto &[lock, recursive_depth] : depth_it->second) {
    if (may_locks.count(lock) != 0 && recursive_depth > 1) {
      const bool unbounded =
          recursive_depth == std::numeric_limits<unsigned>::max();
      depth += unbounded ? 1 : recursive_depth - 1;
    }
  }
  return depth;
}

bool LockSetAnalysis::areLocksOrderedConsistently(LockID lock1,
                                                  LockID lock2) const {
  bool found_12 = m_deadlock_wait_orders.find({lock1, lock2}) !=
                  m_deadlock_wait_orders.end();
  bool found_21 = m_deadlock_wait_orders.find({lock2, lock1}) !=
                  m_deadlock_wait_orders.end();

  // Consistent if only one order is observed
  return !(found_12 && found_21);
}

std::vector<std::pair<LockID, LockID>>
LockSetAnalysis::detectLockOrderInversions() const {
  std::vector<std::pair<LockID, LockID>> inversions;
  std::unordered_set<LockPair, LockPair::Hash> emitted;

  // Check all pairs of locks for order inversions
  for (const auto &pair1 : m_deadlock_wait_orders) {
    LockPair reverse{pair1.second, pair1.first};
    LockPair canonical = std::less<LockID>{}(pair1.first, pair1.second)
                             ? pair1
                             : LockPair{pair1.second, pair1.first};
    if (m_deadlock_wait_orders.find(reverse) != m_deadlock_wait_orders.end() &&
        emitted.insert(canonical).second) {
      // Found an inversion - both lock1->lock2 and lock2->lock1 exist
      inversions.push_back({canonical.first, canonical.second});
    }
  }

  return inversions;
}

// ============================================================================
// Lock Ordering Graph Queries
// ============================================================================

std::vector<LockID> LockSetAnalysis::getLockOrderSuccessors(LockID lock) const {
  std::vector<LockID> successors;
  for (const auto &pair : m_observed_lock_orders) {
    if (pair.first == lock) {
      successors.push_back(pair.second);
    }
  }
  return successors;
}

std::vector<LockID>
LockSetAnalysis::getLockOrderPredecessors(LockID lock) const {
  std::vector<LockID> predecessors;
  for (const auto &pair : m_observed_lock_orders) {
    if (pair.second == lock) {
      predecessors.push_back(pair.first);
    }
  }
  return predecessors;
}

std::vector<std::vector<LockID>> LockSetAnalysis::detectDeadlockCycles() const {
  // Build adjacency list from observed lock orders
  std::unordered_map<LockID, std::vector<LockID>> adj;
  std::unordered_set<LockID> all_nodes;
  for (const auto &pair : m_deadlock_wait_orders) {
    adj[pair.first].push_back(pair.second);
    all_nodes.insert(pair.first);
    all_nodes.insert(pair.second);
  }

  std::vector<std::vector<LockID>> cycles;
  // DFS-based cycle detection
  enum Color { WHITE, GRAY, BLACK };
  std::unordered_map<LockID, Color> color;
  std::unordered_map<LockID, LockID> parent;
  for (LockID node : all_nodes) {
    color[node] = WHITE;
  }

  std::function<void(LockID, std::vector<LockID> &)> dfs =
      [&](LockID u, std::vector<LockID> &path) {
        color[u] = GRAY;
        path.push_back(u);

        auto it = adj.find(u);
        if (it != adj.end()) {
          for (LockID v : it->second) {
            if (color[v] == GRAY) {
              // Found a cycle - extract it from path
              std::vector<LockID> cycle;
              auto cycle_start = std::find(path.begin(), path.end(), v);
              if (cycle_start != path.end()) {
                cycle.assign(cycle_start, path.end());
                cycle.push_back(v); // close the cycle
                cycles.push_back(std::move(cycle));
              }
            } else if (color[v] == WHITE) {
              dfs(v, path);
            }
          }
        }

        path.pop_back();
        color[u] = BLACK;
      };

  for (LockID node : all_nodes) {
    if (color[node] == WHITE) {
      std::vector<LockID> path;
      dfs(node, path);
    }
  }

  return cycles;
}

// ============================================================================
// Critical Section Analysis
// ============================================================================

std::vector<const Instruction *>
LockSetAnalysis::getProtectedInstructions(LockID lock) const {
  std::vector<const Instruction *> result;
  lock = getCanonicalLock(lock);
  if (!lock)
    return result;

  for (const auto &entry : m_must_locksets_entry) {
    for (LockID held : entry.second) {
      if (getCanonicalLock(held) == lock ||
          (m_alias_analysis &&
           m_alias_analysis->mustAlias(getCanonicalLock(held), lock))) {
        result.push_back(entry.first);
        break;
      }
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LockSetAnalysis::getCriticalSections(LockID lock) const {
  std::vector<std::pair<const Instruction *, const Instruction *>> sections;
  auto acquires = getLockAcquires(lock);
  auto releases = getLockReleases(lock);

  for (const Instruction *acq : acquires) {
    // Find the nearest matching release in the same function
    const Instruction *best_release = nullptr;
    for (const Instruction *rel : releases) {
      if (rel->getFunction() != acq->getFunction())
        continue;
      if (detail::instructionPrecedesOrEquals(acq, rel, acq->getFunction())) {
        if (!best_release || detail::instructionPrecedesOrEquals(
                                 rel, best_release, acq->getFunction())) {
          best_release = rel;
        }
      }
    }
    if (best_release) {
      sections.push_back({acq, best_release});
    }
  }
  return sections;
}

size_t LockSetAnalysis::getMaxCriticalSectionSize(LockID lock) const {
  size_t max_size = 0;
  auto sections = getCriticalSections(lock);
  for (const auto &section : sections) {
    size_t count = 0;
    const Function *func = section.first->getFunction();
    bool in_section = false;
    for (const BasicBlock &bb : *func) {
      for (const Instruction &inst : bb) {
        if (&inst == section.first)
          in_section = true;
        if (in_section)
          ++count;
        if (&inst == section.second) {
          in_section = false;
          break;
        }
      }
      if (!in_section && count > 0)
        break;
    }
    max_size = std::max(max_size, count);
  }
  return max_size;
}

// ============================================================================
// Condition Variable Validation
// ============================================================================

bool LockSetAnalysis::isLockHeldAtCondWait(
    const Instruction *cond_wait_inst) const {
  if (!cond_wait_inst || !m_thread_api->isTDCondWait(cond_wait_inst))
    return false;

  const Value *mutex = m_thread_api->getCondMutex(cond_wait_inst);
  if (!mutex)
    return false;

  LockID mutex_lock = getCanonicalLock(mutex);
  if (!mutex_lock)
    return false;

  return mustHoldLock(cond_wait_inst, mutex_lock);
}

std::vector<const Instruction *>
LockSetAnalysis::getUnprotectedCondWaits() const {
  std::vector<const Instruction *> unprotected;

  auto check_func = [&](const Function &func) {
    for (const BasicBlock &bb : func) {
      for (const Instruction &inst : bb) {
        if (!m_thread_api->isTDCondWait(&inst))
          continue;
        if (!isLockHeldAtCondWait(&inst)) {
          unprotected.push_back(&inst);
        }
      }
    }
  };

  if (m_module) {
    for (const Function &func : *m_module) {
      if (!func.isDeclaration())
        check_func(func);
    }
  } else if (m_single_function) {
    check_func(*m_single_function);
  }

  return unprotected;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void LockSetAnalysis::Statistics::print(raw_ostream &os) const {
  os << "Lock Set Analysis Statistics:\n";
  os << "==============================\n";
  os << "Locks:                       " << num_locks << "\n";
  os << "Lock Acquires:               " << num_acquires << "\n";
  os << "Lock Releases:               " << num_releases << "\n";
  os << "Try-Lock Operations:         " << num_try_acquires << "\n";
  os << "Max Nesting Depth:           " << max_nesting_depth << "\n";
  os << "Observed Reentrant Locks:    " << num_reentrant_locks << "\n";
  os << "Potential Deadlocks (pairs): " << num_potential_deadlocks << "\n";
  os << "Functions with Locks:        " << num_functions_with_locks << "\n";
  os << "Critical Sections:           " << num_critical_sections << "\n";
  os << "Avg Critical Section Size:   " << avg_critical_section_size << "\n";
  os << "Max Critical Section Size:   " << max_critical_section_size << "\n";
  os << "Read-Write Locks:            " << num_rwlocks << "\n";
  os << "CondVar Waits:               " << num_condvar_waits << "\n";
  os << "Unprotected CondVar Waits:   " << num_unprotected_condvar_waits
     << "\n";
  os << "Deadlock Cycles:             " << num_deadlock_cycles << "\n";
}

LockSetAnalysis::Statistics LockSetAnalysis::getStatistics() const {
  Statistics stats{};

  stats.num_locks = m_all_locks.size();

  stats.num_acquires = 0;
  for (const auto &pair : m_lock_acquires) {
    stats.num_acquires += pair.second.size();
  }

  stats.num_releases = 0;
  for (const auto &pair : m_lock_releases) {
    stats.num_releases += pair.second.size();
  }

  stats.num_try_acquires = 0;
  for (const auto &pair : m_lock_try_acquires) {
    stats.num_try_acquires += pair.second.size();
  }

  stats.max_nesting_depth = 0;
  for (const auto &pair : m_may_locksets_entry) {
    stats.max_nesting_depth =
        std::max(stats.max_nesting_depth, pair.second.size());
  }

  stats.num_reentrant_locks = m_reentrant_locks.size();
  stats.num_potential_deadlocks = detectLockOrderInversions().size();

  // Functions with lock operations
  stats.num_functions_with_locks = 0;
  if (m_module) {
    for (Function &func : *m_module) {
      if (func.isDeclaration())
        continue;
      bool has_lock = false;
      for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
           ++I) {
        if (isLockOperation(&*I)) {
          has_lock = true;
          break;
        }
      }
      if (has_lock)
        ++stats.num_functions_with_locks;
    }
  } else if (m_single_function) {
    for (inst_iterator I = inst_begin(m_single_function),
                       E = inst_end(m_single_function);
         I != E; ++I) {
      if (isLockOperation(&*I)) {
        stats.num_functions_with_locks = 1;
        break;
      }
    }
  }

  // Critical section metrics
  stats.num_critical_sections = 0;
  stats.max_critical_section_size = 0;
  size_t total_cs_size = 0;
  for (LockID lock : m_all_locks) {
    auto sections = getCriticalSections(lock);
    stats.num_critical_sections += sections.size();
    for (const auto &section : sections) {
      size_t cs_size = 0;
      const Function *func = section.first->getFunction();
      bool in_section = false;
      for (const BasicBlock &bb : *func) {
        for (const Instruction &inst : bb) {
          if (&inst == section.first)
            in_section = true;
          if (in_section)
            ++cs_size;
          if (&inst == section.second) {
            in_section = false;
            break;
          }
        }
        if (!in_section && cs_size > 0)
          break;
      }
      total_cs_size += cs_size;
      stats.max_critical_section_size =
          std::max(stats.max_critical_section_size, cs_size);
    }
  }
  stats.avg_critical_section_size =
      stats.num_critical_sections > 0
          ? static_cast<double>(total_cs_size) / stats.num_critical_sections
          : 0.0;

  // Read-write locks: locks that appear in both read and write lock sets
  stats.num_rwlocks = 0;
  std::unordered_set<LockID> read_locks, write_locks;
  for (const auto &entry : m_may_read_locks_entry) {
    read_locks.insert(entry.second.begin(), entry.second.end());
  }
  for (const auto &entry : m_may_write_locks_entry) {
    write_locks.insert(entry.second.begin(), entry.second.end());
  }
  for (LockID lock : read_locks) {
    if (write_locks.count(lock))
      ++stats.num_rwlocks;
  }

  // Condition variable metrics
  auto unprotected = getUnprotectedCondWaits();
  stats.num_unprotected_condvar_waits = unprotected.size();
  stats.num_condvar_waits = 0;
  auto count_condwaits = [&](const Function &func) {
    for (const BasicBlock &bb : func) {
      for (const Instruction &inst : bb) {
        if (m_thread_api->isTDCondWait(&inst))
          ++stats.num_condvar_waits;
      }
    }
  };
  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration())
        count_condwaits(func);
    }
  } else if (m_single_function) {
    count_condwaits(*m_single_function);
  }

  // Deadlock cycles
  stats.num_deadlock_cycles = detectDeadlockCycles().size();

  return stats;
}

void LockSetAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void LockSetAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== Lock Set Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== All Locks ===\n";
  for (const auto *lock : m_all_locks) {
    os << "Lock: ";
    lock->printAsOperand(os, false);
    os << "\n";

    auto acquires = getLockAcquires(lock);
    os << "  Acquires: " << acquires.size() << "\n";

    auto releases = getLockReleases(lock);
    os << "  Releases: " << releases.size() << "\n";

    if (isReentrantLock(lock)) {
      os << "  [REENTRANT]\n";
    }
  }

  // Print potential deadlocks
  auto inversions = detectLockOrderInversions();
  if (!inversions.empty()) {
    os << "\n=== Potential Deadlocks (Lock Order Inversions) ===\n";
    for (const auto &pair : inversions) {
      os << "Lock ";
      pair.first->printAsOperand(os, false);
      os << " and Lock ";
      pair.second->printAsOperand(os, false);
      os << "\n";
    }
  }

  // Print deadlock cycles
  auto cycles = detectDeadlockCycles();
  if (!cycles.empty()) {
    os << "\n=== Deadlock Cycles (Lock Ordering Graph) ===\n";
    for (size_t i = 0; i < cycles.size(); ++i) {
      os << "Cycle " << (i + 1) << ": ";
      for (size_t j = 0; j < cycles[i].size(); ++j) {
        if (j > 0)
          os << " -> ";
        cycles[i][j]->printAsOperand(os, false);
      }
      os << "\n";
    }
  }

  // Print unprotected condition variable waits
  auto unprotected_cvs = getUnprotectedCondWaits();
  if (!unprotected_cvs.empty()) {
    os << "\n=== Unprotected Condition Variable Waits ===\n";
    for (const auto *inst : unprotected_cvs) {
      os << "  ";
      inst->print(os);
      os << "\n";
    }
  }
}

void LockSetAnalysis::printLockSetsForFunction(const Function *func,
                                               raw_ostream &os) const {
  os << "Lock Sets for Function: " << func->getName() << "\n";
  os << "=============================================\n";

  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;

    auto may_locks = getMayLockSetAt(inst);
    auto must_locks = getMustLockSetAt(inst);

    if (!may_locks.empty() || isLockOperation(inst)) {
      os << "Instruction: ";
      inst->print(os);
      os << "\n";

      os << "  May-Locks: {";
      bool first = true;
      for (const auto *lock : may_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n";

      os << "  Must-Locks: {";
      first = true;
      for (const auto *lock : must_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n\n";
    }
  }
}

void LockSetAnalysis::print(raw_ostream &os) const { printResults(os); }

// ============================================================================
// Visualization
// ============================================================================

void LockSetAnalysis::dumpLockGraph(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  file << "digraph LockGraph {\n";
  file << "  rankdir=LR;\n";
  file << "  node [shape=box];\n\n";

  // Create nodes for locks
  size_t id = 0;
  std::unordered_map<LockID, size_t> lock_ids;
  for (const auto *lock : m_all_locks) {
    lock_ids[lock] = id;
    file << "  lock" << id << " [label=\"";
    lock->printAsOperand(file, false);
    file << "\"];\n";
    id++;
  }

  file << "\n";

  // Create edges for lock ordering
  for (const auto &pair : m_observed_lock_orders) {
    auto it1 = lock_ids.find(pair.first);
    auto it2 = lock_ids.find(pair.second);
    if (it1 != lock_ids.end() && it2 != lock_ids.end()) {
      file << "  lock" << it1->second << " -> lock" << it2->second;

      // Highlight inversions in red
      LockPair reverse{pair.second, pair.first};
      if (m_observed_lock_orders.find(reverse) !=
          m_observed_lock_orders.end()) {
        file << " [color=red, style=bold]";
      }

      file << ";\n";
    }
  }

  file << "}\n";
  file.close();

  errs() << "Lock graph dumped to " << filename << "\n";
}
