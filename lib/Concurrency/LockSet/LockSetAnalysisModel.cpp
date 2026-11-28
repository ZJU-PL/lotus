#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/LockSet/LockSetAnalysisSupport.h"

#include <algorithm>
#include <queue>
#include <set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace mhp;

void LockSetAnalysis::identifyLocks() {
  auto process_func = [this](Function &func) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;
      const ThreadAPI::LockSemantics lock_semantics =
          m_thread_api->describeLockSemantics(inst);

      if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
        continue;
      }

      std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst, true);
      for (LockID lock : raii_releases) {
        if (!lock) {
          continue;
        }
        m_all_locks.insert(lock);
        m_lock_releases[lock].push_back(inst);
      }
      if (!raii_releases.empty()) {
        continue;
      }

      const bool is_acquire = lock_semantics.is_acquire;
      const bool is_release = lock_semantics.is_release;

      if (is_acquire) {
        LockID lock = getLockValue(inst);
        if (!lock) {
          lock = getCppWrapperLockValue(inst);
        }
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_acquires[lock].push_back(inst);

          LockSet entry_locks = getMayLockSetAt(inst);
          for (const auto *held : entry_locks) {
            if (held == lock || mayAlias(held, lock)) {
              m_reentrant_locks.insert(getCanonicalLock(lock));
              break;
            }
          }

          if (m_thread_api->isTryLock(inst))
            m_lock_try_acquires[lock].push_back(inst);
        }
      } else if (is_release) {
        LockID lock = getLockValue(inst);
        if (!lock) {
          lock = getCppWrapperLockValue(inst);
        }
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_releases[lock].push_back(inst);
        }
      }
    }
  };

  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        process_func(func);
      }
    }
  } else if (m_single_function) {
    process_func(*m_single_function);
  }
}

void LockSetAnalysis::trackLockOrdering() {
  for (const auto &pair : m_may_locksets_entry) {
    const Instruction *inst = pair.first;
    const LockSet &locks_held = pair.second;
    auto exit_it = m_may_locksets_exit.find(inst);
    if (exit_it == m_may_locksets_exit.end()) {
      continue;
    }

    LockSet newly_acquired;
    std::set_difference(exit_it->second.begin(), exit_it->second.end(),
                        locks_held.begin(), locks_held.end(),
                        std::inserter(newly_acquired, newly_acquired.begin()));

    const LockSet held_read = getMayReadLockSetAt(inst);
    const LockSet held_write = getMayWriteLockSetAt(inst);
    LockSet newly_acquired_read;
    LockSet newly_acquired_write;
    auto read_exit_it = m_may_read_locks_exit.find(inst);
    if (read_exit_it != m_may_read_locks_exit.end()) {
      std::set_difference(
          read_exit_it->second.begin(), read_exit_it->second.end(),
          held_read.begin(), held_read.end(),
          std::inserter(newly_acquired_read, newly_acquired_read.begin()));
    }
    auto write_exit_it = m_may_write_locks_exit.find(inst);
    if (write_exit_it != m_may_write_locks_exit.end()) {
      std::set_difference(
          write_exit_it->second.begin(), write_exit_it->second.end(),
          held_write.begin(), held_write.end(),
          std::inserter(newly_acquired_write, newly_acquired_write.begin()));
    }
    const bool nonblocking = m_thread_api->isTryLock(inst);
    const bool summary_induced =
        isa<CallBase>(inst) && !m_thread_api->isTDAcquire(inst) &&
        !m_thread_api->isTDRelease(inst);
    const LockSet must_held = getMustLockSetAt(inst);
    const LockSet &order_sources = summary_induced ? must_held : locks_held;

    auto markIfReentrantAcquire = [&](LockID acquired_lock) {
      if (!acquired_lock) {
        return;
      }
      for (const auto *held_lock : order_sources) {
        if (held_lock == acquired_lock || mayAlias(held_lock, acquired_lock)) {
          m_reentrant_locks.insert(getCanonicalLock(acquired_lock));
          return;
        }
      }
    };

    if (isLockOperation(inst)) {
      if (LockID op_lock = getLockValue(inst)) {
        const auto *call = dyn_cast<CallBase>(inst);
        const ThreadAPI::TD_TYPE type =
            call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
        if (type != ThreadAPI::TD_SHARED_RDLOCK &&
            type != ThreadAPI::TD_SHARED_WRLOCK &&
            type != ThreadAPI::TD_SHARED_LOCK_CTOR &&
            type != ThreadAPI::TD_SHARED_UNLOCK) {
          markIfReentrantAcquire(op_lock);
        }
      }
    }

    for (LockID new_lock : newly_acquired) {
      markIfReentrantAcquire(new_lock);

      for (const auto *held_lock : order_sources) {
        if (held_lock != new_lock) {
          m_observed_lock_orders.insert({held_lock, new_lock});
          const bool held_only_shared = held_read.count(held_lock) != 0 &&
                                        held_write.count(held_lock) == 0;
          const bool request_only_shared =
              newly_acquired_read.count(new_lock) != 0 &&
              newly_acquired_write.count(new_lock) == 0;
          if (!nonblocking && !(held_only_shared && request_only_shared)) {
            m_deadlock_wait_orders.insert({held_lock, new_lock});
          }
        }
      }
    }
  }
}

bool LockSetAnalysis::mayAlias(LockID lock1, LockID lock2) const {
  const Module *module =
      m_module ? m_module
               : (m_single_function ? m_single_function->getParent() : nullptr);
  if (detail::areDisjointConstantOffsetPointers(lock1, lock2, module)) {
    return false;
  }

  auto getKnownDistinctObject = [](LockID lock) -> const Value * {
    if (!lock) {
      return nullptr;
    }
    lock = lock->stripPointerCasts();
    if (isa<LoadInst>(lock)) {
      return nullptr;
    }
    const Value *object = getUnderlyingObject(lock, 32);
    object = object ? object->stripPointerCasts() : lock;
    return isa<GlobalVariable>(object) || isa<AllocaInst>(object) ? object
                                                                  : nullptr;
  };
  const Value *object1 = getKnownDistinctObject(lock1);
  const Value *object2 = getKnownDistinctObject(lock2);
  if (object1 && object2 && object1 != object2) {
    return false;
  }

  lock1 = getCanonicalLock(lock1);
  lock2 = getCanonicalLock(lock2);
  if (lock1 == lock2)
    return true;

  if (m_alias_analysis && lock1 && lock2) {
    return m_alias_analysis->mayAlias(lock1, lock2);
  }

  return true;
}

LockID LockSetAnalysis::getCanonicalLock(LockID lock) const {
  if (!lock)
    return nullptr;

  auto cache_it = m_canonical_lock_cache.find(lock);
  if (cache_it != m_canonical_lock_cache.end()) {
    return cache_it->second;
  }

  const LockID orig_lock = lock;
  lock = lock->stripPointerCasts();

  auto cacheAndReturn = [&](LockID result) {
    m_canonical_lock_cache.try_emplace(orig_lock, result);
    if (lock != orig_lock) {
      m_canonical_lock_cache.try_emplace(lock, result);
    }
    return result;
  };

  if (const auto *LI = dyn_cast<LoadInst>(lock)) {
    if (m_alias_analysis) {
      std::vector<const Value *> pts;
      if (m_alias_analysis->getPointsToSet(lock, pts) && pts.size() == 1 &&
          pts.front()) {
        return cacheAndReturn(pts.front()->stripPointerCasts());
      }
    }
    return cacheAndReturn(LI);
  }

  if (const auto *GEP = dyn_cast<GetElementPtrInst>(lock)) {
    if (GEP->hasAllConstantIndices()) {
      const Module *module =
          m_module
              ? m_module
              : (m_single_function ? m_single_function->getParent() : nullptr);
      const Value *base = nullptr;
      int64_t offset = 0;
      uint64_t size = 0;
      if (module &&
          detail::getConstantOffsetPointerInfo(GEP, module, base, offset,
                                               size) &&
          base) {
        if (m_alias_analysis && !isa<GlobalVariable>(base) &&
            !isa<AllocaInst>(base)) {
          std::vector<const Value *> pts;
          if (m_alias_analysis->getPointsToSet(base, pts) && pts.size() == 1 &&
              pts.front()) {
            base = pts.front()->stripPointerCasts();
          }
        }
        auto rep_it = m_access_path_lock_reps.find({base, offset});
        if (rep_it != m_access_path_lock_reps.end()) {
          return cacheAndReturn(rep_it->second);
        }
        m_access_path_lock_reps.emplace(std::make_pair(base, offset), lock);
      }
      return cacheAndReturn(lock);
    }
    lock = GEP->getPointerOperand()->stripPointerCasts();
  } else if (const auto *CE = dyn_cast<GEPOperator>(lock)) {
    const Module *module =
        m_module
            ? m_module
            : (m_single_function ? m_single_function->getParent() : nullptr);
    const Value *base = nullptr;
    int64_t offset = 0;
    uint64_t size = 0;
    if (module &&
        detail::getConstantOffsetPointerInfo(CE, module, base, offset, size) &&
        base) {
      auto rep_it = m_access_path_lock_reps.find({base, offset});
      if (rep_it != m_access_path_lock_reps.end()) {
        return cacheAndReturn(rep_it->second);
      }
      m_access_path_lock_reps.emplace(std::make_pair(base, offset), lock);
      return cacheAndReturn(lock);
    }
    if (const Value *underlying = getUnderlyingObject(lock, 32)) {
      lock = underlying->stripPointerCasts();
    }
  } else if (const Value *base = getUnderlyingObject(lock, 32)) {
    lock = base->stripPointerCasts();
  }

  if (m_alias_analysis) {
    std::vector<const Value *> pts;
    if (m_alias_analysis->getPointsToSet(lock, pts) && pts.size() == 1 &&
        pts.front()) {
      return cacheAndReturn(pts.front()->stripPointerCasts());
    }
  }

  return cacheAndReturn(lock);
}

LockID LockSetAnalysis::getUnderlyingRAIILock(const Instruction *inst,
                                              const Value *lock_obj) const {
  std::vector<LockID> locks = getUnderlyingRAIILocks(inst, lock_obj);
  return locks.empty() ? nullptr : locks.front();
}

std::vector<LockID>
LockSetAnalysis::getUnderlyingRAIILocks(const Instruction *inst,
                                        const Value *lock_obj) const {
  std::vector<LockID> locks;
  if (!inst || !lock_obj) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  const Value *base = lock_obj->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(base, 32)) {
    base = underlying->stripPointerCasts();
  }

  auto lifetime_range = raii_it->second.equal_range(base);
  if (lifetime_range.first == lifetime_range.second) {
    return locks;
  }

  std::vector<const RAIILock::LockLifetime *> selected_lifetimes;
  for (auto lifetime_it = lifetime_range.first;
       lifetime_it != lifetime_range.second; ++lifetime_it) {
    const RAIILock::LockLifetime &lifetime = lifetime_it->second;
    if (std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                  inst) != lifetime.destructors.end()) {
      selected_lifetimes.push_back(&lifetime);
      continue;
    }

    if (!lifetime.constructor ||
        lifetime.constructor->getFunction() != inst->getFunction()) {
      continue;
    }
    if (RAIILock::RAIILockTracker::constructorCanReachWithoutReconstruction(
            lifetime.constructor, inst, lifetime.lockObject)) {
      selected_lifetimes.push_back(&lifetime);
    }
  }

  if (selected_lifetimes.empty()) {
    return locks;
  }

  for (const RAIILock::LockLifetime *lifetime : selected_lifetimes) {
    for (const Value *lock : lifetime->underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        if (std::find(locks.begin(), locks.end(), canonical) == locks.end()) {
          locks.push_back(canonical);
        }
      }
    }
  }
  return locks;
}

std::vector<LockID>
LockSetAnalysis::getRAIILocksReleasedAt(const Instruction *inst,
                                        bool include_maybe_owned) const {
  std::vector<LockID> locks;
  if (!inst) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                  inst) == lifetime.destructors.end()) {
      continue;
    }

    const CallBase *dtor = dyn_cast<CallBase>(inst);
    const Value *wrapper =
        dtor && dtor->arg_size() ? dtor->getArgOperand(0)->stripPointerCasts()
                                 : nullptr;
    const auto &ownership_facts = include_maybe_owned
                                      ? m_may_raii_ownership_entry
                                      : m_must_raii_ownership_entry;
    auto ownership_it = ownership_facts.find(inst);
    const bool owns_at_destruction =
        wrapper && ownership_it != ownership_facts.end() &&
        ownership_it->second.count(wrapper) != 0;
    if (!owns_at_destruction)
      continue;

    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  const CallBase *dtor = dyn_cast<CallBase>(inst);
  const Value *wrapper =
      dtor && dtor->arg_size() ? dtor->getArgOperand(0)->stripPointerCasts()
                               : nullptr;
  if (wrapper) {
    if (const Value *underlying = getUnderlyingObject(wrapper, 32))
      wrapper = underlying->stripPointerCasts();
  }
  const bool has_tracked_lifetime =
      wrapper && raii_it->second.find(wrapper) != raii_it->second.end();
  if (locks.empty() && !has_tracked_lifetime &&
      m_thread_api->cppWrapperDestructorReleases(inst)) {
    for (const Value *lock : m_thread_api->getAnalysisLockIdentities(inst)) {
      if (LockID canonical = getCanonicalLock(lock))
        locks.push_back(canonical);
    }
  }

  return locks;
}

RAIIWrapperSet
LockSetAnalysis::transferRAIIOwnership(const Instruction *inst,
                                       const RAIIWrapperSet &in,
                                       bool is_must) const {
  RAIIWrapperSet out = in;
  const auto *call = dyn_cast_or_null<CallBase>(inst);
  if (!call || call->arg_size() == 0) {
    return out;
  }

  auto wrapperAt = [&](unsigned index) -> const Value * {
    return index < call->arg_size()
               ? call->getArgOperand(index)->stripPointerCasts()
               : nullptr;
  };
  const Value *first = wrapperAt(0);
  const ThreadAPI::TD_TYPE type = m_thread_api->getType(call);

  switch (type) {
  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    out.insert(first);
    break;
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR: {
    const RAIILock::OwnershipKind ownership =
        RAIILock::RAIILockTracker::getOwnershipKind(call);
    const bool owns = ownership == RAIILock::OwnershipKind::Immediate ||
                      ownership == RAIILock::OwnershipKind::Adopt ||
                      (!is_must &&
                       (ownership == RAIILock::OwnershipKind::Try ||
                        ownership == RAIILock::OwnershipKind::Unknown));
    if (owns) {
      out.insert(first);
    } else {
      out.erase(first);
    }
    break;
  }
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
    out.insert(first);
    break;
  case ThreadAPI::TD_CPP_LOCK_TRY:
    if (!is_must) {
      out.insert(first);
    } else {
      out.erase(first);
    }
    break;
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
  case ThreadAPI::TD_CPP_LOCK_RELEASE:
  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
    out.erase(first);
    break;
  case ThreadAPI::TD_CPP_LOCK_MOVE_CTOR:
  case ThreadAPI::TD_CPP_LOCK_MOVE_ASSIGN: {
    const Value *second = wrapperAt(1);
    const bool source_owns = second && in.count(second) != 0;
    out.erase(first);
    if (source_owns) {
      out.insert(first);
    }
    if (second) {
      out.erase(second);
    }
    break;
  }
  case ThreadAPI::TD_CPP_LOCK_SWAP: {
    const Value *second = wrapperAt(1);
    const bool first_owns = first && in.count(first) != 0;
    const bool second_owns = second && in.count(second) != 0;
    out.erase(first);
    out.erase(second);
    if (second_owns) {
      out.insert(first);
    }
    if (first_owns) {
      out.insert(second);
    }
    break;
  }
  default:
    break;
  }
  return out;
}

std::vector<LockID>
LockSetAnalysis::getImpreciseRAIILocksEndingAt(const Instruction *inst) const {
  std::vector<LockID> locks;
  if (!inst) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (lifetime.hasPreciseLifetimeEnd) {
      continue;
    }
    const bool at_unknown_boundary = lifetime.impreciseLifetimeBoundary &&
                                     lifetime.impreciseLifetimeBoundary == inst;
    const bool at_function_exit_without_precise_lifetime =
        !lifetime.impreciseLifetimeBoundary &&
        (isa<ReturnInst>(inst) || isa<ResumeInst>(inst));
    if (!at_unknown_boundary && !at_function_exit_without_precise_lifetime) {
      continue;
    }
    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

std::vector<LockID>
LockSetAnalysis::getImpreciseRAIILocksInFunction(const Function *func) const {
  std::vector<LockID> locks;
  if (!func) {
    return locks;
  }

  auto raii_it = m_raii_locks.find(func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (lifetime.hasPreciseLifetimeEnd) {
      continue;
    }
    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

LockID LockSetAnalysis::getCppWrapperLockValue(const Instruction *inst) const {
  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return nullptr;
  }

  switch (m_thread_api->getType(call)) {
  case ThreadAPI::TD_SHARED_RDLOCK:
  case ThreadAPI::TD_SHARED_WRLOCK:
  case ThreadAPI::TD_SHARED_UNLOCK:
    if (call->arg_size() >= 1) {
      return getCanonicalLock(call->getArgOperand(0));
    }
    return nullptr;

  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR:
    if (call->arg_size() >= 1) {
      if (LockID tracked =
              getUnderlyingRAIILock(inst, call->getArgOperand(0))) {
        return tracked;
      }
    }
    if (call->arg_size() >= 2) {
      return getCanonicalLock(call->getArgOperand(1));
    }
    return nullptr;

  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
    if (call->arg_size() >= 1) {
      return getUnderlyingRAIILock(inst, call->getArgOperand(0));
    }
    return nullptr;

  default:
    return nullptr;
  }
}

bool LockSetAnalysis::isLockOperation(const Instruction *inst) const {
  if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return false;
  }
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    return true;
  }

  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return false;
  }

  switch (m_thread_api->getType(call)) {
  case ThreadAPI::TD_SHARED_RDLOCK:
  case ThreadAPI::TD_SHARED_WRLOCK:
  case ThreadAPI::TD_SHARED_UNLOCK:
  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
    return true;
  default:
    return false;
  }
}

LockID LockSetAnalysis::getLockValue(const Instruction *inst) const {
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    const auto *call = dyn_cast<CallBase>(inst);
    ThreadAPI::TD_TYPE type =
        call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
    switch (type) {
    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_SHARED_LOCK_CTOR:
    case ThreadAPI::TD_SHARED_LOCK_DTOR:
      return getCanonicalLock(m_thread_api->getLockVal(inst));
    default:
      if (const Value *identity = m_thread_api->getAnalysisLockIdentity(inst)) {
        return getCanonicalLock(identity);
      }
      return getCanonicalLock(m_thread_api->getLockVal(inst));
    }
  }
  return getCppWrapperLockValue(inst);
}
