#ifndef LLVMUTILS_SCHEDULER_PIPELINESCHEDULER_H
#define LLVMUTILS_SCHEDULER_PIPELINESCHEDULER_H

#include "LLVMUtils/Scheduler/Task.h"
#include "Support/ProgressBar.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

using namespace llvm;

/**
 * PipelineScheduler provides pipeline-style parallel execution of tasks
 * with dependency tracking based on the call graph.
 *
 * Key features:
 * - Bottom-up scheduling based on call graph dependencies
 * - Pipeline pattern: workers execute tasks → master schedules new tasks
 * - Memory management with automatic garbage collection
 * - Progress tracking and status dumping
 *
 * Usage:
 *   PipelineScheduler Scheduler(M, CG);
 *   Scheduler.setTaskCallback([](const Function *F) { ... analyze F ... });
 *   Scheduler.setGCCallback([](const Function *F) { ... release F ... });
 *   Scheduler.run();
 */
class PipelineScheduler {
public:
  /// Analysis type for scheduling strategy
  enum AnalysisType {
    AT_Local,    // Local analysis - all functions can run in parallel
    AT_BottomUp, // Bottom-up analysis - respect call graph dependencies
    AT_TopDown   // Top-down analysis - TODO: not yet implemented
  };

private:
  /// The module being analyzed
  Module &M;

  /// The call graph for dependency tracking
  CallGraph &CG;

  /// Analysis type determines scheduling strategy
  AnalysisType AType;

  /// Mapping between functions and integers
  /// @{
  std::vector<const Function *> Functions;
  std::map<const Function *, int> FunctionIndexMap;
  /// @}

  /// Dependency tracking utilities for task scheduling
  /// @{
  int *Callees;
  int *Callers;
  int *FirstEdge;
  int *NextEdge;
  int *OutDegree;
  /// @}

  /// Memory management utilities
  /// @{
  /// Recording the indegree of each function in the call graph
  int *InDegree;
  /// Recording the callees of each function
  std::vector<std::set<int>> FunctionCalleeIndexVec;
  /// Recording the functions to release memory
  std::set<const Function *> FunctionToRelease;
  /// @}

  /// The finished task vector - pipe between workers and master
  /// Workers push finished tasks, master pulls them to schedule new tasks
  /// @{
  std::vector<std::shared_ptr<Task>> FinishedTaskVec;
  std::mutex FTVecMutex;
  std::condition_variable FTVecCond;
  /// @}

  /// Progress bar for user feedback
  ProgressBar Prog;

  /// Callbacks for client-defined work
  /// @{
  std::function<void(const Function *)> TaskCallback;
  std::function<void(const Function *)> GCCallback;
  void *ClientContext; // Opaque context pointer for callbacks
  /// @}

  /// Configuration options
  /// @{
  int TaskTimeout;        // Timeout for task completion (seconds)
  bool EnableGC;          // Enable automatic garbage collection
  unsigned GCBatchSize;   // Number of functions to batch for GC
  /// @}

private:
  /// Execute a task by enqueueing it to the thread pool
  void executeTask(std::shared_ptr<Task> T);

  /// Called when a task is finished
  void finishTask(std::shared_ptr<Task> T);

  /// Post-process a FunctionTask after completion
  int postProcessFunctionTask(std::shared_ptr<FunctionTask> T);

  /// Wait for tasks and schedule new ones
  void waitTask();

  /// Compute function dependencies for bottom-up analysis
  int computeBottomUpDeps(size_t ExtraEdges);

  /// Compute function dependencies for top-down analysis
  int computeTopDownDeps(size_t ExtraEdges);

  /// Check if there is a dependency chain from F1 to F2
  bool reachable(int F1, int F2);

  /// Check if a function should be scheduled
  bool shouldScheduleFunction(const Function *F);

public:
  PipelineScheduler(Module &M, CallGraph &CG,
                    AnalysisType AT = AT_BottomUp);
  virtual ~PipelineScheduler();

  /// Set the task callback that will be invoked for each function
  void setTaskCallback(std::function<void(const Function *)> CB) {
    TaskCallback = CB;
  }

  /// Set the garbage collection callback for memory cleanup
  void setGCCallback(std::function<void(const Function *)> CB) {
    GCCallback = CB;
  }

  /// Set opaque context pointer available to callbacks
  void setClientContext(void *Ctx) { ClientContext = Ctx; }

  /// Enable/disable automatic garbage collection
  void setEnableGC(bool Enable) { EnableGC = Enable; }

  /// Set the batch size for garbage collection
  void setGCBatchSize(unsigned Size) { GCBatchSize = Size; }

  /// Set task timeout in seconds
  void setTaskTimeout(int Seconds) { TaskTimeout = Seconds; }

  /// Start scheduling tasks
  void run();

  /// Dump current status (can be called from signal handler)
  void dumpStatus();
};

#endif // LLVMUTILS_SCHEDULER_PIPELINESCHEDULER_H

