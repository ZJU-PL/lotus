#include "LLVMUtils/Scheduler/PipelineScheduler.h"
#include "LLVMUtils/ThreadPool.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <chrono>

#define DEBUG_TYPE "PipelineScheduler"

using namespace llvm;

static cl::opt<int>
    TaskTimeout("scheduler-task-timeout",
                cl::desc("Timeout for avoiding deadlock (in seconds)"),
                cl::ValueOptional, cl::init(60), cl::ReallyHidden);

/// Determine if a function should be analyzed
static inline bool shouldAnalyzeFunction(const Function *Func) {
  return Func && !Func->isIntrinsic() && !Func->isDeclaration();
}

PipelineScheduler::PipelineScheduler(Module &M, CallGraph &CG,
                                     AnalysisType AT)
    : M(M), CG(CG), AType(AT), Prog("[Pipeline Scheduler]",
                                     ProgressBar::PBS_CharacterStyle),
      ClientContext(nullptr), TaskTimeout(::TaskTimeout.getValue()),
      EnableGC(true), GCBatchSize(100) {

  // Build mapping between functions and integers
  int FuncIndex = 0;
  for (auto &F : M) {
    if (shouldAnalyzeFunction(&F)) {
      Functions.push_back(&F);
      FunctionIndexMap[&F] = FuncIndex++;
    }
  }
  FunctionCalleeIndexVec.resize(FuncIndex);

  LLVM_DEBUG(dbgs() << "[PipelineScheduler] Total functions: " << FuncIndex
                    << "\n");

  // Count edges for dependency graph
  size_t NumEdges = 0;
  for (auto &F : M) {
    if (shouldAnalyzeFunction(&F)) {
      CallGraphNode *Node = CG[&F];
      if (Node) {
        NumEdges += Node->size();
      }
    }
  }

  // Initialize dependency tracking structures
  if (AType == AT_BottomUp) {
    computeBottomUpDeps(NumEdges);
  } else if (AType == AT_TopDown) {
    llvm_unreachable("Top-down analysis not yet implemented!");
  } else {
    // Local analysis - initialize to zeros
    int NumFuncs = Functions.size();
    Callees = new int[NumEdges];
    Callers = new int[NumEdges];
    FirstEdge = new int[NumFuncs];
    NextEdge = new int[NumEdges];
    OutDegree = new int[NumFuncs];
    InDegree = new int[NumFuncs];
    memset(OutDegree, 0, NumFuncs * sizeof(int));
    memset(InDegree, 0, NumFuncs * sizeof(int));
    memset(FirstEdge, -1, NumFuncs * sizeof(int));
  }
}

PipelineScheduler::~PipelineScheduler() {
  delete[] OutDegree;
  delete[] InDegree;
  delete[] Callees;
  delete[] Callers;
  delete[] FirstEdge;
  delete[] NextEdge;
}

void PipelineScheduler::finishTask(std::shared_ptr<Task> T) {
  LLVM_DEBUG(dbgs() << "[PipelineScheduler] Task " << T->toString()
                    << " finished\n");
  {
    std::unique_lock<std::mutex> Lock(this->FTVecMutex);
    FinishedTaskVec.push_back(T);
  }
  FTVecCond.notify_one();
}

void PipelineScheduler::run() {
  if (!TaskCallback) {
    errs() << "Error: TaskCallback not set! Call setTaskCallback() before "
              "run().\n";
    return;
  }

  llvm::outs() << "Starting pipeline scheduler...\n";

  // Schedule initial tasks
  for (const auto *F : Functions) {
    if (AType == AT_Local) {
      // Local analysis: schedule all functions immediately
      auto FTask = std::make_shared<FunctionTask>(F, TaskCallback, ClientContext);
      this->executeTask(FTask);
    } else if (AType == AT_BottomUp) {
      // Bottom-up: schedule leaf functions (no callees)
      int FIdx = FunctionIndexMap[F];
      if (OutDegree[FIdx] == 0) {
        auto FTask =
            std::make_shared<FunctionTask>(F, TaskCallback, ClientContext);
        this->executeTask(FTask);
      }
    } else {
      llvm_unreachable("Top-down analysis not yet implemented!");
    }
  }

  // Wait for tasks to complete and schedule new ones
  waitTask();
  ThreadPool::get()->wait();

  llvm::outs() << "\nPipeline scheduler completed!\n";
}

void PipelineScheduler::executeTask(std::shared_ptr<Task> T) {
  ThreadPool::get()->enqueue([T, this]() {
    T->run();
    this->finishTask(T);
  });
}

void PipelineScheduler::waitTask() {
  size_t NumUnfinishedTasks = Functions.size();
  size_t NumAllTasks = NumUnfinishedTasks;
  size_t NumGCTasks = 0;

  while (NumUnfinishedTasks || NumGCTasks) {
    LLVM_DEBUG(dbgs() << "[PipelineScheduler] Unfinished tasks: "
                      << NumUnfinishedTasks << "\n");

    std::shared_ptr<Task> T(nullptr);
    {
      std::unique_lock<std::mutex> Lock(this->FTVecMutex);

      // Wait for a finished task with timeout
      FTVecCond.wait_for(Lock, std::chrono::seconds(TaskTimeout * 2),
                         [this] { return !FinishedTaskVec.empty(); });

      if (FinishedTaskVec.empty()) {
        Prog.showProgress(1);
        errs() << "\nWarning: Timeout waiting for tasks!\n";
        break;
      }

      // Pop a task
      T = FinishedTaskVec.back();
      FinishedTaskVec.pop_back();
    }

    if (isa<GCTask>(T.get())) {
      NumGCTasks--;
    } else if (isa<FunctionTask>(T.get())) {
      int NumGCTasksAdded =
          postProcessFunctionTask(std::static_pointer_cast<FunctionTask>(T));
      NumGCTasks += NumGCTasksAdded;

      --NumUnfinishedTasks;
      if (NumAllTasks)
        Prog.showProgress((float)(NumAllTasks - NumUnfinishedTasks) /
                          (float)NumAllTasks);
      else
        Prog.showProgress(1);
    }
  }

  llvm::outs() << "\n";
}

int PipelineScheduler::postProcessFunctionTask(
    std::shared_ptr<FunctionTask> T) {
  auto *F = T->getFunction();
  int NumGCTasksAdded = 0;

  if (AType == AT_BottomUp) {
    // For bottom-up analysis, schedule callers whose callees are all done
    int FunctionIndex = FunctionIndexMap[F];

    // Check each caller of the finished function
    int Edge = FirstEdge[FunctionIndex];
    while (Edge != -1) {
      int CallerIndex = Callers[Edge];
      if (--(OutDegree[CallerIndex]) == 0) {
        // All callees done, schedule this caller
        auto FTask = std::make_shared<FunctionTask>(
            Functions[CallerIndex], TaskCallback, ClientContext);
        this->executeTask(FTask);
      }
      Edge = NextEdge[Edge];
    }

    // Memory management: release functions no longer needed
    if (EnableGC && GCCallback) {
      if (InDegree[FunctionIndex] == 0) {
        FunctionToRelease.insert(F);
      }

      auto &FunctionCallees = FunctionCalleeIndexVec[FunctionIndex];
      for (int CalleeIndex : FunctionCallees) {
        if (--(InDegree[CalleeIndex]) == 0) {
          FunctionToRelease.insert(Functions[CalleeIndex]);

          if (FunctionToRelease.size() >= GCBatchSize) {
            auto GTask = std::make_shared<GCTask>(FunctionToRelease, GCCallback,
                                                   ClientContext);
            this->executeTask(GTask);
            FunctionToRelease.clear();
            NumGCTasksAdded++;
          }
        }
      }
    }
  }
  // Local analysis: no post-processing needed

  return NumGCTasksAdded;
}

int PipelineScheduler::computeBottomUpDeps(size_t NumEdges) {
  // Build inverse call graph for bottom-up scheduling
  int NumFuncs = Functions.size();

  Callees = new int[NumEdges];
  Callers = new int[NumEdges];
  FirstEdge = new int[NumFuncs];
  NextEdge = new int[NumEdges];
  OutDegree = new int[NumFuncs];
  InDegree = new int[NumFuncs];

  memset(OutDegree, 0, NumFuncs * sizeof(int));
  memset(InDegree, 0, NumFuncs * sizeof(int));
  memset(FirstEdge, -1, NumFuncs * sizeof(int));
  memset(Callees, 0, NumEdges * sizeof(int));
  memset(Callers, 0, NumEdges * sizeof(int));
  memset(NextEdge, -1, NumEdges * sizeof(int));

  int EdgeIndex = 0;

  for (const auto *Caller : Functions) {
    if (!shouldAnalyzeFunction(Caller))
      continue;

    int CallerIndex = FunctionIndexMap[Caller];
    CallGraphNode *CallerNode = CG[Caller];

    if (!CallerNode)
      continue;

    auto &VisitedCallees = FunctionCalleeIndexVec[CallerIndex];

    // Iterate over callees
    for (auto &CallRecord : *CallerNode) {
      Function *Callee = CallRecord.second->getFunction();

      if (!shouldAnalyzeFunction(Callee))
        continue;

      // Skip recursive calls (back edges)
      if (Callee == Caller)
        continue;

      int CalleeIndex = FunctionIndexMap[Callee];

      LLVM_DEBUG(dbgs() << "[PipelineScheduler] Edge: " << CalleeIndex
                        << " -> " << CallerIndex << "\n");

      // Add edge: Callee -> Caller
      Callees[EdgeIndex] = CalleeIndex;
      Callers[EdgeIndex] = CallerIndex;

      // Link into adjacency list
      NextEdge[EdgeIndex] = FirstEdge[CalleeIndex];
      FirstEdge[CalleeIndex] = EdgeIndex;

      // Update degrees
      OutDegree[CallerIndex]++;

      if (!VisitedCallees.count(CalleeIndex)) {
        InDegree[CalleeIndex]++;
        VisitedCallees.insert(CalleeIndex);
      }

      EdgeIndex++;
    }
  }

  return EdgeIndex;
}

int PipelineScheduler::computeTopDownDeps(size_t NumEdges) {
  // TODO: Implement top-down dependency computation
  return 0;
}

bool PipelineScheduler::reachable(int F1, int F2) {
  std::vector<int> Stack;
  Stack.push_back(F2);
  std::set<int> Visited;

  while (!Stack.empty()) {
    int F = Stack.back();
    Stack.pop_back();

    if (Visited.count(F))
      continue;
    if (F == F1)
      return true;
    Visited.insert(F);

    int Edge = FirstEdge[F];
    while (Edge != -1) {
      int CallerIndex = Callers[Edge];
      if (!Visited.count(CallerIndex)) {
        Stack.push_back(CallerIndex);
      }
      Edge = NextEdge[Edge];
    }
  }
  return false;
}

bool PipelineScheduler::shouldScheduleFunction(const Function *F) {
  return shouldAnalyzeFunction(F);
}

void PipelineScheduler::dumpStatus() {
  std::unique_lock<std::mutex> Lock(this->FTVecMutex);
  llvm::outs() << "\n[PipelineScheduler Status]\n";
  llvm::outs() << "  Finished tasks in queue: " << FinishedTaskVec.size()
               << "\n";
  llvm::outs() << "  Functions to release: " << FunctionToRelease.size()
               << "\n";
}

