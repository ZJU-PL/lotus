/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/LockMismatchChecker.h"

#include "Concurrency/Core/ConcurrencyContext.h"
#include "Concurrency/LockSet/RAIILockTracker.h"

#include <deque>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

namespace {

std::vector<const CallBase *> collectDirectCallSites(const Function &target) {
  std::vector<const CallBase *> sites;
  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(&target);
  visited.insert(&target);
  while (!worklist.empty()) {
    const Value *cur = worklist.front();
    worklist.pop_front();
    for (const Use &use : cur->uses()) {
      const User *user = use.getUser();
      if (const auto *cb = dyn_cast<CallBase>(user)) {
        if (cb->isCallee(&use) &&
            (cb->getCalledFunction() == &target ||
             (cb->getCalledOperand() &&
              cb->getCalledOperand()->stripPointerCastsAndAliases() ==
                  &target))) {
          sites.push_back(cb);
        }
      } else if (const auto *ce = dyn_cast<ConstantExpr>(user)) {
        if (ce->isCast() || ce->getOpcode() == Instruction::GetElementPtr) {
          if (visited.insert(ce).second) {
            worklist.push_back(ce);
          }
        }
      }
    }
  }
  return sites;
}

bool isRecursiveMutexType(const ThreadAPI *api, const Instruction *inst) {
  if (!api || !inst) {
    return false;
  }
  if (const auto *cb = dyn_cast<CallBase>(inst)) {
    if (const Function *callee = api->getCallee(cb)) {
      if (api->semanticTagStartsWith(callee, "recursive") ||
          callee->getName().contains_insensitive("recursive_mutex") ||
          callee->getName().contains_insensitive("recursive_timed_mutex")) {
        return true;
      }
    }
  }
  return false;
}

bool isParameterDerivedLock(const Value *lock, const Function &func) {
  if (!lock) {
    return false;
  }
  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  auto push = [&](const Value *v) {
    if (!v)
      return;
    v = v->stripPointerCasts();
    if (visited.insert(v).second)
      worklist.push_back(v);
  };
  push(lock);

  unsigned steps = 0;
  while (!worklist.empty() && ++steps <= 64) {
    const Value *cur = worklist.front();
    worklist.pop_front();
    if (const auto *arg = dyn_cast<Argument>(cur)) {
      if (arg->getParent() == &func)
        return true;
      continue;
    }
    const Value *base = getUnderlyingObject(cur);
    if (base && base != cur) {
      push(base);
    }
    if (const auto *load = dyn_cast<LoadInst>(cur)) {
      push(load->getPointerOperand());
      continue;
    }
    // Handle -O0 parameter-spill allocas (e.g. `store %this, %this.addr`).
    if (const auto *alloca = dyn_cast<AllocaInst>(cur)) {
      if (alloca->getFunction() == &func) {
        for (const User *u : alloca->users()) {
          if (const auto *st = dyn_cast<StoreInst>(u)) {
            if (st->getPointerOperand()->stripPointerCasts() == alloca) {
              push(st->getValueOperand());
            }
          }
        }
      }
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(cur)) {
      for (const Value *inc : phi->incoming_values())
        push(inc);
      continue;
    }
    if (const auto *sel = dyn_cast<SelectInst>(cur)) {
      push(sel->getTrueValue());
      push(sel->getFalseValue());
    }
  }
  return false;
}

} // namespace

LockMismatchChecker::LockMismatchChecker(Module &module,
                                         LockSetAnalysis *locksetAnalysis,
                                         ThreadAPI *threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> LockMismatchChecker::checkLockMisuse() {
  std::vector<ConcurrencyBugReport> reports;

  if (!m_locksetAnalysis)
    return reports;

  const bool moduleHasLocks = !m_locksetAnalysis->getAllLocks().empty();

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    unsigned totalAcq = 0, totalRel = 0;
    bool hasRAIICtor = false;
    std::unordered_set<LockID> locallyAcquiredLocks;
    std::unordered_set<LockID> locallyAcquiredByNonTryLock;
    std::unordered_set<LockID> locallyAcquiredParamLocks;

    for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E; ++J) {
      const Instruction *I = &*J;
      if (m_threadAPI->isTDAcquire(I)) {
        ++totalAcq;
        const Value *rawLock = m_threadAPI->getAnalysisLockIdentity(I);
        if (LockID l = m_locksetAnalysis->getLockValue(I)) {
          LockID canon = m_locksetAnalysis->getCanonicalLock(l);
          locallyAcquiredLocks.insert(canon);
          if (!m_threadAPI->isTryLock(I)) {
            locallyAcquiredByNonTryLock.insert(canon);
          }
          if ((rawLock && isParameterDerivedLock(rawLock, func)) ||
              isParameterDerivedLock(l, func)) {
            locallyAcquiredParamLocks.insert(canon);
          }
        }
      } else if (m_threadAPI->isTDRelease(I)) {
        ++totalRel;
      }
      if (!hasRAIICtor && RAIILock::RAIILockTracker::isRAIILockConstructor(I))
        hasRAIICtor = true;
    }

    if (totalAcq == 0 && totalRel == 0 && !hasRAIICtor && !moduleHasLocks) {
      continue;
    }

    bool directCallSitesCollected = false;
    std::vector<const CallBase *> directCallSites;
    auto getDirectCallSites = [&]() -> const std::vector<const CallBase *> & {
      if (!directCallSitesCollected) {
        directCallSites = collectDirectCallSites(func);
        directCallSitesCollected = true;
      }
      return directCallSites;
    };

    auto callerMayHoldLock = [&](LockID calleeLock) -> bool {
      struct CallerStep {
        const Function *fn;
        LockID lock;
        unsigned depth;
      };
      constexpr unsigned kMaxCallerDepth = 6;
      std::deque<CallerStep> worklist;
      std::unordered_set<const Function *> visitedFuncs;
      worklist.push_back({&func, calleeLock, 0});
      visitedFuncs.insert(&func);

      while (!worklist.empty()) {
        CallerStep cur = worklist.front();
        worklist.pop_front();
        if (cur.depth >= kMaxCallerDepth) {
          continue;
        }
        const std::vector<const CallBase *> sites =
            (cur.fn == &func) ? getDirectCallSites()
                              : collectDirectCallSites(*cur.fn);
        for (const CallBase *CB : sites) {
          LockID projected =
              m_locksetAnalysis->projectLockAtCall(CB, cur.fn, cur.lock);
          if (projected && m_locksetAnalysis->mayHoldLock(CB, projected)) {
            return true;
          }
          if (m_locksetAnalysis->mayHoldLock(CB, cur.lock)) {
            return true;
          }
          const Function *callerFn = CB->getFunction();
          if (callerFn && visitedFuncs.insert(callerFn).second) {
            LockID nextLock = projected ? projected : cur.lock;
            worklist.push_back({callerFn, nextLock, cur.depth + 1});
          }
        }
      }
      return false;
    };

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;

      if (m_threadAPI->isTDRelease(inst)) {
        LockID lock = m_locksetAnalysis->getLockValue(inst);
        if (!lock)
          lock = m_threadAPI->getAnalysisLockIdentity(inst);
        if (!lock)
          continue;
        lock = m_locksetAnalysis->getCanonicalLock(lock);
        if (!lock)
          continue;

        // Skip polymorphic/API unlock wrappers: a single unlock of a
        // parameter-derived lock with no local acquires in an address-taken or
        // externally visible helper with no direct call sites in the module.
        const bool isUnlockWrapper =
            totalAcq == 0 && totalRel == 1 &&
            isParameterDerivedLock(lock, func) &&
            (func.hasAddressTaken() ||
             (!func.hasLocalLinkage() && getDirectCallSites().empty()));

        // Report unlock-without-lock only when LockSetAnalysis proves the lock
        // is not even in the may-lockset here AND no caller holds the lock when
        // invoking this helper function.
        if (!isUnlockWrapper && !m_locksetAnalysis->mayHoldLock(inst, lock) &&
            !callerMayHoldLock(lock)) {
          ConcurrencyBugReport report(ConcurrencyBugType::LOCK_MISMATCH,
                                      "Unlock called without holding the lock",
                                      BugDescription::BI_HIGH,
                                      BugDescription::BC_ERROR);
          report.addStep(inst, "Unlock operation without matching held lock");
          reports.push_back(std::move(report));
        }
      } else if (m_threadAPI->isTDAcquire(inst)) {
        // Non-blocking try-locks and shared read-locks do not self-deadlock on
        // re-acquisition in the same way as exclusive non-recursive locks.
        if (m_threadAPI->isTryLock(inst) ||
            (m_threadAPI->isReadLockAcquire(inst) &&
             !m_threadAPI->isWriteLockAcquire(inst))) {
          continue;
        }
        if (isRecursiveMutexType(m_threadAPI, inst)) {
          continue;
        }

        LockID lock = m_locksetAnalysis->getLockValue(inst);
        if (!lock)
          lock = m_threadAPI->getAnalysisLockIdentity(inst);
        if (!lock)
          continue;
        lock = m_locksetAnalysis->getCanonicalLock(lock);
        if (!lock)
          continue;

        if (m_locksetAnalysis->mustHoldLock(inst, lock)) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::LOCK_MISMATCH,
              "Double lock: attempting to acquire a lock already held",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          report.addStep(inst, "Second acquisition of already-held lock");
          reports.push_back(std::move(report));
        }
      }
    }

    // Check for Lock Leaks at function exits (normal returns and exceptional
    // unwinds), including path-sensitive early-return leaks even after Clang
    // return-block merging.
    if (moduleHasLocks) {
      std::vector<const Instruction *> normalReturns;
      std::vector<const Instruction *> unwindExits;
      for (const BasicBlock &bb : func) {
        const Instruction *term = bb.getTerminator();
        if (!term)
          continue;
        if (isa<ReturnInst>(term)) {
          normalReturns.push_back(term);
        } else if (isa<ResumeInst>(term)) {
          unwindExits.push_back(term);
        } else if (const auto *cr = dyn_cast<CleanupReturnInst>(term)) {
          if (cr->unwindsToCaller())
            unwindExits.push_back(term);
        }
      }

      // A function is an intentional lock-acquire wrapper if it performs no
      // releases, has callers (direct or vtable/address-taken on a parameter
      // lock), and either unconditionally holds `lock` at all normal returns
      // OR conditionally acquires a parameter-derived lock (e.g.,
      // __gthread_mutex_lock / SmartMutex::lock).
      auto isIntentionalAcquireWrapper = [&](LockID lock) -> bool {
        if (totalRel > 0 || normalReturns.empty())
          return false;
        LockID canon = m_locksetAnalysis->getCanonicalLock(lock);
        bool paramLock = isParameterDerivedLock(lock, func);
        if (!paramLock && canon) {
          for (LockID pl : locallyAcquiredParamLocks) {
            if (m_locksetAnalysis->locksMustMatch(canon, pl)) {
              paramLock = true;
              break;
            }
          }
        }
        StringRef fn = func.getName();
        const bool lockNamed = fn.contains_insensitive("lock") ||
                               fn.contains_insensitive("acquire") ||
                               fn.contains_insensitive("mutex") ||
                               fn.contains_insensitive("enter") ||
                               fn.contains_insensitive("guard");
        if (paramLock &&
            (!getDirectCallSites().empty() || func.hasAddressTaken() ||
             func.hasStructRetAttr() || lockNamed)) {
          return true;
        }
        for (const Instruction *ret : normalReturns) {
          if (!m_locksetAnalysis->mustHoldLock(ret, lock))
            return false;
        }
        if (!getDirectCallSites().empty() || func.hasStructRetAttr())
          return true;
        return lockNamed;
      };

      // A function whose only acquisition of `lock` is a non-blocking try_lock
      // (with no releases and a status return value) intentionally returns with
      // the lock held on success and unlocked on failure.
      auto isIntentionalTryLockWrapper = [&](LockID lock) -> bool {
        if (totalRel > 0 || func.getReturnType()->isVoidTy())
          return false;
        LockID canon = m_locksetAnalysis->getCanonicalLock(lock);
        if (!canon)
          return false;
        for (LockID nonTry : locallyAcquiredByNonTryLock) {
          if (m_locksetAnalysis->locksMustMatch(canon, nonTry))
            return false;
        }
        return true;
      };

      auto isLocallyAcquired = [&](LockID lock) -> bool {
        LockID canon = m_locksetAnalysis->getCanonicalLock(lock);
        if (!canon)
          return false;
        for (LockID local : locallyAcquiredLocks) {
          if (m_locksetAnalysis->locksMustMatch(canon, local))
            return true;
        }
        return false;
      };

      for (const Instruction *term : normalReturns) {
        LockSet mustHeld = m_locksetAnalysis->getMustLockSetAt(term);
        LockSet mayHeld = m_locksetAnalysis->getMayLockSetAt(term);

        for (LockID lock : mayHeld) {
          if (!lock || !isLocallyAcquired(lock))
            continue;
          if (isIntentionalAcquireWrapper(lock) ||
              isIntentionalTryLockWrapper(lock))
            continue;

          if (mustHeld.count(lock) != 0) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::LOCK_MISMATCH,
                "Lock leak: function returns with lock held",
                BugDescription::BI_HIGH, BugDescription::BC_ERROR);
            report.addStep(term, "Function returns while definitely holding lock");
            reports.push_back(std::move(report));
            continue;
          }

          // Merged-return early-exit leak detection: if `lock` is in mayHeld
          // at a return block AND at least one incoming predecessor edge enters
          // the return block while definitely holding `lock`, that path leaked
          // the lock without unlocking.
          const BasicBlock *retBB = term->getParent();
          bool hasLeakingPredecessor = false;
          for (const BasicBlock *pred : predecessors(retBB)) {
            const Instruction *predTerm = pred ? pred->getTerminator() : nullptr;
            if (predTerm && m_locksetAnalysis->mustHoldLock(predTerm, lock)) {
              hasLeakingPredecessor = true;
              break;
            }
          }
          if (hasLeakingPredecessor) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::LOCK_MISMATCH,
                "Lock leak: function may return with lock held on an early-exit path",
                BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
            report.addStep(term, "Function return reachable with leaked lock");
            reports.push_back(std::move(report));
          }
        }
      }

      for (const Instruction *term : unwindExits) {
        LockSet mustHeld = m_locksetAnalysis->getMustLockSetAt(term);
        for (LockID lock : mustHeld) {
          if (!lock || !isLocallyAcquired(lock))
            continue;
          ConcurrencyBugReport report(
              ConcurrencyBugType::LOCK_MISMATCH,
              "Lock leak: function unwinds via exception with lock held",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          report.addStep(term, "Exceptional exit while holding lock");
          reports.push_back(std::move(report));
        }
      }
    }

    if (hasRAIICtor) {
      checkRAIILockMisuse(func, reports);
    }
  }

  return reports;
}

void LockMismatchChecker::checkRAIILockMisuse(
    Function &func, std::vector<ConcurrencyBugReport> &reports) {
  RAIILock::RAIILockTracker tracker;
  tracker.analyzeFunction(&func);

  const auto &lifetimes = tracker.getAllLockLifetimes();

  for (const auto &pair : lifetimes) {
    const auto &lifetime = pair.second;
    const Value *wrapperObj =
        lifetime.lockObject ? lifetime.lockObject->stripPointerCasts() : nullptr;

    if (lifetime.destructors.empty()) {
      // Skip RAII factory functions that construct a lock guard directly into
      // an sret/out parameter or return it by value to the caller (e.g. C++17
      // NRVO `qt_scoped_lock(m)`): the caller owns destruction.
      const bool returnedToCaller =
          wrapperObj && isParameterDerivedLock(wrapperObj, func);
      if (!returnedToCaller) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::LOCK_MISMATCH,
            "RAII lock may not be released: no destructor found",
            BugDescription::BI_HIGH, BugDescription::BC_WARNING);
        report.addStep(lifetime.constructor, "RAII lock constructor");
        reports.push_back(std::move(report));
      }
    }

    if (lifetime.constructor && lifetime.constructor->getCalledFunction()) {
      StringRef name = lifetime.constructor->getCalledFunction()->getName();
      if (name.contains("unique_lock")) {
        // Only flag unique_lock when THIS specific wrapper object has no manual
        // lock/unlock/release/move/swap AND is not passed to condition_variable
        // wait (which requires unique_lock).
        bool requiresUniqueLock =
            lifetime.ownership == RAIILock::OwnershipKind::Deferred ||
            lifetime.ownership == RAIILock::OwnershipKind::Try ||
            lifetime.ownership == RAIILock::OwnershipKind::Adopt;
        if (!requiresUniqueLock) {
          for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
               ++I) {
            const CallBase *call = dyn_cast<CallBase>(&*I);
            if (!call || call == lifetime.constructor)
              continue;
            bool usesThisWrapper = false;
            if (wrapperObj) {
              for (unsigned a = 0; a < call->arg_size(); ++a) {
                const Value *arg = call->getArgOperand(a);
                if (arg && arg->stripPointerCasts() == wrapperObj) {
                  usesThisWrapper = true;
                  break;
                }
              }
            }
            if (!usesThisWrapper)
              continue;

            if (m_threadAPI && m_threadAPI->isTDCondWait(call)) {
              requiresUniqueLock = true;
              break;
            }
            if (const Function *callee = call->getCalledFunction()) {
              StringRef callName = callee->getName();
              if (callName.contains("condition_variable") ||
                  (callName.contains("unique_lock") &&
                   (callName.contains("lock") || callName.contains("unlock") ||
                    callName.contains("release") || callName.contains("swap") ||
                    callName.contains("try_lock")))) {
                requiresUniqueLock = true;
                break;
              }
            }
          }
        }

        if (!requiresUniqueLock) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::LOCK_MISMATCH,
              "unique_lock used without manual lock/unlock or condition "
              "variable: consider using lock_guard",
              BugDescription::BI_LOW, BugDescription::BC_STYLE);
          report.addStep(lifetime.constructor, "unique_lock constructor");
          reports.push_back(std::move(report));
        }
      }
    }
  }
}

std::string
LockMismatchChecker::getInstructionLocation(const Instruction *inst) const {
  std::string location;
  raw_string_ostream os(location);
  if (const Function *func = inst->getFunction())
    os << func->getName();
  if (const BasicBlock *bb = inst->getParent())
    os << ":" << bb->getName();
  return os.str();
}

} // namespace concurrency
