/** @file OpenMPChecker.h @brief OpenMP concurrency bug checker for shared-memory parallel programs. */
#pragma once

#include "Concurrency/OpenMP/OpenMPTaskGraph.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/API/ThreadAPI.h"

#include <memory>
#include <vector>

namespace concurrency {

class ConcurrencyContext;

class OpenMPChecker {
public:
  OpenMPChecker(llvm::Module &module, OpenMP::OpenMPTaskGraph *task_graph,
                ThreadAPI *thread_api);

  void setContext(ConcurrencyContext *context) { m_context = context; }

  std::vector<ConcurrencyBugReport> checkOpenMPBugs();

private:
  llvm::Module &m_module;
  OpenMP::OpenMPTaskGraph *m_taskGraph;
  std::unique_ptr<OpenMP::OpenMPTaskGraph> m_ownedTaskGraph;
  ThreadAPI *m_threadAPI;
  ConcurrencyContext *m_context = nullptr;

  void ensureTaskGraph();
  std::vector<ConcurrencyBugReport> checkPartialTaskSynchronization() const;
  std::vector<ConcurrencyBugReport> checkTaskgroupStructure() const;
  std::vector<ConcurrencyBugReport> checkAtomicRegionStructure() const;
  std::vector<ConcurrencyBugReport> checkDetachedTaskLeak() const;
  std::vector<ConcurrencyBugReport> checkNestedSingle() const;
  std::vector<ConcurrencyBugReport> checkNowaitMissingBarrier() const;
  std::vector<ConcurrencyBugReport> checkMissingFlush() const;
  std::vector<ConcurrencyBugReport> checkIncorrectNumThreads() const;
  std::vector<ConcurrencyBugReport> checkReductionError() const;
  std::vector<ConcurrencyBugReport> checkTaskwaitMissing() const;
  std::vector<ConcurrencyBugReport> checkNestedParallelDisabled() const;
  std::vector<ConcurrencyBugReport> checkSharedPrivateConflict() const;
  std::vector<ConcurrencyBugReport> checkIfFalseParallel() const;
  std::vector<ConcurrencyBugReport> checkOrderedDependency() const;
  std::vector<ConcurrencyBugReport> checkLastprivateMissing() const;
  std::vector<ConcurrencyBugReport> checkCopyinNotShared() const;
  std::vector<ConcurrencyBugReport> checkBarrierInCritical() const;
  std::vector<ConcurrencyBugReport> checkPrivateInLoop() const;
  std::vector<ConcurrencyBugReport> checkMissingSchedule() const;
};

} // namespace concurrency