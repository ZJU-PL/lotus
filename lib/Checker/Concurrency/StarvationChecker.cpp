#include "Checker/Concurrency/StarvationChecker.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <stack>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>

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
  llvm::DenseMap<const llvm::BasicBlock *, State> inputs, outputs;
  llvm::DenseSet<const llvm::BasicBlock *> queued, processed;
  std::deque<const llvm::BasicBlock *> worklist;
  inputs.try_emplace(&function.getEntryBlock(), State{});
  worklist.push_back(&function.getEntryBlock());
  queued.insert(&function.getEntryBlock());
  const bool ui = function.hasFnAttribute("lotus.ui-thread");
  const bool lockless = function.hasFnAttribute("lotus.lockless");

  llvm::StringSet<> blockingNames;
  for (const std::string &name : blockingFunctions_) {
    blockingNames.insert(name);
  }

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

  struct PairKey {
    EventKind kind;
    const llvm::Instruction *event;
    bool operator==(const PairKey &o) const {
      return kind == o.kind && event == o.event;
    }
  };
  struct PairKeyHash {
    size_t operator()(const PairKey &k) const {
      size_t h1 = std::hash<int>{}(static_cast<int>(k.kind));
      size_t h2 = std::hash<const void *>{}(k.event);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };
  std::unordered_map<PairKey, size_t, PairKeyHash> pairIndex;
  constexpr size_t kMaxCriticalPairsPerSummary = 1024;
  auto emit = [&](CriticalPair pair) {
    PairKey key{pair.kind, pair.event};
    auto it = pairIndex.find(key);
    if (it == pairIndex.end()) {
      if (summary.criticalPairs.size() >= kMaxCriticalPairsPerSummary) {
        return;
      }
      pairIndex.emplace(key, summary.criticalPairs.size());
      summary.criticalPairs.push_back(std::move(pair));
    } else {
      CriticalPair &existing = summary.criticalPairs[it->second];
      existing.held.insert(pair.held.begin(), pair.held.end());
      existing.releasedBefore =
          intersection(existing.releasedBefore, pair.releasedBefore);
      existing.uiThread |= pair.uiThread;
      existing.lockless |= pair.lockless;
      existing.nonblockingLock &= pair.nonblockingLock;
      if (pair.trace.size() < existing.trace.size())
        existing.trace = std::move(pair.trace);
    }
  };

  while (!worklist.empty()) {
    auto *block = worklist.front();
    worklist.pop_front();
    queued.erase(block);
    State state = inputs.find(block)->second;
    for (const auto &instruction : *block) {
      // Use the lockset service's success/failure refinement for conditional
      // acquisitions in this function. Inherited caller locks are tracked by
      // the summary substitution rather than a context-free lockset query.
      if (!state.held.empty()) {
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
      } else if (blockingNames.count(target->getName()) ||
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
      auto inserted = inputs.try_emplace(successor, state);
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
    const auto &state = outputs.find(&block)->second;
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

  // Pre-scan call graph and starvation triggers (locks, UI-thread, lockless).
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      callees, callers;
  std::unordered_set<const llvm::Function *> directLockFuncs;
  std::unordered_set<const llvm::Function *> restrictedAttrFuncs;
  for (const auto &function : module_) {
    if (function.isDeclaration())
      continue;
    if (function.hasFnAttribute("lotus.ui-thread") ||
        function.hasFnAttribute("lotus.lockless")) {
      restrictedAttrFuncs.insert(&function);
    }
    bool touchesLock = false;
    llvm::DenseSet<const llvm::Function *> seenCallees;
    for (const auto &inst : llvm::instructions(function)) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (!call)
        continue;
      if (threadAPI_->isTDAcquire(call) || threadAPI_->isTDRelease(call) ||
          threadAPI_->isTDCondWait(call)) {
        touchesLock = true;
      }
      if (const auto *target = callee(*call)) {
        if (!target->isDeclaration() && seenCallees.insert(target).second) {
          callees[&function].push_back(target);
          callers[target].push_back(&function);
        }
      }
    }
    if (touchesLock) {
      directLockFuncs.insert(&function);
    }
  }

  // Without any locks, UI-thread functions, or lockless functions, no
  // starvation violation can exist.
  if (directLockFuncs.empty() && restrictedAttrFuncs.empty()) {
    return {};
  }

  // Step A: functions that may acquire/hold a lock locally or via a callee.
  std::unordered_set<const llvm::Function *> mayHoldLockFuncs(
      directLockFuncs.begin(), directLockFuncs.end());
  std::deque<const llvm::Function *> revQueue(directLockFuncs.begin(),
                                              directLockFuncs.end());
  while (!revQueue.empty()) {
    const auto *cur = revQueue.front();
    revQueue.pop_front();
    auto it = callers.find(cur);
    if (it == callers.end())
      continue;
    for (const auto *caller : it->second) {
      if (mayHoldLockFuncs.insert(caller).second) {
        revQueue.push_back(caller);
      }
    }
  }

  // Step B: functions reachable from lock-holding, UI-thread, or lockless
  // functions (the only functions whose effects can contribute to a report).
  std::unordered_set<const llvm::Function *> relevantFuncs;
  std::deque<const llvm::Function *> fwdQueue;
  for (const auto *f : mayHoldLockFuncs) {
    if (relevantFuncs.insert(f).second)
      fwdQueue.push_back(f);
  }
  for (const auto *f : restrictedAttrFuncs) {
    if (relevantFuncs.insert(f).second)
      fwdQueue.push_back(f);
  }
  while (!fwdQueue.empty()) {
    const auto *cur = fwdQueue.front();
    fwdQueue.pop_front();
    auto it = callees.find(cur);
    if (it == callees.end())
      continue;
    for (const auto *target : it->second) {
      if (relevantFuncs.insert(target).second) {
        fwdQueue.push_back(target);
      }
    }
  }

  // Bottom-up post-order over relevant functions, followed by caller-driven
  // fixpoint propagation (avoids 32 full-module sweeps and copying summaries_).
  std::vector<const llvm::Function *> postOrder;
  postOrder.reserve(relevantFuncs.size());
  std::unordered_set<const llvm::Function *> visited;
  std::stack<std::pair<const llvm::Function *, bool>> dfsStack;
  for (const auto &function : module_) {
    if (function.isDeclaration() || !relevantFuncs.count(&function) ||
        visited.count(&function))
      continue;
    dfsStack.push({&function, false});
    while (!dfsStack.empty()) {
      auto [cur, done] = dfsStack.top();
      dfsStack.pop();
      if (done) {
        postOrder.push_back(cur);
        continue;
      }
      if (!visited.insert(cur).second)
        continue;
      dfsStack.push({cur, true});
      auto it = callees.find(cur);
      if (it == callees.end())
        continue;
      for (const auto *target : it->second) {
        if (relevantFuncs.count(target) && !visited.count(target)) {
          dfsStack.push({target, false});
        }
      }
    }
  }

  for (const auto *func : postOrder) {
    summaries_.emplace(func, Summary{});
  }

  std::deque<const llvm::Function *> worklist(postOrder.begin(),
                                              postOrder.end());
  std::unordered_set<const llvm::Function *> inWorklist(postOrder.begin(),
                                                        postOrder.end());
  std::unordered_map<const llvm::Function *, unsigned> evalCount;
  constexpr unsigned kMaxFunctionEvals = 32;

  while (!worklist.empty()) {
    const auto *func = worklist.front();
    worklist.pop_front();
    inWorklist.erase(func);
    if (++evalCount[func] > kMaxFunctionEvals)
      continue;

    Summary nextSummary = analyze(*func);
    auto &existing = summaries_[func];
    if (!summaryEqual(existing, nextSummary)) {
      existing = std::move(nextSummary);
      auto callersIt = callers.find(func);
      if (callersIt != callers.end()) {
        for (const auto *caller : callersIt->second) {
          if (relevantFuncs.count(caller) &&
              evalCount[caller] < kMaxFunctionEvals &&
              inWorklist.insert(caller).second) {
            worklist.push_back(caller);
          }
        }
      }
    }
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
