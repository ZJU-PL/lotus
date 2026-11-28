#pragma once

#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace concurrency {

/// Infer-inspired summaries of events and the locks held at those events.
/// Lock releases are part of the summary so callers do not incorrectly retain
/// a mutex that a helper releases before blocking.
class StarvationChecker {
public:
  enum class EventKind { BlockingCall, Callback, LockAcquire };
  struct CriticalPair {
    EventKind kind = EventKind::BlockingCall;
    const llvm::Instruction *event = nullptr;
    std::string callee;
    std::map<mhp::LockID, const llvm::Instruction *> held;
    std::set<mhp::LockID> releasedBefore;
    std::vector<const llvm::Instruction *> trace;
    bool uiThread = false;
    bool lockless = false;
    bool nonblockingLock = false;
  };
  struct Summary {
    std::vector<CriticalPair> criticalPairs;
    std::map<mhp::LockID, const llvm::Instruction *> acquiredOnReturn;
    std::set<mhp::LockID> conditionallyAcquiredOnReturn;
    std::set<mhp::LockID> releasedOnReturn;
    std::set<mhp::LockID> mayReleasedOnReturn;
  };

  StarvationChecker(llvm::Module &module, mhp::LockSetAnalysis &locks);
  void addBlockingFunction(std::string name);
  std::vector<ConcurrencyBugReport> checkStarvation();
  const std::map<const llvm::Function *, Summary> &getSummaries() const {
    return summaries_;
  }

private:
  llvm::Module &module_;
  mhp::LockSetAnalysis &locks_;
  ThreadAPI *threadAPI_;
  std::set<std::string> blockingFunctions_;
  std::map<const llvm::Function *, Summary> summaries_;
  Summary analyze(const llvm::Function &function) const;
};

} // namespace concurrency
