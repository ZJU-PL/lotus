#include "Checker/Concurrency/StarvationChecker.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <tuple>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>

namespace concurrency {
namespace {
using Held = std::map<mhp::LockID, const llvm::Instruction *>;
using Released = std::set<mhp::LockID>;
struct State {
  Held held;
  Released released;
  bool operator==(const State &s) const {
    return held == s.held && released == s.released;
  }
};
Released intersection(const Released &a, const Released &b) {
  Released result;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(result, result.end()));
  return result;
}
State join(const State &a, const State &b) {
  State result = a;
  result.held.insert(b.held.begin(), b.held.end());
  result.released = intersection(a.released, b.released);
  return result;
}
const llvm::Function *callee(const llvm::CallBase &call) {
  return llvm::dyn_cast<llvm::Function>(
      call.getCalledOperand()->stripPointerCasts());
}
bool pairEqual(const StarvationChecker::CriticalPair &a,
               const StarvationChecker::CriticalPair &b) {
  return a.kind == b.kind && a.event == b.event && a.held == b.held &&
         a.releasedBefore == b.releasedBefore && a.uiThread == b.uiThread &&
         a.lockless == b.lockless && a.nonblockingLock == b.nonblockingLock;
}
bool summaryEqual(const StarvationChecker::Summary &a,
                  const StarvationChecker::Summary &b) {
  return a.acquiredOnReturn == b.acquiredOnReturn &&
         a.releasedOnReturn == b.releasedOnReturn &&
         a.criticalPairs.size() == b.criticalPairs.size() &&
         std::equal(a.criticalPairs.begin(), a.criticalPairs.end(),
                    b.criticalPairs.begin(), pairEqual);
}
} // namespace

StarvationChecker::StarvationChecker(llvm::Module &module,
                                     mhp::LockSetAnalysis &locks)
    : module_(module), locks_(locks), threadAPI_(ThreadAPI::getThreadAPI()),
      blockingFunctions_{
          "sleep",    "usleep",       "nanosleep",  "read",         "recv",
          "recvfrom", "recvmsg",      "accept",     "accept4",      "poll",
          "select",   "pselect",      "epoll_wait", "pthread_join", "thrd_join",
          "sem_wait", "sem_timedwait"} {}

void StarvationChecker::addBlockingFunction(std::string name) {
  blockingFunctions_.insert(std::move(name));
}

StarvationChecker::Summary
StarvationChecker::analyze(const llvm::Function &function) const {
  Summary summary;
  std::map<const llvm::BasicBlock *, State> inputs, outputs;
  std::set<const llvm::BasicBlock *> queued, processed;
  std::deque<const llvm::BasicBlock *> worklist;
  inputs.emplace(&function.getEntryBlock(), State{});
  worklist.push_back(&function.getEntryBlock());
  queued.insert(&function.getEntryBlock());
  const bool ui = function.hasFnAttribute("lotus.ui-thread");
  const bool lockless = function.hasFnAttribute("lotus.lockless");

  auto canonical = [&](mhp::LockID lock) -> mhp::LockID {
    return lock ? locks_.getCanonicalLock(lock) : nullptr;
  };
  auto instantiate = [&](mhp::LockID lock,
                         const llvm::CallBase &call) -> mhp::LockID {
    return locks_.projectLockAtCall(&call, callee(call), lock);
  };
  auto release = [&](State &state, mhp::LockID lock) {
    lock = canonical(lock);
    if (!lock)
      return;
    for (auto it = state.held.begin(); it != state.held.end();) {
      if (locks_.locksMustMatch(it->first, lock))
        it = state.held.erase(it);
      else
        ++it;
    }
    state.released.insert(lock);
  };
  auto acquire = [&](State &state, mhp::LockID lock,
                     const llvm::Instruction *site) {
    lock = canonical(lock);
    if (!lock)
      return;
    state.released.erase(lock);
    state.held.emplace(lock, site);
  };
  auto emit = [&](CriticalPair pair) {
    auto existing =
        std::find_if(summary.criticalPairs.begin(), summary.criticalPairs.end(),
                     [&](const CriticalPair &p) {
                       return p.kind == pair.kind && p.event == pair.event;
                     });
    if (existing == summary.criticalPairs.end()) {
      summary.criticalPairs.push_back(std::move(pair));
    } else {
      existing->held.insert(pair.held.begin(), pair.held.end());
      existing->releasedBefore =
          intersection(existing->releasedBefore, pair.releasedBefore);
      existing->uiThread |= pair.uiThread;
      existing->lockless |= pair.lockless;
      existing->nonblockingLock &= pair.nonblockingLock;
      if (pair.trace.size() < existing->trace.size())
        existing->trace = std::move(pair.trace);
    }
  };

  while (!worklist.empty()) {
    auto *block = worklist.front();
    worklist.pop_front();
    queued.erase(block);
    State state = inputs.at(block);
    for (const auto &instruction : *block) {
      // Use the lockset service's success/failure refinement for conditional
      // acquisitions in this function. Inherited caller locks are tracked by
      // the summary substitution rather than a context-free lockset query.
      for (auto it = state.held.begin(); it != state.held.end();) {
        const auto *origin = it->second;
        if (origin && origin->getFunction() == &function &&
            threadAPI_->isConditionalLockAcquire(origin)) {
          const auto possible = locks_.getMayLockSetAt(&instruction);
          const bool canHold = std::any_of(
              possible.begin(), possible.end(), [&](mhp::LockID lock) {
                return locks_.locksMustMatch(lock, it->first);
              });
          if (!canHold) {
            it = state.held.erase(it);
            continue;
          }
        }
        ++it;
      }
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (!call)
        continue;
      const auto *target = callee(*call);
      auto makePair = [&](EventKind kind) {
        CriticalPair pair;
        pair.kind = kind;
        pair.event = call;
        pair.callee = target ? target->getName().str() : "indirect callback";
        pair.held = state.held;
        pair.releasedBefore = state.released;
        pair.uiThread = ui;
        pair.lockless = lockless;
        return pair;
      };
      if (threadAPI_->isTDAcquire(call)) {
        auto pair = makePair(EventKind::LockAcquire);
        pair.nonblockingLock = threadAPI_->isTryLock(call);
        emit(std::move(pair));
        // The lockset service handles RAII constructors, wrapper objects and
        // try-lock status. A try-lock can be held on a feasible success path.
        auto underlying =
            call->arg_empty()
                ? std::vector<mhp::LockID>{}
                : locks_.getUnderlyingRAIILocks(call, call->getArgOperand(0));
        if (!underlying.empty()) {
          for (const auto *lock : underlying)
            acquire(state, lock, call);
        } else {
          const auto *lock = locks_.getLockValue(call);
          if (const auto *wrapperLock = locks_.getCppWrapperLockValue(call))
            lock = wrapperLock;
          acquire(state, lock, call);
        }
        continue;
      }
      if (threadAPI_->isTDRelease(call)) {
        auto released = locks_.getRAIILocksReleasedAt(call, true);
        if (released.empty()) {
          const auto *lock = locks_.getLockValue(call);
          if (const auto *underlying = locks_.getCppWrapperLockValue(call))
            lock = underlying;
          release(state, lock);
        } else {
          for (const auto *lock : released)
            release(state, lock);
        }
        continue;
      }
      if (threadAPI_->isTDCondWait(call)) {
        auto pair = makePair(EventKind::BlockingCall);
        const auto *mutex = canonical(threadAPI_->getCondMutex(call));
        if (mutex) {
          for (auto it = pair.held.begin(); it != pair.held.end();) {
            if (locks_.locksMustMatch(it->first, mutex))
              it = pair.held.erase(it);
            else
              ++it;
          }
          pair.releasedBefore.insert(mutex);
        }
        emit(std::move(pair));
        continue; // The waited mutex is reacquired on return.
      }
      if (!target || target->hasFnAttribute("lotus.arbitrary-code")) {
        emit(makePair(EventKind::Callback));
      } else if (blockingFunctions_.count(target->getName().str()) ||
                 target->hasFnAttribute("lotus.may-block")) {
        emit(makePair(EventKind::BlockingCall));
      }
      auto found = summaries_.find(target);
      if (found != summaries_.end()) {
        for (const auto &effect : found->second.criticalPairs) {
          CriticalPair pair = effect;
          pair.held.clear();
          pair.releasedBefore = state.released;
          State inherited = state;
          for (const auto *lock : effect.releasedBefore) {
            const auto *actual = instantiate(lock, *call);
            release(inherited, actual);
            if (actual)
              pair.releasedBefore.insert(actual);
          }
          pair.held = inherited.held;
          for (const auto &lock : effect.held) {
            if (const auto *actual = instantiate(lock.first, *call))
              pair.held.emplace(actual, lock.second);
          }
          pair.uiThread |= ui;
          pair.lockless |= lockless;
          // Bound recursive trace growth; summary facts still reach a fixpoint.
          if (pair.trace.size() < 16)
            pair.trace.insert(pair.trace.begin(), call);
          emit(std::move(pair));
        }
        for (const auto *lock : found->second.releasedOnReturn)
          release(state, instantiate(lock, *call));
        for (const auto &lock : found->second.acquiredOnReturn)
          acquire(state, instantiate(lock.first, *call), lock.second);
      }
      if (target && target->doesNotReturn())
        break;
    }
    if (processed.count(block) && outputs[block] == state)
      continue;
    processed.insert(block);
    outputs[block] = state;
    const auto *branch =
        llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    for (unsigned index = 0; index < block->getTerminator()->getNumSuccessors();
         ++index) {
      if (branch && branch->isConditional()) {
        if (auto *constant =
                llvm::dyn_cast<llvm::ConstantInt>(branch->getCondition()))
          if (index != (constant->isZero() ? 1u : 0u))
            continue;
      }
      auto *successor = block->getTerminator()->getSuccessor(index);
      auto inserted = inputs.emplace(successor, state);
      State next =
          inserted.second ? state : join(inserted.first->second, state);
      bool changed = inserted.second || !(next == inserted.first->second);
      inserted.first->second = std::move(next);
      if ((changed || !processed.count(successor)) &&
          queued.insert(successor).second)
        worklist.push_back(successor);
    }
  }
  bool firstReturn = true;
  for (const auto &block : function) {
    if (!llvm::isa<llvm::ReturnInst>(block.getTerminator()) ||
        !processed.count(&block))
      continue;
    const auto &state = outputs.at(&block);
    summary.acquiredOnReturn.insert(state.held.begin(), state.held.end());
    summary.releasedOnReturn =
        firstReturn ? state.released
                    : intersection(summary.releasedOnReturn, state.released);
    firstReturn = false;
  }
  std::sort(summary.criticalPairs.begin(), summary.criticalPairs.end(),
            [](const CriticalPair &a, const CriticalPair &b) {
              return std::make_pair(a.event, a.kind) <
                     std::make_pair(b.event, b.kind);
            });
  return summary;
}

std::vector<ConcurrencyBugReport> StarvationChecker::checkStarvation() {
  summaries_.clear();
  for (const auto &function : module_)
    if (!function.isDeclaration())
      summaries_.emplace(&function, Summary{});
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    bool changed = false;
    auto next = summaries_;
    for (const auto &entry : summaries_) {
      auto summary = analyze(*entry.first);
      changed |= !summaryEqual(entry.second, summary);
      next[entry.first] = std::move(summary);
    }
    summaries_ = std::move(next);
    if (!changed)
      break;
  }
  std::vector<ConcurrencyBugReport> reports;
  std::set<std::tuple<EventKind, const llvm::Instruction *,
                      const llvm::Instruction *, std::string>>
      seen;
  for (const auto &entry : summaries_) {
    for (const auto &pair : entry.second.criticalPairs) {
      std::string reason;
      if (pair.kind == EventKind::LockAcquire && pair.lockless)
        reason = "Lock acquisition in a lockless context";
      else if (pair.kind == EventKind::LockAcquire && pair.uiThread &&
               !pair.nonblockingLock)
        reason = "Potentially blocking lock acquisition on the UI thread";
      else if (pair.kind == EventKind::BlockingCall && pair.uiThread)
        reason = "Blocking call on the UI thread";
      else if (pair.kind == EventKind::BlockingCall && !pair.held.empty())
        reason = "Blocking call while holding a lock";
      else if (pair.kind == EventKind::Callback && !pair.held.empty())
        reason = "Arbitrary callback while holding a lock";
      else
        continue;
      auto *acquisition =
          pair.held.empty() ? nullptr : pair.held.begin()->second;
      if (!seen.emplace(pair.kind, pair.event, acquisition, reason).second)
        continue;
      ConcurrencyBugReport report(
          ConcurrencyBugType::STARVATION, reason + ": " + pair.callee,
          BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE);
      if (acquisition)
        report.addStep(acquisition, "Lock acquired here");
      for (auto *call : pair.trace)
        report.addStep(call, "Call leading to " + pair.callee);
      report.addStep(pair.event, report.description);
      reports.push_back(std::move(report));
    }
  }
  return reports;
}
} // namespace concurrency
