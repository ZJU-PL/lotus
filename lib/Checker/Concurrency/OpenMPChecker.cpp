#include "Checker/Concurrency/OpenMPChecker.h"

#include "Concurrency/Core/ConcurrencyContext.h"

#include <deque>
#include <set>
#include <unordered_map>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace concurrency {

namespace {

bool isPrivateLike(OpenMP::DataSharingAttribute attribute) {
  switch (attribute) {
  case OpenMP::DataSharingAttribute::Private:
  case OpenMP::DataSharingAttribute::Firstprivate:
  case OpenMP::DataSharingAttribute::Lastprivate:
  case OpenMP::DataSharingAttribute::Linear:
    return true;
  default:
    return false;
  }
}

bool isSharedLike(OpenMP::DataSharingAttribute attribute) {
  switch (attribute) {
  case OpenMP::DataSharingAttribute::Shared:
  case OpenMP::DataSharingAttribute::SharedNoModify:
  case OpenMP::DataSharingAttribute::Copyin:
  case OpenMP::DataSharingAttribute::Copyout:
  case OpenMP::DataSharingAttribute::Reduction:
    return true;
  default:
    return false;
  }
}

struct NestingResult {
  std::vector<const Instruction *> unmatchedEnds;
  std::vector<const Instruction *> unclosedStarts;
  std::vector<std::pair<const Instruction *, const Instruction *>> nestedStarts;
};

// CFG-aware balanced-region dataflow over a function's basic blocks. Tracks
// (minDepth, maxDepth) along reachable CFG paths so balanced if/else branches
// never produce false positives.
template <typename StartPred, typename EndPred>
NestingResult analyzeCFGRegionNesting(const Function &func,
                                      StartPred isStart, EndPred isEnd) {
  NestingResult result;
  if (func.empty())
    return result;

  struct DepthState {
    int minDepth = 0;
    int maxDepth = 0;
    const Instruction *lastStart = nullptr;
    bool initialized = false;
  };

  DenseMap<const BasicBlock *, DepthState> inState, outState;
  std::deque<const BasicBlock *> worklist;
  DenseSet<const BasicBlock *> inQueue;

  const BasicBlock *entry = &func.getEntryBlock();
  inState[entry] = {0, 0, nullptr, true};
  worklist.push_back(entry);
  inQueue.insert(entry);

  DenseSet<const Instruction *> reportedEnds;
  DenseSet<const Instruction *> reportedNested;
  DenseSet<const Instruction *> reportedUnclosed;

  constexpr int kMaxDepthClamp = 16;

  while (!worklist.empty()) {
    const BasicBlock *bb = worklist.front();
    worklist.pop_front();
    inQueue.erase(bb);

    DepthState state = inState[bb];
    for (const Instruction &inst : *bb) {
      if (isStart(&inst)) {
        if (state.maxDepth > 0 && reportedNested.insert(&inst).second) {
          result.nestedStarts.emplace_back(state.lastStart, &inst);
        }
        state.minDepth = std::min(state.minDepth + 1, kMaxDepthClamp);
        state.maxDepth = std::min(state.maxDepth + 1, kMaxDepthClamp);
        state.lastStart = &inst;
      } else if (isEnd(&inst)) {
        if (state.minDepth == 0 && reportedEnds.insert(&inst).second) {
          result.unmatchedEnds.push_back(&inst);
        }
        state.minDepth = std::max(0, state.minDepth - 1);
        state.maxDepth = std::max(0, state.maxDepth - 1);
        if (state.maxDepth == 0) {
          state.lastStart = nullptr;
        }
      }
    }

    auto &prevOut = outState[bb];
    if (prevOut.initialized && prevOut.minDepth == state.minDepth &&
        prevOut.maxDepth == state.maxDepth) {
      continue;
    }
    prevOut = state;

    for (const BasicBlock *succ : successors(bb)) {
      auto &sIn = inState[succ];
      if (!sIn.initialized) {
        sIn = state;
        if (inQueue.insert(succ).second)
          worklist.push_back(succ);
      } else {
        int newMin = std::min(sIn.minDepth, state.minDepth);
        int newMax = std::max(sIn.maxDepth, state.maxDepth);
        if (newMin != sIn.minDepth || newMax != sIn.maxDepth) {
          sIn.minDepth = newMin;
          sIn.maxDepth = newMax;
          if (!sIn.lastStart)
            sIn.lastStart = state.lastStart;
          if (inQueue.insert(succ).second)
            worklist.push_back(succ);
        }
      }
    }
  }

  for (const BasicBlock &bb : func) {
    if (!isa<ReturnInst>(bb.getTerminator()))
      continue;
    auto it = outState.find(&bb);
    if (it != outState.end() && it->second.initialized &&
        it->second.maxDepth > 0 && it->second.lastStart &&
        reportedUnclosed.insert(it->second.lastStart).second) {
      result.unclosedStarts.push_back(it->second.lastStart);
    }
  }

  return result;
}

} // namespace

OpenMPChecker::OpenMPChecker(Module &module,
                             OpenMP::OpenMPTaskGraph *task_graph,
                             ThreadAPI *thread_api)
    : m_module(module), m_taskGraph(task_graph), m_threadAPI(thread_api) {}

void OpenMPChecker::ensureTaskGraph() {
  if (m_taskGraph) {
    return;
  }
  m_ownedTaskGraph = std::make_unique<OpenMP::OpenMPTaskGraph>(m_module);
  m_ownedTaskGraph->analyze();
  m_taskGraph = m_ownedTaskGraph.get();
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkPartialTaskSynchronization() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  for (const auto &boundary : m_taskGraph->getWaitBoundaries()) {
    if (!boundary.is_partial_wait) {
      continue;
    }

    ConcurrencyBugReport report(ConcurrencyBugType::OPENMP_PARTIAL_SYNC,
                                "OpenMP task wait with dependencies may leave "
                                "sibling tasks unsynchronized",
                                BugDescription::BI_MEDIUM,
                                BugDescription::BC_ERROR);
    report.addStep(boundary.inst,
                   "Selective __kmpc_omp_wait_deps boundary encountered here");
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkTaskgroupStructure() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    auto isStart = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      return cb && m_threadAPI->getType(cb) == ThreadAPI::TD_OMP_TASKGROUP_START;
    };
    auto isEnd = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      return cb && m_threadAPI->getType(cb) == ThreadAPI::TD_OMP_TASKGROUP_END;
    };

    NestingResult nesting = analyzeCFGRegionNesting(func, isStart, isEnd);
    for (const Instruction *endInst : nesting.unmatchedEnds) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
          "OpenMP taskgroup end has no matching start",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(
          endInst,
          "Encountered __kmpc_end_taskgroup without active taskgroup");
      reports.push_back(std::move(report));
    }
    for (const Instruction *startInst : nesting.unclosedStarts) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
          "OpenMP taskgroup start may not be properly closed",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(startInst, "Taskgroup starts here without matching "
                                "__kmpc_end_taskgroup on all exit paths");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkAtomicRegionStructure() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    auto isStart = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      return cb && m_threadAPI->getType(cb) == ThreadAPI::TD_OMP_ATOMIC_START;
    };
    auto isEnd = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      return cb && m_threadAPI->getType(cb) == ThreadAPI::TD_OMP_ATOMIC_END;
    };

    NestingResult nesting = analyzeCFGRegionNesting(func, isStart, isEnd);
    for (const Instruction *endInst : nesting.unmatchedEnds) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
          "OpenMP atomic end has no matching start", BugDescription::BI_HIGH,
          BugDescription::BC_ERROR);
      report.addStep(
          endInst,
          "Encountered __kmpc_atomic_end without active atomic region");
      reports.push_back(std::move(report));
    }
    for (const Instruction *startInst : nesting.unclosedStarts) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
          "OpenMP atomic region may not be properly closed",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(startInst, "Atomic region starts here without matching "
                                "__kmpc_atomic_end on all exit paths");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkDetachedTaskLeak() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  const auto &summary = m_taskGraph->getSummary();
  if (summary.detached_task_count != 0 &&
      summary.detached_task_count > summary.detach_completion_count) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::OPENMP_DETACHED_TASK_LEAK,
        "OpenMP detached task may not be properly completed",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(
        nullptr,
        "Detached tasks: " + std::to_string(summary.detached_task_count) +
            ", completions: " +
            std::to_string(summary.detach_completion_count));
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkNestedSingle() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    auto isStart = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      return cb && m_threadAPI->getType(cb) == ThreadAPI::TD_OMP_SINGLE_START;
    };
    auto isEnd = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      return cb && m_threadAPI->getType(cb) == ThreadAPI::TD_OMP_SINGLE_END;
    };

    NestingResult nesting = analyzeCFGRegionNesting(func, isStart, isEnd);
    for (const auto &[outerSingle, innerSingle] : nesting.nestedStarts) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_NESTED_SINGLE,
          "Nested OpenMP single regions detected without proper "
          "synchronization",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      if (outerSingle)
        report.addStep(outerSingle, "Outer single region");
      report.addStep(
          innerSingle,
          "Inner single region - may cause implicit barrier deadlock");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkNowaitMissingBarrier() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    // CFG-aware forward propagation of an unsynchronized worksharing completion
    // (workshare without a following barrier before a subsequent workshare or
    // explicit flush).
    auto isWorkshareEnd = [&](const Instruction *inst) -> bool {
      const auto *cb = dyn_cast<CallBase>(inst);
      const Function *callee = cb ? cb->getCalledFunction() : nullptr;
      if (!callee)
        return false;
      StringRef name = callee->getName();
      return name.contains("for_static_fini") ||
             name.contains("for_dispatch_fini") ||
             name.contains("__kmpc_end_single") ||
             name.contains("sections_fini");
    };
    auto isBarrier = [&](const Instruction *inst) -> bool {
      const auto *cb = dyn_cast<CallBase>(inst);
      if (!cb)
        return false;
      if (m_threadAPI->getType(cb) == ThreadAPI::TD_BAR_WAIT)
        return true;
      const Function *callee = cb->getCalledFunction();
      return callee && callee->getName().contains("barrier");
    };
    auto isWorkshareStart = [&](const Instruction *inst) -> bool {
      const auto *cb = dyn_cast<CallBase>(inst);
      if (!cb)
        return false;
      const auto ty = m_threadAPI->getType(cb);
      return ty == ThreadAPI::TD_OMP_SINGLE_START ||
             ty == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
             ty == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT ||
             ty == ThreadAPI::TD_OMP_SECTIONS_INIT;
    };

    DenseMap<const BasicBlock *, const Instruction *> inWorkshare, outWorkshare;
    std::deque<const BasicBlock *> worklist;
    DenseSet<const BasicBlock *> inQueue;
    const BasicBlock *entry = &func.getEntryBlock();
    inWorkshare[entry] = nullptr;
    worklist.push_back(entry);
    inQueue.insert(entry);
    DenseSet<const Instruction *> reported;

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop_front();
      inQueue.erase(bb);

      const Instruction *pending = inWorkshare[bb];
      for (const Instruction &inst : *bb) {
        if (isBarrier(&inst)) {
          pending = nullptr;
          continue;
        }
        if (pending && isWorkshareStart(&inst) &&
            reported.insert(pending).second) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_NOWAIT_MISSING_BARRIER,
              "Subsequent OpenMP worksharing region reached without an "
              "intervening barrier after a nowait workshare",
              BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
          report.addStep(pending, "Prior worksharing region ends without barrier");
          report.addStep(&inst, "Subsequent worksharing region starts here");
          reports.push_back(std::move(report));
          pending = nullptr;
          continue;
        }
        if (isWorkshareEnd(&inst)) {
          pending = &inst;
        }
      }

      if (outWorkshare.count(bb) && outWorkshare[bb] == pending)
        continue;
      outWorkshare[bb] = pending;
      for (const BasicBlock *succ : successors(bb)) {
        if (!pending)
          continue;
        if (!inWorkshare.count(succ) || inWorkshare[succ] == nullptr) {
          inWorkshare[succ] = pending;
          if (inQueue.insert(succ).second)
            worklist.push_back(succ);
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkMissingFlush() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  // OpenMP critical sections, locks, atomics, and barriers imply flush per the
  // OpenMP memory model. Instead of falsely flagging critical/lock regions,
  // detect non-atomic spin-wait loops inside OpenMP functions that poll a
  // shared variable without any flush, atomic, lock, or barrier inside the
  // loop.
  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    bool isOpenMPFunc = func.getName().contains(".omp_outlined");
    if (!isOpenMPFunc) {
      for (const Instruction &inst : instructions(func)) {
        if (const auto *cb = dyn_cast<CallBase>(&inst)) {
          if (m_threadAPI->getType(cb) != ThreadAPI::TD_DUMMY &&
              m_threadAPI->getRuntimeLibrary(cb) ==
                  ThreadAPI::RuntimeLibrary::OpenMP) {
            isOpenMPFunc = true;
            break;
          }
        }
      }
    }
    if (!isOpenMPFunc)
      continue;

    std::unique_ptr<DominatorTree> ownedDT;
    std::unique_ptr<LoopInfo> ownedLI;
    const LoopInfo *LI = nullptr;
    if (m_context) {
      LI = &m_context->getLoopInfo(func);
    } else {
      ownedDT = std::make_unique<DominatorTree>(const_cast<Function &>(func));
      ownedLI = std::make_unique<LoopInfo>(*ownedDT);
      LI = ownedLI.get();
    }

    for (const Loop *L : *LI) {
      bool hasFlushOrSync = false;
      bool hasSideEffectCallOrStore = false;
      const Instruction *nonAtomicSharedLoad = nullptr;

      for (const BasicBlock *bb : L->blocks()) {
        for (const Instruction &inst : *bb) {
          if (isa<FenceInst>(&inst) || isa<AtomicRMWInst>(&inst) ||
              isa<AtomicCmpXchgInst>(&inst)) {
            hasFlushOrSync = true;
          } else if (const auto *load = dyn_cast<LoadInst>(&inst)) {
            if (load->isAtomic()) {
              hasFlushOrSync = true;
            } else if (!load->isVolatile()) {
              const Value *ptr = load->getPointerOperand()->stripPointerCasts();
              if (isa<GlobalValue>(ptr) || isa<Argument>(ptr)) {
                // Check if this load feeds the loop exit branch condition.
                for (const User *u : load->users()) {
                  if (isa<ICmpInst>(u) || isa<BranchInst>(u)) {
                    nonAtomicSharedLoad = load;
                    break;
                  }
                }
              }
            }
          } else if (isa<StoreInst>(&inst)) {
            hasSideEffectCallOrStore = true;
          } else if (const auto *cb = dyn_cast<CallBase>(&inst)) {
            const auto ty = m_threadAPI->getType(cb);
            if (ty == ThreadAPI::TD_OMP_FLUSH || ty == ThreadAPI::TD_BAR_WAIT ||
                m_threadAPI->isTDAcquire(cb) || m_threadAPI->isTDRelease(cb)) {
              hasFlushOrSync = true;
            } else {
              hasSideEffectCallOrStore = true;
            }
          }
        }
      }

      // Pure spin-wait loop polling a non-atomic shared variable without flush.
      if (nonAtomicSharedLoad && !hasFlushOrSync && !hasSideEffectCallOrStore) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::OPENMP_MISSING_FLUSH,
            "OpenMP spin-wait loop reads shared flag without flush or atomic "
            "synchronization",
            BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
        report.addStep(nonAtomicSharedLoad,
                       "Non-atomic shared load polled in spin loop without "
                       "OpenMP flush");
        reports.push_back(std::move(report));
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkIncorrectNumThreads() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;
      StringRef name = callee->getName();

      if (name.contains("__kmpc_fork_call") && call->arg_size() >= 3) {
        const Value *num_threads_arg = call->getArgOperand(2);
        if (const auto *call_inst = dyn_cast<CallInst>(num_threads_arg)) {
          const Function *inner_callee = call_inst->getCalledFunction();
          if (inner_callee &&
              (inner_callee->getName().contains("omp_get_num_threads") ||
               inner_callee->getName().contains("omp_get_max_threads"))) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::OPENMP_INCORRECT_NUMTHREADS,
                "Incorrect use of omp_get_num_threads in num_threads clause",
                BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
            report.addStep(&inst,
                           "omp_get_num_threads called in num_threads clause");
            reports.push_back(std::move(report));
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkReductionError() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    auto isStart = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      const Function *callee = cb ? cb->getCalledFunction() : nullptr;
      return callee && callee->getName().contains("__kmpc_reduce") &&
             !callee->getName().contains("end");
    };
    auto isEnd = [&](const Instruction *inst) {
      const auto *cb = dyn_cast<CallBase>(inst);
      const Function *callee = cb ? cb->getCalledFunction() : nullptr;
      return callee && callee->getName().contains("__kmpc_end_reduce");
    };

    NestingResult nesting = analyzeCFGRegionNesting(func, isStart, isEnd);
    for (const Instruction *startInst : nesting.unclosedStarts) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_REDUCTION_ERROR,
          "OpenMP reduction region may be improperly closed",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      report.addStep(startInst, "Reduction start without matching end");
      reports.push_back(std::move(report));
    }
    for (const Instruction *endInst : nesting.unmatchedEnds) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_REDUCTION_ERROR,
          "OpenMP reduction end has no matching start",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      report.addStep(endInst, "Reduction end without matching start");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkTaskwaitMissing() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration() || func.empty())
      continue;

    // CFG-aware forward propagation of an unsynchronized spawned OpenMP task.
    DenseMap<const BasicBlock *, const Instruction *> inTask, outTask;
    std::deque<const BasicBlock *> worklist;
    DenseSet<const BasicBlock *> inQueue;
    const BasicBlock *entry = &func.getEntryBlock();
    inTask[entry] = nullptr;
    worklist.push_back(entry);
    inQueue.insert(entry);
    DenseSet<const Instruction *> reported;

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop_front();
      inQueue.erase(bb);

      const Instruction *pendingTask = inTask[bb];
      for (const Instruction &inst : *bb) {
        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          if (const Function *callee = call->getCalledFunction()) {
            StringRef name = callee->getName();
            if (name.contains("__kmpc_omp_task") && !name.contains("wait")) {
              pendingTask = &inst;
            } else if (name.contains("__kmpc_omp_taskwait") ||
                       name.contains("__kmpc_omp_wait_deps") ||
                       name.contains("__kmpc_end_taskgroup") ||
                       name.contains("__kmpc_barrier")) {
              pendingTask = nullptr;
            }
          }
        } else if (isa<ReturnInst>(&inst) && pendingTask &&
                   reported.insert(pendingTask).second) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OMP_TASKWAIT_MISSING,
              "OpenMP task created without taskwait before function returns",
              BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
          report.addStep(pendingTask,
                         "Task created here without taskwait before return");
          reports.push_back(std::move(report));
        }
      }

      if (outTask.count(bb) && outTask[bb] == pendingTask)
        continue;
      outTask[bb] = pendingTask;
      for (const BasicBlock *succ : successors(bb)) {
        if (!pendingTask)
          continue;
        if (!inTask.count(succ) || inTask[succ] == nullptr) {
          inTask[succ] = pendingTask;
          if (inQueue.insert(succ).second)
            worklist.push_back(succ);
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkNestedParallelDisabled() const {
  std::vector<ConcurrencyBugReport> reports;

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee || !callee->getName().contains("__kmpc_fork_call"))
        continue;

      for (const User *U : func.users()) {
        if (const auto *call_inst = dyn_cast<CallBase>(U)) {
          const Function *inner_callee = call_inst->getCalledFunction();
          if (inner_callee &&
              inner_callee->getName().contains("__kmpc_fork_call")) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::OMP_NESTED_PARALLEL_DISABLED,
                "Nested parallel region may not execute - "
                "OMP_MAX_ACTIVE_LEVELS might be 1",
                BugDescription::BI_LOW, BugDescription::BC_WARNING);
            report.addStep(&inst, "Nested parallel region detected");
            reports.push_back(std::move(report));
            break;
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkSharedPrivateConflict() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    const Instruction *copyprivate_inst = nullptr;
    const Instruction *task_inst = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      const ThreadAPI::TD_TYPE type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_TASK ||
          type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
          type == ThreadAPI::TD_OMP_TASKLOOP) {
        task_inst = &inst;
      }
      if (callee->getName().contains("copyprivate")) {
        copyprivate_inst = &inst;
      }
    }

    if (copyprivate_inst && task_inst) {
      bool semantic_conflict = true;
      if (m_taskGraph) {
        if (const OpenMP::Task *task =
                m_taskGraph->getTaskForCreate(task_inst)) {
          bool saw_private_like = false;
          bool saw_shared_like = false;
          for (const OpenMP::DataSharingEntry &entry :
               task->data_sharing_entries) {
            saw_private_like =
                saw_private_like || isPrivateLike(entry.attribute);
            saw_shared_like = saw_shared_like || isSharedLike(entry.attribute);
          }
          if (!task->data_sharing_entries.empty()) {
            semantic_conflict = saw_private_like && saw_shared_like;
          }
        }
      }
      if (!semantic_conflict)
        continue;

      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_SHARED_PRIVATE_CONFLICT,
          "OpenMP copyprivate state crosses tasking boundary; shared/private "
          "intent may be inconsistent",
          BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
      report.addStep(copyprivate_inst, "copyprivate-style operation here");
      report.addStep(task_inst, "tasking operation in same region");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkIfFalseParallel() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    const Instruction *set_num_threads_inst = nullptr;
    const Instruction *fork_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      if (callee->getName().contains("omp_set_num_threads") &&
          call->arg_size() >= 1) {
        if (const auto *ci = dyn_cast<ConstantInt>(call->getArgOperand(0))) {
          if (ci->getSExtValue() <= 1) {
            set_num_threads_inst = &inst;
          }
        }
      }

      if (m_threadAPI->getType(callee) == ThreadAPI::TD_FORK) {
        fork_inst = &inst;
      }
    }

    if (set_num_threads_inst && fork_inst) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_IF_FALSE_PARALLEL,
          "Parallel region may be serialized because thread count is forced to "
          "1",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      report.addStep(set_num_threads_inst,
                     "omp_set_num_threads forces a single worker");
      report.addStep(fork_inst, "parallel region launch occurs here");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkOrderedDependency() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    size_t ordered_count = 0;
    size_t doacross_wait_count = 0;
    size_t doacross_submit_count = 0;
    const Instruction *first_ordered = nullptr;
    const Instruction *first_wait = nullptr;
    const Instruction *first_submit = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_ORDERED_START) {
        ++ordered_count;
        if (!first_ordered)
          first_ordered = &inst;
      } else if (type == ThreadAPI::TD_OMP_DOACROSS_WAIT) {
        ++doacross_wait_count;
        if (!first_wait)
          first_wait = &inst;
      } else if (type == ThreadAPI::TD_OMP_DOACROSS_SUBMIT) {
        ++doacross_submit_count;
        if (!first_submit)
          first_submit = &inst;
      }
    }

    const bool missing_doacross = ordered_count > 0 &&
                                  doacross_wait_count == 0 &&
                                  doacross_submit_count == 0;
    const bool imbalance = doacross_wait_count != doacross_submit_count;
    if (missing_doacross || imbalance) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_ORDERED_DEPENDENCY,
          "OpenMP ordered dependency protocol appears incomplete",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      if (first_ordered)
        report.addStep(first_ordered, "ordered region starts here");
      if (missing_doacross) {
        report.addStep(nullptr,
                       "no doacross wait/submit operations were observed");
      }
      if (imbalance) {
        if (first_wait)
          report.addStep(first_wait, "doacross wait observed");
        if (first_submit)
          report.addStep(first_submit, "doacross submit observed");
      }
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkLastprivateMissing() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    bool saw_loop_init = false;
    bool saw_loop_fini = false;
    const Instruction *loop_init_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        saw_loop_init = true;
        if (!loop_init_inst)
          loop_init_inst = &inst;
      }
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
        saw_loop_fini = true;
      }
    }

    if (saw_loop_init && !saw_loop_fini && loop_init_inst) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_LASTPRIVATE_MISSING,
          "OpenMP worksharing loop init has no matching finalize; trailing "
          "lastprivate update may be skipped",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      report.addStep(loop_init_inst,
                     "loop worksharing initialization appears unmatched");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkCopyinNotShared() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    const Instruction *copyprivate_inst = nullptr;
    bool saw_single_region = false;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      if (m_threadAPI->getType(callee) == ThreadAPI::TD_OMP_SINGLE_START ||
          m_threadAPI->getType(callee) == ThreadAPI::TD_OMP_SINGLE_END) {
        saw_single_region = true;
      }
      if (callee->getName().contains("copyprivate")) {
        copyprivate_inst = &inst;
      }
    }

    if (copyprivate_inst && !saw_single_region) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_COPYIN_NOT_SHARED,
          "copyprivate/copyin-style operation appears outside single region",
          BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
      report.addStep(copyprivate_inst,
                     "copyprivate-like operation observed without single");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkBarrierInCritical() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration() || func.empty())
      continue;

    // CFG-aware active critical-section depth propagation.
    struct CritState {
      int maxDepth = 0;
      const Instruction *entry = nullptr;
      bool initialized = false;
    };
    DenseMap<const BasicBlock *, CritState> inState, outState;
    std::deque<const BasicBlock *> worklist;
    DenseSet<const BasicBlock *> inQueue;
    const BasicBlock *entry = &func.getEntryBlock();
    inState[entry] = {0, nullptr, true};
    worklist.push_back(entry);
    inQueue.insert(entry);
    DenseSet<const Instruction *> reportedBarriers;

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop_front();
      inQueue.erase(bb);

      CritState state = inState[bb];
      for (const Instruction &inst : *bb) {
        const auto *call = dyn_cast<CallBase>(&inst);
        const Function *callee = call ? call->getCalledFunction() : nullptr;
        if (!callee)
          continue;

        const auto type = m_threadAPI->getType(callee);
        if (type == ThreadAPI::TD_ACQUIRE &&
            m_threadAPI->semanticTagStartsWith(callee, "critical")) {
          ++state.maxDepth;
          if (!state.entry)
            state.entry = &inst;
        } else if (type == ThreadAPI::TD_RELEASE &&
                   m_threadAPI->semanticTagStartsWith(callee, "critical")) {
          if (state.maxDepth > 0) {
            --state.maxDepth;
            if (state.maxDepth == 0)
              state.entry = nullptr;
          }
        } else if (type == ThreadAPI::TD_BAR_WAIT && state.maxDepth > 0 &&
                   reportedBarriers.insert(&inst).second) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OMP_BARRIER_IN_CRITICAL,
              "Barrier encountered in active critical region",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          if (state.entry)
            report.addStep(state.entry, "critical region begins here");
          report.addStep(&inst, "barrier inside critical may deadlock");
          reports.push_back(std::move(report));
        }
      }

      auto &prevOut = outState[bb];
      if (prevOut.initialized && prevOut.maxDepth == state.maxDepth)
        continue;
      prevOut = state;

      for (const BasicBlock *succ : successors(bb)) {
        auto &sIn = inState[succ];
        if (!sIn.initialized || state.maxDepth > sIn.maxDepth) {
          sIn.initialized = true;
          sIn.maxDepth = std::max(sIn.maxDepth, state.maxDepth);
          if (!sIn.entry)
            sIn.entry = state.entry;
          if (inQueue.insert(succ).second)
            worklist.push_back(succ);
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkPrivateInLoop() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    bool in_loop_region = false;
    const Instruction *loop_enter = nullptr;
    const Instruction *task_call_in_loop = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT) {
        in_loop_region = true;
        if (!loop_enter)
          loop_enter = &inst;
        continue;
      }

      if (in_loop_region && (type == ThreadAPI::TD_OMP_TASK ||
                             type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
                             type == ThreadAPI::TD_OMP_TASKLOOP)) {
        task_call_in_loop = &inst;
      }

      if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
        in_loop_region = false;
      }
    }

    if (loop_enter && task_call_in_loop) {
      bool captures_loop_private = true;
      if (m_taskGraph) {
        if (const OpenMP::Task *task =
                m_taskGraph->getTaskForCreate(task_call_in_loop)) {
          if (!task->data_sharing_entries.empty()) {
            captures_loop_private = false;
            for (const OpenMP::DataSharingEntry &entry :
                 task->data_sharing_entries) {
              if (isPrivateLike(entry.attribute)) {
                captures_loop_private = true;
                break;
              }
            }
          }
        }
      }
      if (!captures_loop_private)
        continue;

      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_PRIVATE_IN_LOOP,
          "Tasking inside OpenMP loop may capture loop-private values "
          "unsafely",
          BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
      report.addStep(loop_enter, "OpenMP loop region starts here");
      report.addStep(task_call_in_loop, "tasking operation within loop region");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkMissingSchedule() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    bool has_static_loop = false;
    bool has_dynamic_loop = false;
    const Instruction *loop_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      const Function *callee = call->getCalledFunction();
      if (!callee)
        continue;

      const auto type = m_threadAPI->getType(callee);
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT) {
        has_static_loop = true;
        if (!loop_inst)
          loop_inst = &inst;
      }
      if (type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        has_dynamic_loop = true;
      }
    }

    if (has_static_loop && !has_dynamic_loop && loop_inst) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OMP_MISSING_SCHEDULE,
          "OpenMP loop appears to rely on implicit default scheduling",
          BugDescription::BI_LOW, BugDescription::BC_STYLE);
      report.addStep(loop_inst,
                     "static loop init observed without explicit dispatch "
                     "runtime path");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkOpenMPBugs() {
  ensureTaskGraph();

  std::vector<ConcurrencyBugReport> reports;

  auto append = [&](std::vector<ConcurrencyBugReport> more) {
    reports.insert(reports.end(), std::make_move_iterator(more.begin()),
                   std::make_move_iterator(more.end()));
  };

  append(checkPartialTaskSynchronization());
  append(checkTaskgroupStructure());
  append(checkAtomicRegionStructure());
  append(checkDetachedTaskLeak());
  append(checkNestedSingle());
  append(checkNowaitMissingBarrier());
  append(checkMissingFlush());
  append(checkIncorrectNumThreads());
  append(checkReductionError());
  append(checkTaskwaitMissing());
  append(checkNestedParallelDisabled());
  append(checkSharedPrivateConflict());
  append(checkIfFalseParallel());
  append(checkOrderedDependency());
  append(checkLastprivateMissing());
  append(checkCopyinNotShared());
  append(checkBarrierInCritical());
  append(checkPrivateInLoop());
  append(checkMissingSchedule());

  return reports;
}

} // namespace concurrency
