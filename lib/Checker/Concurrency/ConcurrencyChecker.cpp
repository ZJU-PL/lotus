// Author: rainoftime

#include "Checker/Concurrency/ConcurrencyChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Alias/UnificationBased/seadsa/DsaAnalysis.hh"
#include "Alias/UnificationBased/seadsa/InitializePasses.hh"
#include "Checker/Concurrency/ConcurrencyAnalysisDumper.h"
#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

ConcurrencyChecker::ConcurrencyChecker(Module &module)
    : m_module(module), m_aliasAnalysis(nullptr),
      m_threadAPI(ThreadAPI::getThreadAPI()), m_stats{} {

  // Register EVERY ConcurrencyBugType with BugReportMgr so no checker report
  // is ever silently dropped.
  BugReportMgr &mgr = BugReportMgr::get_instance();
  auto reg = [&](ConcurrencyBugType type, const std::string &name,
                 BugDescription::BugImportance imp,
                 BugDescription::BugClassification cls,
                 const std::string &desc) {
    m_bugTypeIds[type] = mgr.register_bug_type(name, imp, cls, desc);
  };

  // Core shared-memory concurrency
  reg(ConcurrencyBugType::STARVATION, "Starvation", BugDescription::BI_MEDIUM,
      BugDescription::BC_PERFORMANCE,
      "Blocking or arbitrary callbacks in restricted concurrency contexts");
  reg(ConcurrencyBugType::DATA_RACE, "Data Race", BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-362");
  reg(ConcurrencyBugType::DEADLOCK, "Deadlock", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Deadlock potential");
  reg(ConcurrencyBugType::ATOMICITY_VIOLATION, "Atomicity Violation",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "Non-atomic operation sequence");
  reg(ConcurrencyBugType::COND_VAR_MISUSE, "Condition Variable Misuse",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Improper condition variable usage");
  reg(ConcurrencyBugType::LOCK_MISMATCH, "Lock Mismatch",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Lock acquisition/release mismatch");

  // OpenMP (all 19 bug types)
  reg(ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
      "OpenMP Taskgroup Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Unbalanced OpenMP taskgroup region");
  reg(ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
      "OpenMP Atomic Region Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Unbalanced OpenMP atomic region");
  reg(ConcurrencyBugType::OPENMP_PARTIAL_SYNC,
      "OpenMP Partial Task Synchronization", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Selective task wait may leave sibling tasks unsynchronized");
  reg(ConcurrencyBugType::OPENMP_DETACHED_TASK_LEAK,
      "OpenMP Detached Task Leak", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Detached OpenMP task may never be completed");
  reg(ConcurrencyBugType::OPENMP_NESTED_SINGLE, "OpenMP Nested Single",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "Nested OpenMP single regions may deadlock or violate specification");
  reg(ConcurrencyBugType::OPENMP_NOWAIT_MISSING_BARRIER,
      "OpenMP Nowait Missing Barrier", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Worksharing nowait region lacks synchronization before subsequent use");
  reg(ConcurrencyBugType::OPENMP_REDUCTION_ERROR, "OpenMP Reduction Error",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "Unbalanced or improperly nested OpenMP reduction region");
  reg(ConcurrencyBugType::OPENMP_INCORRECT_NUMTHREADS,
      "OpenMP Incorrect num_threads", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Incorrect runtime query used in OpenMP num_threads clause");
  reg(ConcurrencyBugType::OPENMP_MISSING_FLUSH, "OpenMP Missing Flush",
      BugDescription::BI_MEDIUM, BugDescription::BC_WARNING,
      "Shared synchronization loop lacks OpenMP flush");
  reg(ConcurrencyBugType::OMP_TASKWAIT_MISSING, "OpenMP Missing Taskwait",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "OpenMP task created without synchronizing taskwait before return");
  reg(ConcurrencyBugType::OMP_NESTED_PARALLEL_DISABLED,
      "OpenMP Nested Parallel Disabled", BugDescription::BI_LOW,
      BugDescription::BC_WARNING,
      "Nested OpenMP parallel region may be serialized by runtime");
  reg(ConcurrencyBugType::OMP_SHARED_PRIVATE_CONFLICT,
      "OpenMP Shared/Private Conflict", BugDescription::BI_MEDIUM,
      BugDescription::BC_WARNING,
      "Conflicting OpenMP shared/private data-sharing semantics");
  reg(ConcurrencyBugType::OMP_IF_FALSE_PARALLEL,
      "OpenMP Serialized Parallel Region", BugDescription::BI_LOW,
      BugDescription::BC_WARNING,
      "OpenMP parallel region forced to execute on a single thread");
  reg(ConcurrencyBugType::OMP_ORDERED_DEPENDENCY,
      "OpenMP Ordered Dependency Protocol Error", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Incomplete or unbalanced OpenMP ordered/doacross dependency");
  reg(ConcurrencyBugType::OMP_LASTPRIVATE_MISSING,
      "OpenMP Lastprivate Finalize Missing", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "OpenMP worksharing loop init missing matching finalize");
  reg(ConcurrencyBugType::OMP_COPYIN_NOT_SHARED, "OpenMP Copyin/Copyprivate Misuse",
      BugDescription::BI_MEDIUM, BugDescription::BC_WARNING,
      "OpenMP copyprivate/copyin operation outside valid single region");
  reg(ConcurrencyBugType::OMP_BARRIER_IN_CRITICAL, "OpenMP Barrier in Critical",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "OpenMP barrier executed inside an active critical section");
  reg(ConcurrencyBugType::OMP_PRIVATE_IN_LOOP,
      "OpenMP Loop-Private Capture in Task", BugDescription::BI_MEDIUM,
      BugDescription::BC_WARNING,
      "OpenMP task created inside loop may unsafely capture loop-private state");
  reg(ConcurrencyBugType::OMP_MISSING_SCHEDULE,
      "OpenMP Implicit Default Schedule", BugDescription::BI_LOW,
      BugDescription::BC_STYLE,
      "OpenMP worksharing loop relies on implicit default scheduling");

  // MPI (all 28 bug types)
  reg(ConcurrencyBugType::MPI_ORPHANED_REQUEST, "MPI Orphaned Request",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Non-blocking MPI request without matching completion");
  reg(ConcurrencyBugType::MPI_DEADLOCK, "MPI Deadlock",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Potential blocking communication deadlock");
  reg(ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH, "MPI Collective Mismatch",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Incompatible collective operations across processes");
  reg(ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE,
      "MPI Conditional Collective", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Collective may be executed only by a subset of ranks");
  reg(ConcurrencyBugType::MPI_UNSYNC_RMA, "MPI Unsynchronized RMA",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "RMA operation without recognized synchronization");
  reg(ConcurrencyBugType::MPI_RMA_RACE, "MPI RMA Race",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Conflicting RMA operations without sufficient synchronization");
  reg(ConcurrencyBugType::MPI_WINDOW_LEAK, "MPI Window Leak",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "RMA window may not be freed");
  reg(ConcurrencyBugType::MPI_DOUBLE_FINALIZE, "MPI Double Finalize",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI_Finalize called more than once");
  reg(ConcurrencyBugType::MPI_MISSING_FINALIZE, "MPI Missing Finalize",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "MPI_Init called without matching MPI_Finalize");
  reg(ConcurrencyBugType::MPI_TAG_MISMATCH, "MPI Tag Mismatch",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI send and receive have mismatched tags");
  reg(ConcurrencyBugType::MPI_COUNT_DATATYPE_MISMATCH,
      "MPI Count/Datatype Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "MPI send and receive have incompatible count or datatype");
  reg(ConcurrencyBugType::MPI_RANK_OUT_OF_BOUNDS, "MPI Rank Out of Bounds",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI operation uses an out-of-bounds or negative rank");
  reg(ConcurrencyBugType::MPI_PERSISTENT_REQUEST_LEAK,
      "MPI Persistent Request Leak", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "MPI persistent request initialized without completion/free");
  reg(ConcurrencyBugType::MPI_WRONG_ROOT_RANK, "MPI Inconsistent Root Rank",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI collective operations disagree on root rank");
  reg(ConcurrencyBugType::MPI_CANCEL_WITHOUT_WAIT, "MPI Cancel Without Wait",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "MPI_Cancel called without subsequent wait/test");
  reg(ConcurrencyBugType::MPI_BUFFER_OVERLAP, "MPI Buffer Overlap",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI operation aliases send and receive buffers");
  reg(ConcurrencyBugType::MPI_WILDCARD_IN_COLLECTIVE,
      "MPI Wildcard in Collective", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "MPI collective uses disallowed wildcard rank");
  reg(ConcurrencyBugType::MPI_IN_PLACE_CONFLICT, "MPI_IN_PLACE Conflict",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "Incorrect MPI_IN_PLACE usage in collective");
  reg(ConcurrencyBugType::MPI_NULL_HANDLES, "MPI Null Handle",
      BugDescription::BI_MEDIUM, BugDescription::BC_WARNING,
      "MPI operation uses NULL handle");
  reg(ConcurrencyBugType::MPI_ROOT_NEGATIVE, "MPI Negative Root Rank",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI collective specifies a negative root rank");
  reg(ConcurrencyBugType::MPI_INVALID_TAG, "MPI Invalid Tag",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI operation uses invalid negative tag");
  reg(ConcurrencyBugType::MPI_INVALID_RANK, "MPI Invalid Rank",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI operation uses invalid rank");
  reg(ConcurrencyBugType::MPI_TYPE_SIZE_MISMATCH, "MPI Type Size Mismatch",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI send and receive payload byte sizes disagree");
  reg(ConcurrencyBugType::MPI_DESTROY_NULL_COMM, "MPI Destroy Null Communicator",
      BugDescription::BI_MEDIUM, BugDescription::BC_WARNING,
      "MPI_Comm_free called on null communicator");
  reg(ConcurrencyBugType::MPI_REQUEST_FREE_AFTER_WAIT,
      "MPI Request Free After Wait", BugDescription::BI_MEDIUM,
      BugDescription::BC_WARNING,
      "MPI_Request_free called after request completion");
  reg(ConcurrencyBugType::MPI_IN_PLACE_WRONG_OP,
      "MPI_IN_PLACE Unsupported Operand", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "MPI_IN_PLACE used on disallowed collective operand");
  reg(ConcurrencyBugType::MPI_INVALID_RMA_TRANSITION,
      "MPI Invalid RMA Epoch Transition", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "MPI RMA synchronization violates epoch transition rules");
  reg(ConcurrencyBugType::MPI_USE_AFTER_FREE_WINDOW,
      "MPI Window Use After Free", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "MPI RMA window used after MPI_Win_free");
  reg(ConcurrencyBugType::MPI_DOUBLE_WINDOW_FREE, "MPI Double Window Free",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "MPI_Win_free called repeatedly on the same window");

  // CUDA (all 11 bug types)
  reg(ConcurrencyBugType::CUDA_SHARED_MEMORY_RACE, "CUDA Shared-Memory Race",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Conflicting shared-memory accesses without sufficient synchronization");
  reg(ConcurrencyBugType::CUDA_GLOBAL_MEMORY_RACE, "CUDA Global-Memory Race",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Conflicting global/device-memory accesses without sufficient synchronization");
  reg(ConcurrencyBugType::CUDA_INTER_KERNEL_HAZARD, "CUDA Inter-Kernel Hazard",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Potential inter-kernel hazard across ordered or unordered CUDA launches");
  reg(ConcurrencyBugType::CUDA_BARRIER_MISMATCH, "CUDA Barrier Mismatch",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Not all threads reach the same CUDA barrier");
  reg(ConcurrencyBugType::CUDA_WARP_DIVERGENCE, "CUDA Warp Divergence",
      BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE,
      "Warp executes divergent control-flow paths");
  reg(ConcurrencyBugType::CUDA_BANK_CONFLICT, "CUDA Bank Conflict",
      BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE,
      "Shared-memory accesses may map multiple lanes to the same bank");
  reg(ConcurrencyBugType::CUDA_UNCOALESCED_ACCESS, "CUDA Uncoalesced Access",
      BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE,
      "Global-memory accesses may not coalesce");
  reg(ConcurrencyBugType::CUDA_VOLATILE_MISSING, "CUDA Missing Volatile",
      BugDescription::BI_LOW, BugDescription::BC_WARNING,
      "Potential missing volatile on inter-thread CUDA memory");
  reg(ConcurrencyBugType::CUDA_SYMBOLIC_CONFIG_RISK,
      "CUDA Symbolic Launch Configuration", BugDescription::BI_LOW,
      BugDescription::BC_WARNING,
      "Kernel launch uses symbolic or unknown thread/block sizing");
  reg(ConcurrencyBugType::CUDA_SHARED_GLOBAL_SPACE_MISMATCH,
      "CUDA Memory Space Ambiguity", BugDescription::BI_LOW,
      BugDescription::BC_WARNING,
      "Could not classify CUDA memory space precisely");
  reg(ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK,
      "CUDA Parametric Race Risk", BugDescription::BI_MEDIUM,
      BugDescription::BC_WARNING,
      "Parametric CUDA thread/block reasoning exposes a potential race");

  // Linux Kernel (all 13 bug types)
  reg(ConcurrencyBugType::LINUX_KERNEL_DEADLOCK, "Linux Kernel Deadlock",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Potential Linux kernel lock deadlock");
  reg(ConcurrencyBugType::LINUX_KERNEL_DOUBLE_LOCK, "Linux Kernel Double Lock",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Repeated acquisition of non-recursive kernel lock");
  reg(ConcurrencyBugType::LINUX_KERNEL_UNLOCK_WITHOUT_LOCK,
      "Linux Kernel Unlock Without Lock", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Kernel unlock without holding lock");
  reg(ConcurrencyBugType::LINUX_KERNEL_LOCK_WITHOUT_UNLOCK,
      "Linux Kernel Lock Leak", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "Kernel lock acquired without matching unlock");
  reg(ConcurrencyBugType::LINUX_KERNEL_LOCK_ORDER_INVERSION,
      "Linux Kernel Lock Order Inversion", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Inconsistent kernel lock acquisition order");
  reg(ConcurrencyBugType::LINUX_KERNEL_SLEEP_IN_ATOMIC,
      "Linux Kernel Sleep in Atomic Context", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Sleeping call inside spinlock/atomic context");
  reg(ConcurrencyBugType::LINUX_KERNEL_MIX_RAW_COOKED,
      "Linux Kernel Raw/Cooked Lock Mix", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Raw spinlock mixed with standard spinlock/sleepable lock");
  reg(ConcurrencyBugType::LINUX_KERNEL_IRQ_SAVE_RESTORE,
      "Linux Kernel IRQ Save/Restore Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Mismatched kernel IRQ save/restore");
  reg(ConcurrencyBugType::LINUX_KERNEL_DATA_RACE, "Linux Kernel Data Race",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Potential Linux kernel memory-model data race");
  reg(ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR, "Linux Kernel RCU Error",
      BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "RCU synchronization or reclamation misuse");
  reg(ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR,
      "Linux Kernel Wait/Completion Error", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "Wait-queue or completion misuse");
  reg(ConcurrencyBugType::LINUX_KERNEL_TIMER_ERROR, "Linux Kernel Timer Error",
      BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
      "Kernel timer lifecycle error");
  reg(ConcurrencyBugType::LINUX_KERNEL_LIFETIME_ERROR,
      "Linux Kernel Lifetime Hazard", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Concurrent use-after-free or async work lifetime hazard");

  m_stats = Statistics{};
  for (Function &func : module) {
    if (!func.isDeclaration()) {
      for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I)
        m_stats.totalInstructions++;
    }
  }
}

int ConcurrencyChecker::lookupBugTypeId(ConcurrencyBugType type) const {
  auto it = m_bugTypeIds.find(type);
  return it != m_bugTypeIds.end() ? it->second : -1;
}

void ConcurrencyChecker::runAnalyses() {
  // The previous checker may retain a non-owning pointer to the sparse solver.
  m_dataRaceChecker.reset();
  m_deadlockChecker.reset();
  m_atomicityChecker.reset();
  m_condVarChecker.reset();
  m_lockMismatchChecker.reset();
  m_openMPChecker.reset();
  m_mpiChecker.reset();
  m_cudaChecker.reset();
  m_linuxKernelChecker.reset();
  m_starvationChecker.reset();
  m_mhpAnalysis = nullptr;
  m_mhpAnalysisStorage.reset();
  m_locksetAnalysis.reset();
  m_locksetAnalysisView = nullptr;
  m_escapeAnalysis.reset();
  m_threadLocalAnalysis.reset();
  m_happensBeforeAnalysis.reset();
  m_staticThreadSharingPM.reset();
  m_staticThreadSharingAnalysis = nullptr;
  m_openMPTaskGraph.reset();
  m_mpiAnalysis.reset();
  m_cudaAnalysis.reset();
  m_linuxKernelAnalysis.reset();
  m_sparseRefinement.reset();
  m_context.reset();

  const size_t totalInsts = m_stats.totalInstructions;
  m_stats = Statistics{};
  m_stats.totalInstructions = totalInsts;

  m_context = std::make_unique<ConcurrencyContext>(m_module, m_aliasAnalysis);
  const ModuleConcurrencySummary &moduleSummary = m_context->getSummary();

  // Config-driven activation: run only analyses required by enabled checks
  // (Goblint-style)
  bool needMHP = m_checkDataRaces || m_checkDeadlocks ||
                 m_checkAtomicityViolations || m_checkCondVars ||
                 (m_checkLinuxKernel && moduleSummary.has_linux_kernel);
  bool needLockSet = m_checkDataRaces || m_checkDeadlocks ||
                     m_checkAtomicityViolations || m_checkCondVars ||
                     m_checkLockMismatches || m_checkStarvation;
  bool needEscape = m_checkDataRaces;
  bool needThreadLocal = m_checkDataRaces;
  bool needStaticSharing = m_checkDataRaces;
  bool needHappensBefore =
      m_checkDataRaces || m_checkDeadlocks ||
      (m_checkLinuxKernel && moduleSummary.has_linux_kernel);
  bool needOpenMP = m_checkOpenMP;
  bool needMPI = m_checkMPI;
  bool needCUDA = m_checkCUDA;
  bool needLinuxKernel = m_checkLinuxKernel && moduleSummary.has_linux_kernel;

  if (needMHP) {
    if (m_mhpBackend == MHPBackendKind::StaticVectorClock) {
      auto svc = std::make_unique<StaticVectorClockMHP>(*m_context);
      if (needLockSet) {
        svc->enableLockSetAnalysis();
      }
      svc->analyze();
      m_stats.mhpPairs = svc->getMhpPairCount();
      m_mhpAnalysis = svc.get();
      m_mhpAnalysisStorage = std::move(svc);
    } else {
      auto mhp = std::make_unique<MHPAnalysis>(*m_context);
      if (needLockSet) {
        mhp->enableLockSetAnalysis();
      }
      mhp->analyze();
      m_stats.mhpPairs = mhp->getMhpPairCount();
      m_mhpAnalysis = mhp.get();
      m_mhpAnalysisStorage = std::move(mhp);
    }
  }

  auto *regionMHP = dynamic_cast<MHPAnalysis *>(m_mhpAnalysis);
  auto *svcMHP = dynamic_cast<StaticVectorClockMHP *>(m_mhpAnalysis);

  // Prefer reusing the lockset computed inside MHPAnalysis/SVC-MHP.
  m_locksetAnalysisView = nullptr;
  if (needMHP && needLockSet && regionMHP && regionMHP->getLockSetAnalysis()) {
    m_locksetAnalysisView = regionMHP->getLockSetAnalysis();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getAllLocks().size();
  } else if (needMHP && needLockSet && svcMHP && svcMHP->getLockSetAnalysis()) {
    m_locksetAnalysisView = svcMHP->getLockSetAnalysis();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getAllLocks().size();
  } else if (needLockSet) {
    m_locksetAnalysis = std::make_unique<LockSetAnalysis>(*m_context);
    if (m_aliasAnalysis)
      m_locksetAnalysis->setAliasAnalysis(m_aliasAnalysis);
    m_locksetAnalysis->analyze();
    m_locksetAnalysisView = m_locksetAnalysis.get();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getAllLocks().size();
  }

  if (needEscape) {
    m_escapeAnalysis = std::make_unique<EscapeAnalysis>(m_module);
    m_escapeAnalysis->analyze();
  }

  if (needThreadLocal) {
    m_threadLocalAnalysis =
        std::make_unique<ThreadLocal::ThreadLocalAnalysis>(m_module);
    m_threadLocalAnalysis->analyze();
  }

  if (needStaticSharing) {
    auto canStaticSharingProveThreadLocal = [&]() -> bool {
      std::vector<const Function *> threadEntries;
      if (Function *mainFn = m_module.getFunction("main")) {
        if (!mainFn->isDeclaration()) {
          threadEntries.push_back(mainFn);
        }
      }
      bool hasFork = false;
      for (const Function &F : m_module) {
        if (F.isDeclaration()) {
          continue;
        }
        for (const Instruction &I : instructions(F)) {
          if (!m_threadAPI->isTDFork(&I)) {
            continue;
          }
          hasFork = true;
          const Value *entry = m_threadAPI->getForkedFun(&I);
          if (!entry) {
            return false;
          }
          const Function *entryFn =
              dyn_cast<Function>(entry->stripPointerCasts());
          if (!entryFn) {
            return false;
          }
          if (!entryFn->isDeclaration()) {
            threadEntries.push_back(entryFn);
          }
        }
      }

      std::unordered_set<const Function *> visited;
      std::vector<const Function *> stack(threadEntries.begin(),
                                          threadEntries.end());
      while (!stack.empty()) {
        const Function *cur = stack.back();
        stack.pop_back();
        if (!cur || cur->isDeclaration() || !visited.insert(cur).second) {
          continue;
        }
        for (const Instruction &I : instructions(*cur)) {
          const auto *cb = dyn_cast<CallBase>(&I);
          if (!cb) {
            continue;
          }
          const Value *called = cb->getCalledOperand();
          const Function *direct =
              cb->getCalledFunction()
                  ? cb->getCalledFunction()
                  : dyn_cast_or_null<Function>(
                        called ? called->stripPointerCasts() : nullptr);
          if (!direct) {
            return false;
          }
          if (!direct->isDeclaration() && !visited.count(direct)) {
            stack.push_back(direct);
          }
        }
      }

      return hasFork;
    };

    if (canStaticSharingProveThreadLocal()) {
      static bool passes_initialized = false;
      if (!passes_initialized) {
        llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();
        seadsa::initializeAnalysisPasses(registry);
        llvm::initializeDsaAnalysisPass(registry);
        passes_initialized = true;
      }

      m_staticThreadSharingPM = std::make_unique<llvm::legacy::PassManager>();
      m_staticThreadSharingPM->add(new seadsa::DsaAnalysis());
      auto *sharing = new lotus::StaticThreadSharingAnalysis();
      m_staticThreadSharingAnalysis = sharing;
      m_staticThreadSharingPM->add(sharing);
      m_staticThreadSharingPM->run(m_module);
    }
  }

  lotus::AliasAnalysisWrapper *aa = m_aliasAnalysis;
  if (!aa && (moduleSummary.hasSharedMemoryConcurrency() ||
              moduleSummary.has_cuda)) {
    aa = m_context->getOrCreateAliasAnalysis();
  } else if (!aa) {
    aa = m_context->getAliasAnalysisIfPresent();
  }

  if (needHappensBefore && m_mhpAnalysis) {
    m_happensBeforeAnalysis =
        std::make_unique<HappensBeforeAnalysis>(m_module, *m_mhpAnalysis);
    m_happensBeforeAnalysis->setContext(m_context.get());
    if (aa)
      m_happensBeforeAnalysis->setAliasAnalysis(aa);
    m_happensBeforeAnalysis->analyze();
  }

  if (needOpenMP) {
    const OpenMP::OpenMPSemantics *sharedOpenMP =
        m_mhpAnalysis ? m_mhpAnalysis->getOpenMPSemantics() : nullptr;
    m_openMPTaskGraph =
        std::make_unique<OpenMP::OpenMPTaskGraph>(m_module, sharedOpenMP);
    m_openMPTaskGraph->analyze();
    m_stats.openMPSummary = m_openMPTaskGraph->getSummary();
  }

  if (needMPI) {
    m_mpiAnalysis = std::make_unique<mpi::MPIAnalysis>(m_module);
    m_mpiAnalysis->runAnalysis();
    const auto &mpi_results = m_mpiAnalysis->getResults();
    m_stats.mpiSummary.operation_count =
        m_mpiAnalysis->getProcessModel().getAllOperations().size();
    m_stats.mpiSummary.nonblocking_operation_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::SEND_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::RECV_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(
            mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
    m_stats.mpiSummary.collective_operation_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::COLLECTIVE_BLOCKING) +
        m_mpiAnalysis->getOperationCount(
            mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
    m_stats.mpiSummary.communicator_management_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::COMM_MANAGEMENT);
    m_stats.mpiSummary.request_management_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::REQUEST_MANAGEMENT);
    m_stats.mpiSummary.rma_operation_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::RMA_DATA);
    m_stats.mpiSummary.rma_sync_count =
        m_mpiAnalysis->getOperationCount(mpi::MPIOpKind::RMA_SYNC);
    m_stats.mpiSummary.orphaned_request_count =
        mpi_results.orphaned_requests.size();
    m_stats.mpiSummary.potential_deadlock_count =
        mpi_results.potential_deadlocks.size();
    m_stats.mpiSummary.mismatched_collective_count =
        mpi_results.mismatched_collectives.size();
    m_stats.mpiSummary.conditional_collective_count =
        mpi_results.conditional_collectives.size();
    m_stats.mpiSummary.unsynchronized_rma_count =
        mpi_results.unsynchronized_rma.size();
    m_stats.mpiSummary.rma_race_count = mpi_results.rma_races.size();
    m_stats.mpiSummary.leaked_window_count = mpi_results.leaked_windows.size();
    m_stats.mpiSummary.collective_slot_count =
        m_mpiAnalysis->getProtocolDiagnosticCount("collective_slots_tracked");
  }

  if (needCUDA) {
    m_cudaAnalysis =
        aa ? std::make_unique<cuda::CUDAAnalysis>(m_module, aa)
           : std::make_unique<cuda::CUDAAnalysis>(m_module);
    m_cudaAnalysis->runAnalysis();
    m_stats.cudaSummary = ConcurrencyFacade::summarizeCUDA(*m_cudaAnalysis);
  }

  if (needLinuxKernel) {
    kernel::LinuxKernelAnalysisServices services;
    services.alias_analysis = aa;
    services.mhp_analysis = m_mhpAnalysis;
    services.happens_before = m_happensBeforeAnalysis.get();
    m_linuxKernelAnalysis =
        std::make_unique<kernel::LinuxKernelAnalysis>(m_module, false, services);
    m_linuxKernelAnalysis->runAnalysis();
    m_stats.linuxKernelSummary =
        ConcurrencyFacade::summarizeLinuxKernel(*m_linuxKernelAnalysis);
  }

  if (m_enableSparseFlowSensitiveRefinement && m_checkDataRaces &&
      m_mhpAnalysis) {
    m_sparseRefinement =
        std::make_unique<lotus::analysis::WholeProgramSparseRefinement>();
    lotus::analysis::WholeProgramSparseRefinement::Config sparseConfig;
    sparseConfig.mode =
        m_enableMultiStageSlicing
            ? lotus::analysis::WholeProgramSparseRefinement::Mode::
                  MultiStageSlicing
            : lotus::analysis::WholeProgramSparseRefinement::Mode::WholeProgram;
    sparseConfig.memoryPartition = m_sparseMemoryPartition;
    sparseConfig.threadContextLimit = m_threadContextLimit;
    sparseConfig.pointsToSetBackend = m_sparsePointsToSetBackend;
    sparseConfig.pruneLockSpanFlow = m_enableLockSpanPruning;
    const auto &sparseStats = m_sparseRefinement->build(
        m_module, *m_mhpAnalysis, m_locksetAnalysisView, sparseConfig);
    m_stats.sparseInterferenceEdges = sparseStats.overlay.edgesAdded;
    m_stats.sparseLockSpanPrunedPairs = sparseStats.preOverlay.spanPrunedPairs +
                                        sparseStats.overlay.spanPrunedPairs;
    m_stats.sparseLockSpanPrunedObjects =
        sparseStats.preOverlay.spanPrunedObjects +
        sparseStats.overlay.spanPrunedObjects;
    m_stats.sparsePointsToFacts = sparseStats.solver.topLevelFacts;
    m_stats.sparseMemoryRegions = sparseStats.memoryRegions.regions;
    m_stats.sparseOriginalNodes = sparseStats.slicing.originalNodes;
    m_stats.sparseSlicedNodes = sparseStats.slicing.pointsToNodes;
    m_stats.sparsePreThreads = sparseStats.preThreads.nodes;
    m_stats.sparseMainThreads = sparseStats.mainThreads.nodes;
    m_stats.sparseForkMemoryEdges = sparseStats.preOverlay.forkMemoryEdges +
                                    sparseStats.overlay.forkMemoryEdges;
    m_stats.sparseJoinMemoryEdges = sparseStats.preOverlay.joinMemoryEdges +
                                    sparseStats.overlay.joinMemoryEdges;
    m_stats.sparseHashConsedUniqueSets =
        sparseStats.solver.hashConsedUniqueSets;
    m_stats.sparseHashConsedUnionCacheHits =
        sparseStats.solver.hashConsedUnionCacheHits;
  }

  m_dataRaceChecker = std::make_unique<DataRaceChecker>(
      m_module, m_mhpAnalysis, m_locksetAnalysisView, m_escapeAnalysis.get(),
      m_threadLocalAnalysis.get(), m_staticThreadSharingAnalysis, aa,
      m_happensBeforeAnalysis.get());
  if (m_sparseRefinement) {
    m_dataRaceChecker->setSparseRefinement(m_sparseRefinement.get());
  }

  m_deadlockChecker = std::make_unique<DeadlockChecker>(
      m_module, m_locksetAnalysisView, m_mhpAnalysis,
      m_happensBeforeAnalysis.get(), m_threadAPI);
  m_deadlockChecker->setAliasAnalysis(aa);

  m_atomicityChecker = std::make_unique<AtomicityChecker>(
      m_module, m_mhpAnalysis, m_locksetAnalysisView, m_threadAPI, aa);
  m_atomicityChecker->setContext(m_context.get());
  m_atomicityChecker->setEscapeAnalysis(m_escapeAnalysis.get());
  m_atomicityChecker->setThreadLocalAnalysis(m_threadLocalAnalysis.get());
  m_atomicityChecker->setStaticThreadSharingAnalysis(
      m_staticThreadSharingAnalysis);
  m_atomicityChecker->setHappensBeforeAnalysis(m_happensBeforeAnalysis.get());

  m_condVarChecker = std::make_unique<ConditionVariableChecker>(
      m_module, m_threadAPI, m_locksetAnalysisView);
  m_condVarChecker->setAliasAnalysis(aa);
  m_condVarChecker->setContext(m_context.get());

  m_lockMismatchChecker = std::make_unique<LockMismatchChecker>(
      m_module, m_locksetAnalysisView, m_threadAPI);
  m_lockMismatchChecker->setContext(m_context.get());

  m_openMPChecker = std::make_unique<OpenMPChecker>(
      m_module, m_openMPTaskGraph.get(), m_threadAPI);
  m_openMPChecker->setContext(m_context.get());

  m_mpiChecker = std::make_unique<MPIChecker>(m_module, m_mpiAnalysis.get());
  m_cudaChecker = std::make_unique<CUDAChecker>(m_module, m_cudaAnalysis.get());
  if (m_linuxKernelAnalysis) {
    m_linuxKernelChecker = std::make_unique<LinuxKernelChecker>(
        m_module, m_linuxKernelAnalysis.get());
  }
}

void ConcurrencyChecker::runChecks() {
  if (m_checkStarvation)
    checkStarvation();
  if (m_checkDataRaces)
    checkDataRaces();
  if (m_checkDeadlocks)
    checkDeadlocks();
  if (m_checkAtomicityViolations)
    checkAtomicityViolations();
  if (m_checkCondVars)
    checkConditionVariables();
  if (m_checkLockMismatches)
    checkLockMismatches();
  if (m_checkOpenMP)
    checkOpenMPBugs();
  if (m_checkMPI)
    checkMPIBugs();
  if (m_checkCUDA)
    checkCUDABugs();
  if (m_checkLinuxKernel)
    checkLinuxKernelBugs();
}

void ConcurrencyChecker::checkDataRaces() {
  if (m_dataRaceChecker && m_mhpAnalysis) {
    auto reports = m_dataRaceChecker->checkDataRaces();
    m_stats.dataRacesFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report);
    }
  }
}

void ConcurrencyChecker::checkDeadlocks() {
  if (m_deadlockChecker && m_mhpAnalysis && m_locksetAnalysisView) {
    auto reports = m_deadlockChecker->checkDeadlocks();
    m_stats.deadlocksFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report);
    }
  }
}

void ConcurrencyChecker::checkStarvation() {
  m_stats.starvationBugsFound = 0;
  if (!m_locksetAnalysisView)
    return;
  m_starvationChecker =
      std::make_unique<StarvationChecker>(m_module, *m_locksetAnalysisView);
  for (const auto &name : m_starvationBlockingFunctions)
    m_starvationChecker->addBlockingFunction(name);
  auto reports = m_starvationChecker->checkStarvation();
  m_stats.starvationBugsFound = reports.size();
  for (const auto &report : reports)
    reportBug(report);
}

void ConcurrencyChecker::checkAtomicityViolations() {
  if (m_atomicityChecker && m_mhpAnalysis && m_locksetAnalysisView) {
    auto reports = m_atomicityChecker->checkAtomicityViolations();
    m_stats.atomicityViolationsFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report);
    }
  }
}

void ConcurrencyChecker::checkConditionVariables() {
  if (m_condVarChecker && m_locksetAnalysisView) {
    auto reports = m_condVarChecker->checkConditionVariables();
    m_stats.condVarBugsFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report);
    }
  }
}

void ConcurrencyChecker::checkLockMismatches() {
  if (m_lockMismatchChecker && m_locksetAnalysisView) {
    auto reports = m_lockMismatchChecker->checkLockMisuse();
    m_stats.lockMismatchesFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report);
    }
  }
}

void ConcurrencyChecker::checkOpenMPBugs() {
  if (!m_openMPChecker) {
    return;
  }
  auto reports = m_openMPChecker->checkOpenMPBugs();
  m_stats.openMPBugsFound = reports.size();
  for (const auto &report : reports) {
    reportBug(report);
  }
}

void ConcurrencyChecker::checkMPIBugs() {
  if (!m_mpiChecker) {
    return;
  }
  auto reports = m_mpiChecker->checkMPIBugs();
  m_stats.mpiBugsFound = reports.size();
  for (const auto &report : reports) {
    reportBug(report);
  }
}

void ConcurrencyChecker::checkCUDABugs() {
  if (!m_cudaChecker) {
    return;
  }
  auto reports = m_cudaChecker->checkCUDABugs();
  m_stats.cudaBugsFound = reports.size();
  for (const auto &report : reports) {
    reportBug(report);
  }
}

void ConcurrencyChecker::checkLinuxKernelBugs() {
  if (!m_linuxKernelChecker) {
    return;
  }
  auto reports = m_linuxKernelChecker->checkLinuxKernelBugs();
  m_stats.linuxKernelBugsFound = reports.size();
  for (const auto &report : reports) {
    reportBug(report);
  }
}

void ConcurrencyChecker::reportBug(const ConcurrencyBugReport &bug_report) {
  const int bug_type_id = lookupBugTypeId(bug_report.bugType);
  if (bug_type_id < 0) {
    return;
  }
  reportBug(bug_report, bug_type_id);
}

void ConcurrencyChecker::reportBug(const ConcurrencyBugReport &bug_report,
                                   int bug_type_id) {
  BugReport *report = new BugReport(bug_type_id);

  // Preserve all diagnostic steps — including module-level steps whose
  // instruction is null — and honor explicit traceLevel instead of blindly
  // indenting sibling concurrent call events.
  if (bug_report.steps.empty()) {
    report->append_step(nullptr, bug_report.description, 0, {}, "summary");
  } else {
    for (const auto &step : bug_report.steps) {
      if (step.instruction) {
        std::vector<NodeTag> tags;
        if (isa<CallBase>(step.instruction)) {
          tags.push_back(NodeTag::CALL_SITE);
        }

        std::string access = "step";
        if (isa<LoadInst>(step.instruction)) {
          access = "load";
        } else if (isa<StoreInst>(step.instruction)) {
          access = "store";
        } else if (isa<AtomicRMWInst>(step.instruction) ||
                   isa<AtomicCmpXchgInst>(step.instruction)) {
          access = "atomic";
        } else if (isa<MemIntrinsic>(step.instruction)) {
          access = "memop";
        } else if (isa<CallBase>(step.instruction)) {
          access = "call";
        }

        report->append_step(const_cast<Instruction *>(step.instruction),
                            step.description, step.traceLevel, tags, access);
      } else {
        report->append_step(nullptr, step.description, step.traceLevel, {},
                            "note");
      }
    }
  }

  // Forward full description and structured DataRaceInfo metadata.
  if (!bug_report.description.empty()) {
    report->add_metadata("description", bug_report.description);
  }
  if (bug_report.dataRaceInfo) {
    report->add_metadata("access_path_1",
                         bug_report.dataRaceInfo->accessPath1);
    report->add_metadata("access_path_2",
                         bug_report.dataRaceInfo->accessPath2);
    report->add_metadata("write_1",
                         bug_report.dataRaceInfo->write1 ? "true" : "false");
    report->add_metadata("write_2",
                         bug_report.dataRaceInfo->write2 ? "true" : "false");
    if (!bug_report.dataRaceInfo->sharedLocation.empty()) {
      report->add_metadata("shared_location",
                           bug_report.dataRaceInfo->sharedLocation);
    }
  }

  // Confidence and importance mapping (preserving HIGH / MEDIUM / LOW).
  int confidence = 70;
  const char *importanceStr = "MEDIUM";
  switch (bug_report.importance) {
  case BugDescription::BI_HIGH:
    confidence = 90;
    importanceStr = "HIGH";
    break;
  case BugDescription::BI_MEDIUM:
    confidence = 70;
    importanceStr = "MEDIUM";
    break;
  case BugDescription::BI_LOW:
    confidence = 50;
    importanceStr = "LOW";
    break;
  case BugDescription::BI_NA:
    confidence = 50;
    importanceStr = "NA";
    break;
  }
  report->set_conf_score(confidence);

  // Domain checker metadata.
  auto isOpenMPType = [](ConcurrencyBugType t) {
    switch (t) {
    case ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH:
    case ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH:
    case ConcurrencyBugType::OPENMP_PARTIAL_SYNC:
    case ConcurrencyBugType::OPENMP_DETACHED_TASK_LEAK:
    case ConcurrencyBugType::OPENMP_NESTED_SINGLE:
    case ConcurrencyBugType::OPENMP_NOWAIT_MISSING_BARRIER:
    case ConcurrencyBugType::OPENMP_REDUCTION_ERROR:
    case ConcurrencyBugType::OPENMP_INCORRECT_NUMTHREADS:
    case ConcurrencyBugType::OPENMP_MISSING_FLUSH:
    case ConcurrencyBugType::OMP_TASKWAIT_MISSING:
    case ConcurrencyBugType::OMP_NESTED_PARALLEL_DISABLED:
    case ConcurrencyBugType::OMP_SHARED_PRIVATE_CONFLICT:
    case ConcurrencyBugType::OMP_IF_FALSE_PARALLEL:
    case ConcurrencyBugType::OMP_ORDERED_DEPENDENCY:
    case ConcurrencyBugType::OMP_LASTPRIVATE_MISSING:
    case ConcurrencyBugType::OMP_COPYIN_NOT_SHARED:
    case ConcurrencyBugType::OMP_BARRIER_IN_CRITICAL:
    case ConcurrencyBugType::OMP_PRIVATE_IN_LOOP:
    case ConcurrencyBugType::OMP_MISSING_SCHEDULE:
      return true;
    default:
      return false;
    }
  };
  auto isMPIType = [](ConcurrencyBugType t) {
    switch (t) {
    case ConcurrencyBugType::MPI_ORPHANED_REQUEST:
    case ConcurrencyBugType::MPI_DEADLOCK:
    case ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH:
    case ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE:
    case ConcurrencyBugType::MPI_UNSYNC_RMA:
    case ConcurrencyBugType::MPI_RMA_RACE:
    case ConcurrencyBugType::MPI_WINDOW_LEAK:
    case ConcurrencyBugType::MPI_DOUBLE_FINALIZE:
    case ConcurrencyBugType::MPI_TAG_MISMATCH:
    case ConcurrencyBugType::MPI_COUNT_DATATYPE_MISMATCH:
    case ConcurrencyBugType::MPI_RANK_OUT_OF_BOUNDS:
    case ConcurrencyBugType::MPI_MISSING_FINALIZE:
    case ConcurrencyBugType::MPI_PERSISTENT_REQUEST_LEAK:
    case ConcurrencyBugType::MPI_WRONG_ROOT_RANK:
    case ConcurrencyBugType::MPI_CANCEL_WITHOUT_WAIT:
    case ConcurrencyBugType::MPI_BUFFER_OVERLAP:
    case ConcurrencyBugType::MPI_WILDCARD_IN_COLLECTIVE:
    case ConcurrencyBugType::MPI_IN_PLACE_CONFLICT:
    case ConcurrencyBugType::MPI_NULL_HANDLES:
    case ConcurrencyBugType::MPI_ROOT_NEGATIVE:
    case ConcurrencyBugType::MPI_INVALID_TAG:
    case ConcurrencyBugType::MPI_INVALID_RANK:
    case ConcurrencyBugType::MPI_TYPE_SIZE_MISMATCH:
    case ConcurrencyBugType::MPI_DESTROY_NULL_COMM:
    case ConcurrencyBugType::MPI_REQUEST_FREE_AFTER_WAIT:
    case ConcurrencyBugType::MPI_IN_PLACE_WRONG_OP:
    case ConcurrencyBugType::MPI_INVALID_RMA_TRANSITION:
    case ConcurrencyBugType::MPI_USE_AFTER_FREE_WINDOW:
    case ConcurrencyBugType::MPI_DOUBLE_WINDOW_FREE:
      return true;
    default:
      return false;
    }
  };
  auto isCUDAType = [](ConcurrencyBugType t) {
    switch (t) {
    case ConcurrencyBugType::CUDA_WARP_DIVERGENCE:
    case ConcurrencyBugType::CUDA_SHARED_MEMORY_RACE:
    case ConcurrencyBugType::CUDA_GLOBAL_MEMORY_RACE:
    case ConcurrencyBugType::CUDA_INTER_KERNEL_HAZARD:
    case ConcurrencyBugType::CUDA_BARRIER_MISMATCH:
    case ConcurrencyBugType::CUDA_BANK_CONFLICT:
    case ConcurrencyBugType::CUDA_UNCOALESCED_ACCESS:
    case ConcurrencyBugType::CUDA_VOLATILE_MISSING:
    case ConcurrencyBugType::CUDA_SYMBOLIC_CONFIG_RISK:
    case ConcurrencyBugType::CUDA_SHARED_GLOBAL_SPACE_MISMATCH:
    case ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK:
      return true;
    default:
      return false;
    }
  };
  auto isLinuxKernelType = [](ConcurrencyBugType t) {
    switch (t) {
    case ConcurrencyBugType::LINUX_KERNEL_DEADLOCK:
    case ConcurrencyBugType::LINUX_KERNEL_DOUBLE_LOCK:
    case ConcurrencyBugType::LINUX_KERNEL_UNLOCK_WITHOUT_LOCK:
    case ConcurrencyBugType::LINUX_KERNEL_LOCK_WITHOUT_UNLOCK:
    case ConcurrencyBugType::LINUX_KERNEL_LOCK_ORDER_INVERSION:
    case ConcurrencyBugType::LINUX_KERNEL_SLEEP_IN_ATOMIC:
    case ConcurrencyBugType::LINUX_KERNEL_MIX_RAW_COOKED:
    case ConcurrencyBugType::LINUX_KERNEL_IRQ_SAVE_RESTORE:
    case ConcurrencyBugType::LINUX_KERNEL_DATA_RACE:
    case ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR:
    case ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR:
    case ConcurrencyBugType::LINUX_KERNEL_TIMER_ERROR:
    case ConcurrencyBugType::LINUX_KERNEL_LIFETIME_ERROR:
      return true;
    default:
      return false;
    }
  };

  report->add_metadata("checker", "ConcurrencyChecker");
  if (isOpenMPType(bug_report.bugType)) {
    report->add_metadata("checker", "OpenMPChecker");
    report->add_metadata("openmp_task_count",
                         std::to_string(m_stats.openMPSummary.task_count));
    report->add_metadata(
        "openmp_task_with_dependencies_count",
        std::to_string(m_stats.openMPSummary.task_with_dependencies_count));
    report->add_metadata(
        "openmp_wait_boundary_count",
        std::to_string(m_stats.openMPSummary.wait_boundary_count));
    report->add_metadata(
        "openmp_partial_wait_boundary_count",
        std::to_string(m_stats.openMPSummary.partial_wait_boundary_count));
  } else if (isMPIType(bug_report.bugType)) {
    report->add_metadata("checker", "MPIChecker");
    report->add_metadata("mpi_operation_count",
                         std::to_string(m_stats.mpiSummary.operation_count));
    report->add_metadata(
        "mpi_nonblocking_operation_count",
        std::to_string(m_stats.mpiSummary.nonblocking_operation_count));
    report->add_metadata(
        "mpi_collective_operation_count",
        std::to_string(m_stats.mpiSummary.collective_operation_count));
    report->add_metadata(
        "mpi_collective_slot_count",
        std::to_string(m_stats.mpiSummary.collective_slot_count));
    report->add_metadata(
        "mpi_rma_operation_count",
        std::to_string(m_stats.mpiSummary.rma_operation_count));
    report->add_metadata(
        "mpi_leaked_window_count",
        std::to_string(m_stats.mpiSummary.leaked_window_count));
  } else if (isCUDAType(bug_report.bugType)) {
    report->add_metadata("checker", "CUDAChecker");
    report->add_metadata(
        "cuda_kernel_launch_count",
        std::to_string(m_stats.cudaSummary.kernel_launch_count));
    report->add_metadata(
        "cuda_inter_kernel_hazard_count",
        std::to_string(m_stats.cudaSummary.inter_kernel_hazard_count));
    report->add_metadata("cuda_transfer_count",
                         std::to_string(m_stats.cudaSummary.transfer_count));
    report->add_metadata(
        "cuda_unified_memory_count",
        std::to_string(m_stats.cudaSummary.unified_memory_count));
  } else if (isLinuxKernelType(bug_report.bugType)) {
    report->add_metadata("checker", "LinuxKernelChecker");
    report->add_metadata(
        "linux_kernel_operation_count",
        std::to_string(m_stats.linuxKernelSummary.operation_count));
    report->add_metadata("linux_kernel_lock_count",
                         std::to_string(m_stats.linuxKernelSummary.lock_count));
    report->add_metadata(
        "linux_kernel_rcu_section_count",
        std::to_string(m_stats.linuxKernelSummary.rcu_section_count));
  }
  report->add_metadata("importance", importanceStr);

  BugReportMgr::get_instance().insert_report(bug_type_id, report, true);
}

void ConcurrencyChecker::dumpAnalysisResults(raw_ostream &os,
                                             bool jsonFormat) const {
  if (!m_mhpAnalysis || !m_locksetAnalysisView) {
    os << "No analysis results (runAnalyses() not run or no analyses "
          "enabled).\n";
    return;
  }
  ConcurrencyAnalysisDumper dumper(m_module, m_mhpAnalysis,
                                   m_locksetAnalysisView,
                                   m_escapeAnalysis.get(), m_threadAPI);
  ConcurrencyAnalysisDumper::ExtendedSummaries ext;
  ext.hasOpenMP = m_openMPTaskGraph != nullptr;
  ext.openMP = m_stats.openMPSummary;
  ext.hasMPI = m_mpiAnalysis != nullptr;
  ext.mpi = m_stats.mpiSummary;
  ext.hasCUDA = m_cudaAnalysis != nullptr;
  ext.cuda = m_stats.cudaSummary;
  ext.hasLinuxKernel = m_linuxKernelAnalysis != nullptr;
  ext.linuxKernel = m_stats.linuxKernelSummary;
  ext.hasSparse = m_sparseRefinement != nullptr;
  ext.sparseInterferenceEdges = m_stats.sparseInterferenceEdges;
  ext.sparseLockSpanPrunedPairs = m_stats.sparseLockSpanPrunedPairs;
  ext.sparsePointsToFacts = m_stats.sparsePointsToFacts;
  ext.sparseMemoryRegions = m_stats.sparseMemoryRegions;
  ext.sparseOriginalNodes = m_stats.sparseOriginalNodes;
  ext.sparseSlicedNodes = m_stats.sparseSlicedNodes;
  dumper.setExtendedSummaries(std::move(ext));
  dumper.dump(os, jsonFormat);
}

} // namespace concurrency
