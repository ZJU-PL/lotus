/**
 * @file MHPAnalysis.cpp
 * @brief Implementation of May-Happen-in-Parallel Analysis
 *
 * This analysis constructs a Thread Flow Graph (TFG) to determine which
 * instructions may execute concurrently.
 *
 * Soundness Properties:
 * - Default Safety: The analysis is a conservative overlap oracle.
 *   It assumes two instructions may be concurrent unless structural
 *   non-overlap is proven.
 * - Synchronization:
 *   - Fork/Join: Precisely models ancestor relationships.
 *   - Condition Variables: Treats wait/signal/broadcast as synchronization
 *     boundaries, but does not synthesize inter-thread ordering.
 *   - Barriers/OpenMP waits: Enforces structural non-overlap when the runtime
 *     primitive guarantees it.
 * - Thread Instances: Conservatively assumes spawned threads may have multiple
 * instances.
 *
 * Author: rainoftime
 */

#include "Concurrency/MHP/MHPAnalysis.h"

#include <deque>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

// ============================================================================
// MHPAnalysis Implementation
// ============================================================================

MHPAnalysis::MHPAnalysis(Module &module,
                         lotus::AliasAnalysisWrapper *aliasAnalysis)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()),
      m_alias_analysis(aliasAnalysis) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
}

MHPAnalysis::MHPAnalysis(concurrency::ConcurrencyContext &context)
    : m_module(context.getModule()), m_thread_api(context.getThreadAPI()),
      m_context(&context),
      m_alias_analysis(context.getAliasAnalysisIfPresent()) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
}

lotus::AliasAnalysisWrapper *MHPAnalysis::getAliasAnalysis() const {
  if (m_alias_analysis) {
    return m_alias_analysis;
  }
  if (m_context) {
    m_alias_analysis = m_context->getOrCreateAliasAnalysis();
    return m_alias_analysis;
  }
  m_owned_alias_analysis = lotus::AliasAnalysisFactory::create(
      m_module, lotus::AAConfig::SparrowAA_NoCtx());
  m_alias_analysis = m_owned_alias_analysis.get();
  return m_alias_analysis;
}

namespace {
bool isOnlyUsedInVTableOrTypeInfo(const Function &func) {
  bool sawNonCallUse = false;
  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  for (const Use &u : func.uses()) {
    const User *user = u.getUser();
    if (const auto *cb = dyn_cast<CallBase>(user)) {
      if (cb->isCallee(&u)) {
        continue;
      }
    }
    sawNonCallUse = true;
    if (visited.insert(user).second) {
      worklist.push_back(user);
    }
  }
  if (!sawNonCallUse) {
    return true;
  }
  while (!worklist.empty()) {
    const Value *cur = worklist.front();
    worklist.pop_front();
    if (const auto *gv = dyn_cast<GlobalVariable>(cur)) {
      StringRef name = gv->getName();
      if (name.startswith("_ZTV") || name.startswith("_ZTI") ||
          name.startswith("_ZTC") || name.startswith("_ZTS")) {
        continue;
      }
      return false;
    }
    if (isa<ConstantAggregate>(cur) || isa<ConstantExpr>(cur)) {
      if (cur->use_empty()) {
        return false;
      }
      for (const User *u : cur->users()) {
        if (visited.insert(u).second) {
          worklist.push_back(u);
        }
      }
      continue;
    }
    return false;
  }
  return true;
}

bool isLikelyThreadEntryCandidate(const Function &func,
                                  bool pthreadOnlyForks = false) {
  if (func.isDeclaration() || func.arg_size() > 1) {
    return false;
  }
  if (isOnlyUsedInVTableOrTypeInfo(func)) {
    return false;
  }
  if (pthreadOnlyForks) {
    if (func.arg_size() != 1) {
      return false;
    }
    return func.getFunctionType()->getParamType(0)->isPointerTy() &&
           func.getReturnType()->isPointerTy();
  }
  if (func.arg_size() == 1 &&
      !func.getFunctionType()->getParamType(0)->isPointerTy()) {
    return false;
  }
  Type *retTy = func.getReturnType();
  return retTy->isPointerTy() || retTy->isVoidTy() || retTy->isIntegerTy();
}
} // namespace

MHPAnalysis::~MHPAnalysis() = default;

void MHPAnalysis::analyze() {
  errs() << "Starting MHP Analysis...\n";

  m_order_cache.clear();
  m_mhp_cache.clear();
  m_multi_instance_overlap_cache.clear();
  m_order_visit_stamp.clear();
  m_order_current_stamp = 0;
  m_order_bfs_queue.clear();
  m_mhp_pairs.clear();
  m_inst_to_thread.clear();
  m_instruction_ordinals.clear();
  m_func_context_count.clear();
  m_func_shared_context.clear();
  m_next_merged_context_id = 1;
  m_sync_relevant_functions.clear();
  m_thread_fork_sites.clear();
  m_thread_parents.clear();
  m_thread_children.clear();
  m_fork_to_thread.clear();
  m_fork_node_to_thread.clear();
  m_join_to_thread.clear();
  m_detached_threads.clear();
  m_pthread_value_to_threads.clear();
  m_thread_to_pthread_value.clear();
  m_condvar_signals.clear();
  m_condvar_waits.clear();
  m_barrier_waits.clear();
  m_barrier_expected_counts.clear();
  m_barrier_init_sites.clear();
  m_uncertain_barrier_counts.clear();
  m_visited_functions_by_thread.clear();
  m_active_call_stack_by_thread.clear();
  m_pre_fork_main_nodes.clear();
  m_thread_entry_candidates.clear();
  m_has_unresolved_fork = false;
  m_has_multi_context_nodes = false;
  m_single_main_thread_only = false;
  m_module_may_spawn_threads =
      m_context
          ? m_context->hasPotentialThreadCreation()
          : concurrency::ConcurrencyContext::moduleHasPotentialThreadCreation(
                m_module, m_thread_api);
  m_pending_joins.clear();
  m_pending_detaches.clear();
  m_multi_instance_threads.clear();
  m_dom_cache.clear();
  m_openmp_task_threads.clear();
  m_openmp_task_exclusions.clear();
  m_openmp_semantics = std::make_unique<OpenMP::OpenMPSemantics>(m_module);
  m_openmp_semantics->analyze();
  if (!m_openmp_semantics->getTasks().empty() ||
      !m_openmp_semantics->getSemanticEntities().empty()) {
    m_module_may_spawn_threads = true;
  }
  m_next_thread_id = 1;
  m_region_analysis.reset();
  m_lockset.reset();
  m_tfg = std::make_unique<ThreadFlowGraph>();
  if (m_context) {
    m_call_graph = &m_context->getCallGraph();
    m_thread_multiplicity = &m_context->getThreadMultiplicity();
  } else {
    m_owned_call_graph = std::make_unique<CallGraph>(m_module);
    m_call_graph = m_owned_call_graph.get();
    m_owned_thread_multiplicity =
        std::make_unique<concurrency::ThreadMultiplicityAnalysis>(m_module,
                                                                  m_call_graph);
    m_thread_multiplicity = m_owned_thread_multiplicity.get();
  }

  // Only materialize alias analysis when the module actually uses joins or
  // locks (or when a caller already supplied one); single-threaded modules
  // avoid running an unused whole-program Andersen pass.
  bool has_joins = false;
  bool has_locks = false;
  if (m_context) {
    const auto &summary = m_context->getSummary();
    has_joins = summary.has_joins;
    has_locks = summary.has_locks;
  } else {
    for (const Function &func : m_module) {
      if (func.isDeclaration()) {
        continue;
      }
      for (const Instruction &inst : instructions(func)) {
        if (m_thread_api->isTDJoin(&inst)) {
          has_joins = true;
        } else if (m_thread_api->isTDAcquire(&inst) ||
                   m_thread_api->isTDRelease(&inst) ||
                   m_thread_api->isReadLockAcquire(&inst) ||
                   m_thread_api->isWriteLockAcquire(&inst) ||
                   m_thread_api->isTryLock(&inst)) {
          has_locks = true;
        }
      }
    }
  }
  lotus::AliasAnalysisWrapper *aa_for_mhp = m_alias_analysis;
  if (!aa_for_mhp && (has_joins || (m_enable_lockset_analysis && has_locks))) {
    aa_for_mhp = getAliasAnalysis();
  }

  m_join_target_analysis =
      std::make_unique<JoinTargetAnalysis>(m_module, aa_for_mhp);

  buildThreadFlowGraph();
  {
    const Function *main_fn = m_module.getFunction("main");
    if (!main_fn || main_fn->isDeclaration()) {
      main_fn = m_module.getFunction("MAIN__");
    }
    const bool unresolved_may_spawn_defined =
        m_has_unresolved_fork && !m_thread_entry_candidates.empty();
    m_single_main_thread_only =
        m_tfg && !unresolved_may_spawn_defined &&
        m_multi_instance_threads.empty() &&
        m_tfg->getAllThreads().size() <= 1 &&
        ((main_fn && !main_fn->isDeclaration()) ||
         (!m_module_may_spawn_threads && !has_locks && !has_joins));
  }
  ++m_analysis_generation;

  // Optional: LockSet analysis for more precise reasoning
  if (m_enable_lockset_analysis) {
    analyzeLockSets();
  }

  if (m_precompute_mhp_pairs) {
    computeMHPPairs();
  }

  errs() << "MHP Analysis Complete!\n";
}

bool MHPAnalysis::instructionCanonicalLess(const Instruction *lhs,
                                           const Instruction *rhs) const {
  if (lhs == rhs) {
    return false;
  }
  auto lhs_it = m_instruction_ordinals.find(lhs);
  auto rhs_it = m_instruction_ordinals.find(rhs);
  if (lhs_it != m_instruction_ordinals.end() &&
      rhs_it != m_instruction_ordinals.end()) {
    return lhs_it->second < rhs_it->second;
  }
  return std::less<const Instruction *>{}(lhs, rhs);
}

void MHPAnalysis::enableLockSetAnalysis() { m_enable_lockset_analysis = true; }

void MHPAnalysis::computeSyncRelevantFunctions() {
  m_sync_relevant_functions.clear();
  std::deque<const Function *> worklist;
  std::unordered_map<const Function *, std::vector<const Function *>> callers;

  auto markRelevant = [&](const Function *func) {
    if (func && !func->isDeclaration() &&
        m_sync_relevant_functions.insert(func).second) {
      worklist.push_back(func);
    }
  };

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    bool direct_sync = false;
    for (const Instruction &inst : instructions(func)) {
      if (isa<FenceInst>(inst) || isa<AtomicRMWInst>(inst) ||
          isa<AtomicCmpXchgInst>(inst)) {
        direct_sync = true;
      } else if (const auto *load = dyn_cast<LoadInst>(&inst)) {
        if (load->isAtomic()) {
          direct_sync = true;
        }
      } else if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        if (store->isAtomic()) {
          direct_sync = true;
        }
      } else if (const auto *cb = dyn_cast<CallBase>(&inst)) {
        const ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
        if (type != ThreadAPI::TD_DUMMY || m_thread_api->isTDFork(&inst) ||
            m_thread_api->isTDJoin(&inst) || m_thread_api->isTDAcquire(&inst) ||
            m_thread_api->isTDRelease(&inst) || m_thread_api->isTDExit(&inst) ||
            m_thread_api->isTDCondWait(&inst) ||
            m_thread_api->isTDCondSignal(&inst) ||
            m_thread_api->isTDCondBroadcast(&inst) ||
            m_thread_api->isTDBarWait(&inst)) {
          direct_sync = true;
        }
        if (const Function *callee = m_thread_api->getCallee(cb)) {
          if (!callee->isDeclaration()) {
            callers[callee].push_back(&func);
          }
        }
      }
    }
    if (direct_sync) {
      markRelevant(&func);
    }
  }

  if (m_openmp_semantics) {
    for (const auto &task : m_openmp_semantics->getTasks()) {
      if (task && task->task_create) {
        markRelevant(task->task_create->getFunction());
      }
      if (task && task->task_function) {
        markRelevant(task->task_function);
      }
    }
    for (const auto &boundary : m_openmp_semantics->getWaitBoundaryInfos()) {
      if (boundary.inst) {
        markRelevant(boundary.inst->getFunction());
      }
    }
    for (const auto &event : m_openmp_semantics->getTaskCompletionEvents()) {
      if (event.inst) {
        markRelevant(event.inst->getFunction());
      }
    }
  }

  while (!worklist.empty()) {
    const Function *current = worklist.front();
    worklist.pop_front();
    auto it = callers.find(current);
    if (it == callers.end()) {
      continue;
    }
    for (const Function *caller : it->second) {
      markRelevant(caller);
    }
  }
}

CallContextID MHPAnalysis::chooseCalleeContext(ThreadID tid,
                                               const Function *target,
                                               SyncNode *call_node,
                                               bool force_merged,
                                               size_t queued_node_estimate) {
  ThreadFuncKey key{tid, target};
  auto shared_it = m_func_shared_context.find(key);
  if (shared_it != m_func_shared_context.end()) {
    return shared_it->second;
  }

  const bool sync_relevant = m_sync_relevant_functions.count(target) != 0;
  const size_t max_contexts =
      sync_relevant ? kMaxSyncContextsPerFunction : kMaxContextsPerFunction;
  const size_t current_depth = m_active_call_stack_by_thread[tid].size();
  const size_t max_depth =
      sync_relevant ? (kMaxContextDepth + 2) : kMaxContextDepth;
  const bool within_node_budget =
      (m_tfg->getAllNodes().size() + queued_node_estimate) <
      kMaxClonedTFGNodes;

  size_t &ctx_count = m_func_context_count[key];
  if (!force_merged && within_node_budget && current_depth < max_depth &&
      ctx_count < max_contexts) {
    ++ctx_count;
    return call_node->getNodeID();
  }

  const CallContextID merged_ctx =
      kMergedContextBit | (m_next_merged_context_id++);
  m_func_shared_context.emplace(key, merged_ctx);
  return merged_ctx;
}

void MHPAnalysis::buildThreadFlowGraph() {
  errs() << "Building Thread Flow Graph...\n";

  // Find main function (with Fortran MAIN__ fallback).
  Function *main_func = m_module.getFunction("main");
  if (!main_func || main_func->isDeclaration()) {
    main_func = m_module.getFunction("MAIN__");
  }
  if (!main_func || main_func->isDeclaration()) {
    errs() << "Warning: No main function found\n";
    return;
  }

  computeSyncRelevantFunctions();

  if (m_join_target_analysis) {
    m_join_target_analysis->analyze();
  }

  // Main thread (thread 0)
  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0, 0);
  if (m_openmp_semantics) {
    lowerOpenMPTasks(*m_openmp_semantics);
  }
  finalizePendingDetaches();
  finalizePendingJoins();
  finalizeBarrierPhases();

  // Build reachability index for faster structural-order queries
  m_tfg->buildReachabilityIndex();
  recomputePreForkMainNodes();

  errs() << "Thread Flow Graph built with " << m_tfg->getAllNodes().size()
         << " nodes\n";
}

std::pair<ThreadID, ThreadID>
MHPAnalysis::normalizeThreadPair(ThreadID lhs, ThreadID rhs) const {
  return lhs < rhs ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
}

void MHPAnalysis::lowerOpenMPTasks(const OpenMP::OpenMPSemantics &semantics) {
  auto wireInlineTask = [&](const OpenMP::Task *task, ThreadID parent_tid) {
    for (SyncNode *create_node :
         m_tfg->getNodes(task->task_create, parent_tid)) {
      CallContextID parent_ctx = create_node->getCallContextID();
      CallContextID callee_ctx =
          chooseCalleeContext(parent_tid, task->task_function, create_node);
      m_has_multi_context_nodes = true;
      processFunction(task->task_function, parent_tid, callee_ctx);

      if (SyncNode *task_entry = m_tfg->getNode(
              &task->task_function->front().front(), parent_tid, callee_ctx)) {
        m_tfg->addCallEdge(create_node, task_entry);
      }

      std::vector<SyncNode *> task_exits = m_tfg->getFunctionExitNodes(
          parent_tid, task->task_function, callee_ctx);
      if (task_exits.empty()) {
        continue;
      }

      if (const Instruction *next_inst = task->task_create->getNextNode()) {
        SyncNode *return_site =
            m_tfg->getNode(next_inst, parent_tid, parent_ctx);
        for (SyncNode *task_exit : task_exits) {
          if (return_site) {
            m_tfg->addRetEdge(task_exit, return_site);
          }
        }
        continue;
      }

      if (task->task_create->isTerminator()) {
        for (const BasicBlock *succ :
             successors(task->task_create->getParent())) {
          if (succ->empty()) {
            continue;
          }
          SyncNode *return_site =
              m_tfg->getNode(&succ->front(), parent_tid, parent_ctx);
          for (SyncNode *task_exit : task_exits) {
            if (return_site) {
              m_tfg->addRetEdge(task_exit, return_site);
            }
          }
        }
      }
    }
  };

  for (const auto &task_uptr : semantics.getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    if (!task || !task->task_create || !task->task_function ||
        task->task_function->isDeclaration()) {
      continue;
    }

    ThreadID parent_tid = getThreadID(task->task_create);
    if (parent_tid == kUnknownThread) {
      continue;
    }

    if (task->execution_mode == OpenMP::TaskExecutionMode::Included) {
      wireInlineTask(task, parent_tid);
      continue;
    }

    for (SyncNode *create_node :
         m_tfg->getNodes(task->task_create, parent_tid)) {
      ThreadID task_tid = allocateThreadID();
      m_openmp_task_threads[task->task_create].push_back(
          {task_tid, create_node->getCallContextID()});
      m_thread_fork_sites[task_tid] = task->task_create;
      m_thread_parents[task_tid] = parent_tid;
      m_thread_children[parent_tid].push_back(task_tid);
      m_fork_to_thread[task->task_create].insert(task_tid);
      if (task->is_recurrent && !task->recurrent_instances_serialized &&
          !task->recurrent_instances_excluded) {
        m_multi_instance_threads.insert(task_tid);
      }

      m_tfg->addThread(task_tid, task->task_function);
      processFunction(task->task_function, task_tid, 0);

      create_node->setForkedThread(task_tid);
      if (SyncNode *task_entry = m_tfg->getThreadEntryNode(task_tid)) {
        m_tfg->addInterThreadEdge(create_node, task_entry, EdgeKind::Create);
      }
    }
  }

  auto getTaskThreads = [&](const OpenMP::Task *task) -> std::vector<ThreadID> {
    if (!task || !task->task_create) {
      return {};
    }
    auto it = m_openmp_task_threads.find(task->task_create);
    if (it == m_openmp_task_threads.end()) {
      return {};
    }
    std::vector<ThreadID> tids;
    tids.reserve(it->second.size());
    for (const auto &[tid, unused_ctx] : it->second) {
      (void)unused_ctx;
      tids.push_back(tid);
    }
    return tids;
  };

  for (const auto &task_uptr : semantics.getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    std::vector<ThreadID> task_tids = getTaskThreads(task);
    if (task_tids.empty()) {
      continue;
    }

    for (ThreadID task_tid : task_tids) {
      SyncNode *task_entry = m_tfg->getThreadEntryNode(task_tid);
      for (const OpenMP::Task *pred : task->predecessors) {
        for (ThreadID pred_tid : getTaskThreads(pred)) {
          for (SyncNode *pred_exit : m_tfg->getThreadExitNodes(pred_tid)) {
            if (pred_exit && task_entry) {
              m_tfg->addInterThreadEdge(pred_exit, task_entry,
                                        EdgeKind::TaskDepend);
            }
          }
        }
      }

      for (const OpenMP::Task *excluded : task->exclusions) {
        for (ThreadID excluded_tid : getTaskThreads(excluded)) {
          m_openmp_task_exclusions.insert(
              normalizeThreadPair(task_tid, excluded_tid));
        }
      }
    }
  }

  for (const auto &task_uptr : semantics.getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    for (ThreadID task_tid : getTaskThreads(task)) {
      for (const OpenMP::Task *excluded : task->exclusions) {
        for (ThreadID excluded_tid : getTaskThreads(excluded)) {
          if (task_tid != excluded_tid) {
            m_openmp_task_exclusions.insert(
                normalizeThreadPair(task_tid, excluded_tid));
          }
        }
      }
    }
  }

  for (const auto &boundary : semantics.getWaitBoundaryInfos()) {
    if (!boundary.inst) {
      continue;
    }
    if (boundary.is_partial_wait) {
      continue;
    }

    ThreadID parent_tid = getThreadID(boundary.inst);
    if (parent_tid == kUnknownThread) {
      continue;
    }

    for (SyncNode *wait_node : m_tfg->getNodes(boundary.inst, parent_tid)) {
      for (const auto &task_uptr : semantics.getTasks()) {
        const OpenMP::Task *task = task_uptr.get();
        if (!task) {
          continue;
        }
        if (boundary.is_taskgroup_end) {
          if (boundary.taskgroup_id == 0 ||
              task->taskgroup_ids.count(boundary.taskgroup_id) == 0) {
            continue;
          }
        } else if (task->scheduling_context_id !=
                       boundary.scheduling_context_id ||
                   task->event_order >= boundary.event_order ||
                   task->phase_id != boundary.phase_id) {
          continue;
        }

        for (ThreadID task_tid : getTaskThreads(task)) {
          for (SyncNode *task_exit : m_tfg->getThreadExitNodes(task_tid)) {
            if (task_exit) {
              m_tfg->addInterThreadEdge(task_exit, wait_node,
                                        EdgeKind::TaskWait);
            }
          }
        }
      }
    }
  }

  for (const OpenMP::TaskCompletionEvent &event :
       semantics.getTaskCompletionEvents()) {
    if (!event.task || !event.task->task_create || !event.inst) {
      continue;
    }

    std::vector<ThreadID> task_tids = getTaskThreads(event.task);
    if (task_tids.empty()) {
      continue;
    }

    ThreadID parent_tid = getThreadID(event.inst);
    if (parent_tid == kUnknownThread) {
      continue;
    }
    for (SyncNode *completion_node : m_tfg->getNodes(event.inst, parent_tid)) {
      for (ThreadID task_tid : task_tids) {
        for (SyncNode *task_exit : m_tfg->getThreadExitNodes(task_tid)) {
          if (task_exit) {
            m_tfg->addInterThreadEdge(task_exit, completion_node,
                                      EdgeKind::TaskCompletion);
          }
        }
      }
    }
  }
}

void MHPAnalysis::processFunction(const Function *root_func, ThreadID root_tid,
                                  CallContextID root_ctx) {
  if (!root_func || root_func->isDeclaration())
    return;

  enum class WorkKind { Enter, Exit };
  struct WorkItem {
    WorkKind kind = WorkKind::Enter;
    const Function *func = nullptr;
    ThreadID tid = 0;
    CallContextID ctx = 0;
  };
  struct PendingCallEdge {
    SyncNode *call_node = nullptr;
    ThreadID tid = 0;
    const Function *callee = nullptr;
    CallContextID callee_ctx = 0;
    SyncNode *normal_ret = nullptr;
    SyncNode *unwind_ret = nullptr;
  };
  struct PendingForkEdge {
    SyncNode *fork_node = nullptr;
    ThreadID child_tid = 0;
  };
  struct DeferredJoin {
    const Instruction *inst = nullptr;
    SyncNode *node = nullptr;
    ThreadID tid = 0;
  };

  struct FrameKey {
    const Function *func = nullptr;
    ThreadID tid = 0;
    CallContextID ctx = 0;
    bool operator==(const FrameKey &o) const {
      return func == o.func && tid == o.tid && ctx == o.ctx;
    }
  };
  struct FrameKeyHash {
    size_t operator()(const FrameKey &k) const {
      size_t h = std::hash<const void *>{}(k.func);
      h ^= std::hash<ThreadID>{}(k.tid) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
      h ^= std::hash<CallContextID>{}(k.ctx) + 0x9e3779b97f4a7c15ULL +
           (h << 6) + (h >> 2);
      return h;
    }
  };
  DenseMap<const Function *, size_t> func_inst_count_cache;
  auto getFuncInstCount = [&](const Function *fn) -> size_t {
    if (!fn) {
      return 0;
    }
    auto it = func_inst_count_cache.find(fn);
    if (it != func_inst_count_cache.end()) {
      return it->second;
    }
    size_t count = 0;
    for (const BasicBlock &bb : *fn) {
      count += bb.size();
    }
    func_inst_count_cache.try_emplace(fn, count);
    return count;
  };

  std::unordered_set<FrameKey, FrameKeyHash> queued_frames;
  std::vector<WorkItem> work_stack;
  size_t queued_node_estimate = getFuncInstCount(root_func);
  queued_frames.insert({root_func, root_tid, root_ctx});
  work_stack.push_back({WorkKind::Enter, root_func, root_tid, root_ctx});
  std::vector<PendingCallEdge> pending_calls;
  std::vector<PendingForkEdge> pending_forks;
  std::vector<DeferredJoin> deferred_joins;

  while (!work_stack.empty()) {
    WorkItem item = work_stack.back();
    work_stack.pop_back();

    const Function *func = item.func;
    const ThreadID tid = item.tid;
    const CallContextID ctx = item.ctx;

    if (item.kind == WorkKind::Exit) {
      auto &active_stack = m_active_call_stack_by_thread[tid];
      if (!active_stack.empty() && active_stack.back() == func) {
        active_stack.pop_back();
      }
      continue;
    }

    if (!func || func->isDeclaration())
      continue;
    const size_t func_insts = getFuncInstCount(func);
    queued_node_estimate =
        (queued_node_estimate >= func_insts) ? (queued_node_estimate - func_insts) : 0;

    auto &active_stack = m_active_call_stack_by_thread[tid];
    if (std::find(active_stack.begin(), active_stack.end(), func) !=
        active_stack.end()) {
      continue;
    }

    // Avoid re-processing functions for the same thread context
    if (m_visited_functions_by_thread[tid][ctx].count(func)) {
      continue;
    }
    m_visited_functions_by_thread[tid][ctx].insert(func);
    active_stack.push_back(func);
    work_stack.push_back({WorkKind::Exit, func, tid, ctx});

    SmallVector<WorkItem, 8> children_to_visit;

    // Scope local_nodes and indirect_cg_targets so all per-function temporary
    // maps are destroyed BEFORE descending into callee/child frames.
    {
      // --- Pass 1: Create all nodes for this function ---
      DenseMap<const Instruction *, SyncNode *> local_nodes;
      local_nodes.reserve(func_insts);
      for (const BasicBlock &bb : *func) {
        for (const Instruction &inst : bb) {
          mapInstructionToThread(&inst, tid);
          SyncNodeType node_type = SyncNodeType::REGULAR_INST;

          if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
            ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
            if (type == ThreadAPI::TD_BAR_INIT && cb->arg_size() >= 3) {
              if (const Value *barrier = m_thread_api->getBarrierVal(&inst)) {
                if (const auto *count =
                        dyn_cast<ConstantInt>(cb->getArgOperand(2))) {
                  const Value *key = barrier->stripPointerCasts();
                  const size_t value =
                      static_cast<size_t>(count->getZExtValue());
                  size_t &init_sites = m_barrier_init_sites[key];
                  ++init_sites;
                  auto expected = m_barrier_expected_counts.find(key);
                  if (init_sites > 1 ||
                      (expected != m_barrier_expected_counts.end() &&
                       expected->second != value)) {
                    m_uncertain_barrier_counts.insert(key);
                    m_barrier_expected_counts.erase(key);
                  } else {
                    m_barrier_expected_counts[key] = value;
                  }
                }
              }
            }
            if (m_thread_api->isTDFork(&inst)) {
              node_type = SyncNodeType::THREAD_FORK;
            } else if (m_thread_api->isTDJoin(&inst)) {
              node_type = SyncNodeType::THREAD_JOIN;
            } else if (m_thread_api->isTDAcquire(&inst)) {
              node_type = SyncNodeType::LOCK_ACQUIRE;
            } else if (m_thread_api->isTDRelease(&inst)) {
              node_type = SyncNodeType::LOCK_RELEASE;
            } else if (m_thread_api->isTDExit(&inst)) {
              node_type = SyncNodeType::THREAD_EXIT;
            } else if (m_thread_api->isTDCondWait(&inst)) {
              node_type = SyncNodeType::COND_WAIT;
            } else if (m_thread_api->isTDCondBroadcast(&inst)) {
              node_type = SyncNodeType::COND_BROADCAST;
            } else if (m_thread_api->isTDCondSignal(&inst)) {
              node_type = SyncNodeType::COND_SIGNAL;
            } else if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT ||
                       type == ThreadAPI::TD_BARRIER_ARRIVE ||
                       m_thread_api->isTDBarWait(&inst)) {
              node_type = SyncNodeType::BARRIER_WAIT;
            }
          }
          SyncNode *created = m_tfg->createNode(&inst, node_type, tid, ctx);
          local_nodes[&inst] = created;
        }
      }

      // --- Pass 2: Add edges and handle synchronization logic ---
      SyncNode *entry_node = nullptr;
      if (!func->empty() && !func->front().empty()) {
        entry_node = local_nodes.lookup(&func->front().front());
        if (entry_node && ctx == 0) {
          m_tfg->setThreadEntryNode(tid, entry_node);
        }
      }

      bool indirect_targets_indexed = false;
      DenseMap<const CallBase *, SmallVector<CallGraphNode *, 4>>
          indirect_cg_targets;
      auto ensureIndirectTargetsIndexed = [&]() {
        if (indirect_targets_indexed) {
          return;
        }
        indirect_targets_indexed = true;
        if (!m_call_graph) {
          return;
        }
        if (CallGraphNode *cgNode = (*m_call_graph)[func]) {
          for (auto &callRecord : *cgNode) {
            if (!callRecord.first.hasValue()) {
              continue;
            }
            if (const auto *record_cb =
                    dyn_cast_or_null<CallBase>(*callRecord.first)) {
              indirect_cg_targets[record_cb].push_back(callRecord.second);
            }
          }
        }
      };

      for (const BasicBlock &bb : *func) {
        SyncNode *prev_node = nullptr;
        for (const Instruction &inst : bb) {
          SyncNode *node = local_nodes.lookup(&inst);
          if (!node) {
            prev_node = nullptr;
            continue;
          }

          if (prev_node) {
            m_tfg->addIntraThreadEdge(prev_node, node);
          }
          prev_node = node;

          if (inst.isTerminator()) {
            for (const BasicBlock *succ : successors(inst.getParent())) {
              if (!succ->empty()) {
                SyncNode *succ_node = local_nodes.lookup(&succ->front());
                if (succ_node)
                  m_tfg->addIntraThreadEdge(node, succ_node);
              }
            }
          }

          if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
            switch (node->getType()) {
            case SyncNodeType::THREAD_FORK: {
              std::vector<std::pair<const Function *, ThreadID>> spawns;
              handleThreadFork(&inst, node, tid, &spawns);
              for (const auto &sp : spawns) {
                pending_forks.push_back({node, sp.second});
                if (queued_frames.insert({sp.first, sp.second, 0}).second) {
                  queued_node_estimate += getFuncInstCount(sp.first);
                  children_to_visit.push_back(
                      {WorkKind::Enter, sp.first, sp.second, 0});
                }
              }
              break;
            }
            case SyncNodeType::THREAD_JOIN:
              deferred_joins.push_back({&inst, node, tid});
              break;
            case SyncNodeType::LOCK_ACQUIRE:
              handleLockAcquire(&inst, node);
              break;
            case SyncNodeType::LOCK_RELEASE:
              handleLockRelease(&inst, node);
              break;
            case SyncNodeType::COND_WAIT:
              handleCondWait(&inst, node);
              break;
            case SyncNodeType::COND_SIGNAL:
            case SyncNodeType::COND_BROADCAST:
              handleCondSignal(&inst, node);
              break;
            case SyncNodeType::BARRIER_WAIT:
              handleBarrier(&inst, node);
              break;
            case SyncNodeType::THREAD_EXIT:
              break;
            default: {
              const ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
              if (type == ThreadAPI::TD_DETACH) {
                handleThreadDetach(&inst);
                break;
              }
              if (m_thread_api->isCUDAKernelCallImmediatelyAfterLaunch(&inst)) {
                break;
              }
              const Function *callee = m_thread_api->getCallee(cb);

              auto enqueueCallee = [&](const Function *target,
                                       bool force_merged = false) -> bool {
                if (!target || target->isDeclaration()) {
                  return false;
                }
                const auto &active = m_active_call_stack_by_thread[tid];
                if (std::find(active.begin(), active.end(), target) !=
                    active.end()) {
                  return false;
                }
                const CallContextID callee_ctx = chooseCalleeContext(
                    tid, target, node, force_merged, queued_node_estimate);
                m_has_multi_context_nodes = true;

                SyncNode *normal_ret = nullptr;
                SyncNode *unwind_ret = nullptr;
                if (const Instruction *next_inst = inst.getNextNode()) {
                  normal_ret = local_nodes.lookup(next_inst);
                } else if (const auto *invoke = dyn_cast<InvokeInst>(&inst)) {
                  if (invoke->getNormalDest() &&
                      !invoke->getNormalDest()->empty()) {
                    normal_ret =
                        local_nodes.lookup(&invoke->getNormalDest()->front());
                  }
                  if (invoke->getUnwindDest() &&
                      !invoke->getUnwindDest()->empty()) {
                    unwind_ret =
                        local_nodes.lookup(&invoke->getUnwindDest()->front());
                  }
                }
                pending_calls.push_back(
                    {node, tid, target, callee_ctx, normal_ret, unwind_ret});
                if (!m_visited_functions_by_thread[tid][callee_ctx].count(
                        target) &&
                    queued_frames.insert({target, tid, callee_ctx}).second) {
                  queued_node_estimate += getFuncInstCount(target);
                  children_to_visit.push_back(
                      {WorkKind::Enter, target, tid, callee_ctx});
                }
                return true;
              };

              if (type == ThreadAPI::TD_CALL_ONCE) {
                for (unsigned idx = 1; idx < cb->arg_size(); ++idx) {
                  const Value *arg = cb->getArgOperand(idx);
                  if (!arg) {
                    continue;
                  }
                  if (const auto *callable =
                          dyn_cast<Function>(arg->stripPointerCasts())) {
                    enqueueCallee(callable);
                    break;
                  }
                }
              }

              if (!callee) {
                if (!m_module_may_spawn_threads) {
                  // Single-threaded modules without any thread creation sites
                  // cannot hide a fork behind an indirect call.
                  break;
                }
                bool resolved_indirect_target = false;
                bool has_unresolved_indirect_target = false;
                if (m_context) {
                  const auto resolved = m_context->resolveCallees(cb);
                  if (!resolved.has_unresolved) {
                    // Closed indirect call: wire all resolved targets so we
                    // avoid false conservatism.
                    for (const Function *possibleCallee : resolved.targets) {
                      if (enqueueCallee(possibleCallee)) {
                        resolved_indirect_target = true;
                      } else {
                        has_unresolved_indirect_target = true;
                      }
                    }
                    if (!resolved.targets.empty()) {
                      resolved_indirect_target = true;
                    }
                  } else {
                    // Open/unresolved indirect call: conservatism will activate,
                    // so wire sync-relevant targets to keep all fork/join/lock
                    // edges without creating dense non-sync call-graph cliques.
                    has_unresolved_indirect_target = true;
                    for (const Function *possibleCallee : resolved.targets) {
                      if (m_sync_relevant_functions.count(possibleCallee)) {
                        if (enqueueCallee(possibleCallee,
                                          /*force_merged=*/true)) {
                          resolved_indirect_target = true;
                        }
                      }
                    }
                  }
                } else {
                  ensureIndirectTargetsIndexed();
                  auto cg_it = indirect_cg_targets.find(cb);
                  if (cg_it != indirect_cg_targets.end()) {
                    for (CallGraphNode *calleeNode : cg_it->second) {
                      if (!calleeNode) {
                        has_unresolved_indirect_target = true;
                        continue;
                      }
                      Function *possibleCallee = calleeNode->getFunction();
                      if (!possibleCallee || possibleCallee->isDeclaration()) {
                        if (!possibleCallee ||
                            !concurrency::ConcurrencyContext::
                                isKnownSyncFreeExternalFunction(
                                    possibleCallee)) {
                          has_unresolved_indirect_target = true;
                        }
                        continue;
                      }
                      if (enqueueCallee(possibleCallee)) {
                        resolved_indirect_target = true;
                      } else {
                        has_unresolved_indirect_target = true;
                      }
                    }
                  }
                }
                if (!resolved_indirect_target ||
                    has_unresolved_indirect_target) {
                  enableIndirectForkConservatism();
                }
              } else if (!callee->isDeclaration()) {
                enqueueCallee(callee);
              }
              break;
            }
            }
          }

          if (isa<ReturnInst>(inst) || isa<ResumeInst>(inst) ||
              node->getType() == SyncNodeType::THREAD_EXIT) {
            m_tfg->setFunctionExitNode(tid, func, node, ctx);
            if (ctx == 0 && m_tfg->getThreadEntry(tid) == func) {
              m_tfg->setThreadExitNode(tid, node);
            }
          }
        }
      }
    }

    // Push children in reverse order to preserve deterministic DFS order.
    for (auto it = children_to_visit.rbegin(); it != children_to_visit.rend();
         ++it) {
      work_stack.push_back(*it);
    }
  }

  // Wire all deferred fork, call, return, and join edges now that every
  // reachable function/thread in this traversal has materialized its nodes.
  for (const PendingForkEdge &pf : pending_forks) {
    if (SyncNode *entry = m_tfg->getThreadEntryNode(pf.child_tid)) {
      m_tfg->addInterThreadEdge(pf.fork_node, entry);
    }
  }

  for (const PendingCallEdge &pc : pending_calls) {
    if (!pc.callee || pc.callee->empty() || pc.callee->front().empty()) {
      continue;
    }
    SyncNode *callee_entry =
        m_tfg->getNode(&pc.callee->front().front(), pc.tid, pc.callee_ctx);
    if (!callee_entry) {
      continue;
    }
    m_tfg->addCallEdge(pc.call_node, callee_entry);

    std::vector<SyncNode *> callee_exits =
        m_tfg->getFunctionExitNodes(pc.tid, pc.callee, pc.callee_ctx);
    for (SyncNode *callee_exit : callee_exits) {
      const Instruction *exit_inst = callee_exit->getInstruction();
      if (!exit_inst) {
        continue;
      }
      if (isa<ResumeInst>(exit_inst)) {
        if (pc.unwind_ret) {
          m_tfg->addRetEdge(callee_exit, pc.unwind_ret);
        }
      } else if (pc.normal_ret) {
        m_tfg->addRetEdge(callee_exit, pc.normal_ret);
      }
    }
  }

  for (const DeferredJoin &dj : deferred_joins) {
    handleThreadJoin(dj.inst, dj.node, dj.tid);
  }
}

void MHPAnalysis::processInstruction(const Instruction * /*inst*/,
                                     ThreadID /*tid*/,
                                     SyncNode *& /*current_node*/) {
  // This method is a helper for more fine-grained processing if needed
  // Currently unused but kept for future extensibility
}

void MHPAnalysis::handleThreadFork(
    const Instruction *fork_inst, SyncNode *node, ThreadID parent_tid,
    std::vector<std::pair<const Function *, ThreadID>> *deferred_spawns) {
  // Allocate new thread ID
  ThreadID new_tid = allocateThreadID();

  const bool multi_instance =
      ((node->getCallContextID() & kMergedContextBit) != 0) ||
      (fork_inst && m_thread_multiplicity &&
       m_thread_multiplicity->instructionMayExecuteMultipleTimes(fork_inst)) ||
      isMultiInstanceThread(parent_tid);
  if (multi_instance) {
    m_multi_instance_threads.insert(new_tid);
  }

  node->setForkedThread(new_tid);

  // Track fork-join relationships
  m_thread_fork_sites[new_tid] = fork_inst;
  m_thread_parents[new_tid] = parent_tid;
  m_thread_children[parent_tid].push_back(new_tid);
  m_fork_to_thread[fork_inst].insert(new_tid);
  m_fork_node_to_thread[node] = new_tid;

  const Value *pthread_ptr = m_thread_api->getForkedThread(fork_inst);
  if (pthread_ptr) {
    m_pthread_value_to_threads[pthread_ptr].insert(new_tid);
    m_thread_to_pthread_value[new_tid] = pthread_ptr;
  }

  auto findAncestorThreadForEntry = [&](const Function *target) -> ThreadID {
    ThreadID anc = parent_tid;
    std::unordered_set<ThreadID> seen;
    while (seen.insert(anc).second) {
      if (m_tfg->getThreadEntry(anc) == target) {
        return anc;
      }
      auto pIt = m_thread_parents.find(anc);
      if (pIt == m_thread_parents.end()) {
        break;
      }
      anc = pIt->second;
    }
    return kUnknownThread;
  };

  auto launchTarget = [&](const Function *target, ThreadID target_tid) {
    if (!target || target->isDeclaration()) {
      return;
    }
    // Guard against recursive thread creation cycles (direct or indirect):
    // mark the ancestor thread multi-instance and link back to its entry
    // instead of spawning an unbounded chain of synthetic ThreadIDs.
    const ThreadID recursiveAnc = findAncestorThreadForEntry(target);
    if (recursiveAnc != kUnknownThread) {
      m_multi_instance_threads.insert(recursiveAnc);
      m_multi_instance_threads.insert(target_tid);
      if (SyncNode *entry = m_tfg->getThreadEntryNode(recursiveAnc)) {
        m_tfg->addInterThreadEdge(node, entry);
      }
      return;
    }
    m_tfg->addThread(target_tid, target);
    if (deferred_spawns) {
      deferred_spawns->emplace_back(target, target_tid);
    } else {
      processFunction(target, target_tid, 0);
      if (SyncNode *entry = m_tfg->getThreadEntryNode(target_tid)) {
        m_tfg->addInterThreadEdge(node, entry);
      }
    }
  };

  // Get the forked function (direct or statically/AA-resolved indirect target).
  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(
          forked_fun_val ? forked_fun_val->stripPointerCasts() : nullptr)) {
    launchTarget(forked_fun, new_tid);
  } else {
    concurrency::ConcurrencyContext::ResolvedCallees resolved;
    if (m_context) {
      resolved = m_context->resolveFunctionTargets(forked_fun_val);
    } else {
      concurrency::ConcurrencyContext localCtx(m_module, m_alias_analysis);
      resolved = localCtx.resolveFunctionTargets(forked_fun_val);
    }
    if (!resolved.targets.empty() &&
        (!resolved.has_unresolved || resolved.targets.size() <= 4)) {
      for (size_t idx = 0; idx < resolved.targets.size(); ++idx) {
        const Function *target = resolved.targets[idx];
        ThreadID target_tid = (idx == 0) ? new_tid : allocateThreadID();
        if (idx > 0) {
          if (multi_instance) {
            m_multi_instance_threads.insert(target_tid);
          }
          m_thread_fork_sites[target_tid] = fork_inst;
          m_thread_parents[target_tid] = parent_tid;
          m_thread_children[parent_tid].push_back(target_tid);
          m_fork_to_thread[fork_inst].insert(target_tid);
          if (pthread_ptr) {
            m_pthread_value_to_threads[pthread_ptr].insert(target_tid);
            m_thread_to_pthread_value[target_tid] = pthread_ptr;
          }
        }
        launchTarget(target, target_tid);
      }
    }
    if (resolved.targets.empty() || resolved.has_unresolved) {
      enableIndirectForkConservatism();
      m_multi_instance_threads.insert(new_tid);
    }
  }
}

void MHPAnalysis::enableIndirectForkConservatism() {
  if (m_has_unresolved_fork) {
    return;
  }
  m_has_unresolved_fork = true;

  // Distinguish modules where every fork site has a statically known target
  // (and no fork API itself has its address taken) from modules with an
  // unresolved indirect fork start routine. In the former case, an unresolved
  // indirect call can only reach the known fork sites and therefore only spawn
  // those known thread-entry targets (and their reachable callees) — avoiding
  // marking every C++ vtable method/destructor as a spurious thread entry.
  bool has_indirect_or_addr_taken_fork = false;
  std::deque<const Function *> seed_targets;
  std::unordered_set<const Function *> marked_funcs;

  auto markFunctionUnknown = [&](const Function *fn) {
    if (!fn || fn->isDeclaration() || !marked_funcs.insert(fn).second) {
      return;
    }
    m_thread_entry_candidates.insert(fn);
    for (const Instruction &inst : instructions(*fn)) {
      m_inst_to_thread[&inst] = kUnknownThread;
    }
  };

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      if (func.hasAddressTaken()) {
        const ThreadAPI::TD_TYPE ty = m_thread_api->getType(&func);
        if (ty == ThreadAPI::TD_FORK || ty == ThreadAPI::TD_OMP_TASK ||
            ty == ThreadAPI::TD_OMP_TASKLOOP) {
          has_indirect_or_addr_taken_fork = true;
        }
      }
      continue;
    }
    for (const Instruction &inst : instructions(func)) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb || !m_thread_api->isTDFork(cb)) {
        continue;
      }
      const Value *forked = m_thread_api->getForkedFun(cb);
      if (const auto *direct = dyn_cast_or_null<Function>(
              forked ? forked->stripPointerCasts() : nullptr)) {
        if (!direct->isDeclaration()) {
          seed_targets.push_back(direct);
        } else {
          has_indirect_or_addr_taken_fork = true;
        }
      } else {
        has_indirect_or_addr_taken_fork = true;
      }
    }
  }

  if (m_openmp_semantics) {
    for (const auto &task : m_openmp_semantics->getTasks()) {
      if (task && task->task_function && !task->task_function->isDeclaration()) {
        seed_targets.push_back(task->task_function);
      }
    }
  }

  if (!has_indirect_or_addr_taken_fork && !seed_targets.empty()) {
    while (!seed_targets.empty()) {
      const Function *cur = seed_targets.front();
      seed_targets.pop_front();
      if (!cur || cur->isDeclaration() || marked_funcs.count(cur)) {
        continue;
      }
      markFunctionUnknown(cur);
      for (const Instruction &inst : instructions(*cur)) {
        if (const auto *cb = dyn_cast<CallBase>(&inst)) {
          if (const Function *callee = m_thread_api->getCallee(cb)) {
            if (!callee->isDeclaration() && !marked_funcs.count(callee)) {
              seed_targets.push_back(callee);
            }
          }
        }
      }
    }
    return;
  }

  bool has_non_pthread_fork = false;
  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    for (const Instruction &inst : instructions(func)) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb || !m_thread_api->isTDFork(cb)) {
        continue;
      }
      const Function *callee = m_thread_api->getCallee(cb);
      if (!callee || callee->getName() != "pthread_create") {
        has_non_pthread_fork = true;
      }
    }
  }
  const bool pthread_only_forks =
      !has_non_pthread_fork &&
      (!m_openmp_semantics || m_openmp_semantics->getTasks().empty());

  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    if (!func.hasAddressTaken() ||
        !isLikelyThreadEntryCandidate(func, pthread_only_forks)) {
      continue;
    }
    markFunctionUnknown(&func);
  }
}

#include <deque>
#include <set>

// ============================================================================
// Value Tracing Helpers
// ============================================================================
const Value *MHPAnalysis::tracePthreadT(const Value *val) const {
  return JoinTargetAnalysis::traceThreadHandleRoot(val, &m_module);
}

void MHPAnalysis::handleThreadJoin(const Instruction *join_inst, SyncNode *node,
                                   ThreadID parent_tid) {
  if (m_thread_api->getType(dyn_cast<CallBase>(join_inst)) ==
      ThreadAPI::TD_CUDA_DEVICE_SYNC) {
    for (const auto &fork_entry : m_thread_parents) {
      ThreadID child_tid = fork_entry.first;
      if (fork_entry.second != parent_tid ||
          m_detached_threads.count(child_tid)) {
        continue;
      }
      std::vector<SyncNode *> child_exits =
          m_tfg->getThreadExitNodes(child_tid);
      for (SyncNode *child_exit : child_exits) {
        if (child_exit) {
          m_tfg->addInterThreadEdge(child_exit, node, EdgeKind::Join);
        }
      }
    }
    return;
  }

  ThreadID joined_tid = 0;
  bool found_thread = false;

  // JoinTargetAnalysis is the single authority for definite joins. Its result
  // includes path, alias, lifetime, detach, overwrite, and multiplicity
  // uncertainty; syntactic handle tracing must not bypass those checks.
  if (m_join_target_analysis) {
    const Instruction *definite_fork =
        m_join_target_analysis->getDefiniteFeasibleJoinedFork(join_inst);
    if (definite_fork) {
      ThreadID candidate = 0;
      const bool requires_exact_context =
          definite_fork->getFunction() == join_inst->getFunction();
      SyncNode *fork_node =
          m_tfg->getNode(definite_fork, parent_tid, node->getCallContextID());
      if (fork_node) {
        auto exact = m_fork_node_to_thread.find(fork_node);
        if (exact != m_fork_node_to_thread.end()) {
          candidate = exact->second;
        }
      }
      auto it = m_fork_to_thread.find(definite_fork);
      if (!candidate && !requires_exact_context &&
          it != m_fork_to_thread.end() && it->second.size() == 1) {
        candidate = *it->second.begin();
      }
      if (candidate) {
        if (!isMultiInstanceThread(candidate) &&
            !m_detached_threads.count(candidate)) {
          joined_tid = candidate;
          found_thread = true;
        }
      }
      if (!found_thread &&
          (it == m_fork_to_thread.end() || requires_exact_context)) {
        const auto duplicate = std::find_if(
            m_pending_joins.begin(), m_pending_joins.end(),
            [join_inst, node](const PendingJoin &pending) {
              return pending.inst == join_inst && pending.node == node;
            });
        if (duplicate == m_pending_joins.end()) {
          m_pending_joins.push_back({join_inst, node, parent_tid});
        }
      }
    }
  }

  if (found_thread && joined_tid != 0) {
    // We successfully identified the joined thread
    std::vector<SyncNode *> child_exits = m_tfg->getThreadExitNodes(joined_tid);
    if (!child_exits.empty()) {
      for (SyncNode *child_exit : child_exits) {
        if (child_exit) {
          m_tfg->addInterThreadEdge(child_exit, node, EdgeKind::Join);
        }
      }
      node->setJoinedThread(joined_tid);
      m_join_to_thread[join_inst] = joined_tid;
    }
  } else {
    // Ambiguous joins must not create structural non-overlap edges.
    (void)parent_tid;
  }
}

void MHPAnalysis::finalizePendingJoins() {
  std::vector<PendingJoin> pending = std::move(m_pending_joins);
  m_pending_joins.clear();
  for (const PendingJoin &join : pending) {
    handleThreadJoin(join.inst, join.node, join.parent_tid);
  }
  // A definite fork that is still unavailable after global lowering is a
  // model gap, not work for another retry cycle.
  m_pending_joins.clear();
}

void MHPAnalysis::handleThreadDetach(const Instruction *detach_inst) {
  if (resolveThreadDetach(detach_inst)) {
    return;
  }
  if (std::find(m_pending_detaches.begin(), m_pending_detaches.end(),
                detach_inst) == m_pending_detaches.end()) {
    m_pending_detaches.push_back(detach_inst);
  }
}

bool MHPAnalysis::resolveThreadDetach(const Instruction *detach_inst) {
  const auto *call = dyn_cast<CallBase>(detach_inst);
  if (!call || call->arg_size() < 1) {
    return false;
  }
  const Value *detached_thread_val = call->getArgOperand(0);
  if (!detached_thread_val) {
    return false;
  }

  std::unordered_set<const Value *> detached_roots;
  JoinTargetAnalysis::traceThreadHandleRoots(detached_thread_val, &m_module,
                                             detached_roots);
  if (detached_roots.empty()) {
    if (const Value *root = tracePthreadT(detached_thread_val)) {
      detached_roots.insert(root);
    } else {
      detached_roots.insert(detached_thread_val->stripPointerCasts());
    }
  }

  bool resolved = false;
  for (const Value *root : detached_roots) {
    auto it = m_pthread_value_to_threads.find(root);
    if (it == m_pthread_value_to_threads.end()) {
      continue;
    }
    m_detached_threads.insert(it->second.begin(), it->second.end());
    resolved = true;
  }
  return resolved;
}

void MHPAnalysis::finalizePendingDetaches() {
  std::vector<const Instruction *> pending = std::move(m_pending_detaches);
  m_pending_detaches.clear();
  for (const Instruction *detach : pending) {
    resolveThreadDetach(detach);
  }
}

void MHPAnalysis::handleLockAcquire(const Instruction *lock_inst,
                                    SyncNode *node) {
  const Value *lock = m_thread_api->getAnalysisLockIdentity(lock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleLockRelease(const Instruction *unlock_inst,
                                    SyncNode *node) {
  const Value *lock = m_thread_api->getAnalysisLockIdentity(unlock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleCondWait(const Instruction *wait_inst, SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);

  node->setCondValue(cond);
  node->setLockValue(mutex);

  // Track the wait as a region boundary only.
  m_condvar_waits[cond].push_back(node);
}

void MHPAnalysis::handleCondSignal(const Instruction *signal_inst,
                                   SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  node->setCondValue(cond);
  // Record the signal/broadcast as a region boundary only.
  m_condvar_signals[cond].push_back(node);
}

void MHPAnalysis::handleBarrier(const Instruction *barrier_inst,
                                SyncNode *node) {
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  if (!barrier) {
    barrier = barrier_inst;
  }
  node->setLockValue(barrier); // Reuse lock field for barrier value

  BarrierParticipant current;
  current.arrival = node;
  const auto *call = dyn_cast<CallBase>(barrier_inst);
  const ThreadAPI::TD_TYPE type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (type != ThreadAPI::TD_BARRIER_ARRIVE) {
    current.continuations = getBarrierContinuations(node);
  }
  std::optional<size_t> phase = getBarrierPhaseOrdinal(barrier_inst, barrier);
  if (phase) {
    m_barrier_waits[barrier][*phase].push_back(std::move(current));
  }
}

void MHPAnalysis::finalizeBarrierPhases() {
  auto phaseMayContainRepeatedParticipant =
      [this](const std::vector<BarrierParticipant> &participants) {
        for (const BarrierParticipant &participant : participants) {
          const SyncNode *arrival = participant.arrival;
          const Instruction *inst =
              arrival ? arrival->getInstruction() : nullptr;
          if (!inst || !m_thread_multiplicity) {
            continue;
          }
          if (m_thread_multiplicity->instructionMayExecuteMultipleTimes(inst)) {
            return true;
          }
        }
        return false;
      };

  for (const auto &barrier_entry : m_barrier_waits) {
    const Value *barrier_key = barrier_entry.first
                                   ? barrier_entry.first->stripPointerCasts()
                                   : nullptr;
    std::unordered_set<ThreadID> all_threads_for_barrier;
    for (const auto &phase_entry : barrier_entry.second) {
      for (const BarrierParticipant &participant : phase_entry.second) {
        if (participant.arrival) {
          all_threads_for_barrier.insert(participant.arrival->getThreadID());
        }
      }
    }
    const size_t expected_count =
        barrier_key && !m_uncertain_barrier_counts.count(barrier_key) &&
                m_barrier_expected_counts.count(barrier_key)
            ? m_barrier_expected_counts.at(barrier_key)
            : 0;
    for (const auto &phase_entry : barrier_entry.second) {
      const std::vector<BarrierParticipant> &participants = phase_entry.second;
      std::unordered_set<ThreadID> distinct_threads;
      for (const BarrierParticipant &participant : participants) {
        if (participant.arrival) {
          distinct_threads.insert(participant.arrival->getThreadID());
        }
      }

      bool phase_has_multi_instance_thread = false;
      for (ThreadID tid : distinct_threads) {
        if (isMultiInstanceThread(tid)) {
          phase_has_multi_instance_thread = true;
          break;
        }
      }
      if (!phase_has_multi_instance_thread &&
          phaseMayContainRepeatedParticipant(participants)) {
        phase_has_multi_instance_thread = true;
      }

      const bool phase_complete =
          !m_uncertain_barrier_counts.count(barrier_key) &&
          !phase_has_multi_instance_thread && !distinct_threads.empty() &&
          (expected_count != 0
               ? distinct_threads.size() == expected_count
               : !m_has_unresolved_fork &&
                     distinct_threads.size() == all_threads_for_barrier.size());
      if (!phase_complete) {
        continue;
      }
      for (size_t i = 0; i < participants.size(); ++i) {
        const BarrierParticipant &lhs = participants[i];
        if (!lhs.arrival) {
          continue;
        }
        for (size_t j = 0; j < participants.size(); ++j) {
          if (i == j) {
            continue;
          }
          const BarrierParticipant &rhs = participants[j];
          if (!rhs.arrival ||
              lhs.arrival->getThreadID() == rhs.arrival->getThreadID()) {
            continue;
          }
          for (SyncNode *cont : rhs.continuations) {
            if (cont) {
              m_tfg->addInterThreadEdge(lhs.arrival, cont, EdgeKind::Barrier);
            }
          }
        }
      }
    }
  }
}

void MHPAnalysis::analyzeLockSets() {
  errs() << "Analyzing Lock Sets...\n";
  if (m_context) {
    m_lockset = std::make_unique<LockSetAnalysis>(*m_context);
    if (m_alias_analysis) {
      m_lockset->setAliasAnalysis(m_alias_analysis);
    }
  } else {
    m_lockset = std::make_unique<LockSetAnalysis>(m_module);
    m_lockset->setAliasAnalysis(m_alias_analysis);
    m_lockset->setCallGraph(m_call_graph);
  }
  m_lockset->analyze();
}

void MHPAnalysis::ensureThreadRegionAnalysis() const {
  if (!m_region_analysis && m_tfg) {
    m_region_analysis = std::make_unique<ThreadRegionAnalysis>(*m_tfg);
    m_region_analysis->analyze();
  }
}

void MHPAnalysis::analyzeThreadRegions() {
  errs() << "Analyzing Thread Regions...\n";
  m_region_analysis = std::make_unique<ThreadRegionAnalysis>(*m_tfg);
  m_region_analysis->analyze();
  errs() << "Identified " << m_region_analysis->getAllRegions().size()
         << " regions\n";
}

void MHPAnalysis::computeMHPPairs() {
  errs() << "Computing authoritative instruction-level MHP pairs...\n";
  computeMHPPairsInstructionLevel();
}

void MHPAnalysis::computeMHPPairsInstructionLevel() {
  errs() << "Computing MHP Pairs (Instruction-Level Fallback)...\n";

  std::vector<const Instruction *> all_insts;
  all_insts.reserve(m_inst_to_thread.size());

  for (const auto &entry : m_inst_to_thread) {
    all_insts.push_back(entry.first);
  }

  size_t num_pairs = 0;
  for (size_t i = 0; i < all_insts.size(); ++i) {
    for (size_t j = i; j < all_insts.size(); ++j) {
      const Instruction *i1 = all_insts[i];
      const Instruction *i2 = all_insts[j];
      if (mayHappenInParallel(i1, i2)) {
        // Store in canonical (pointer-sorted) order for O(1) symmetric lookup.
        const Instruction *ca = std::less<const Instruction *>{}(i1, i2) ? i1 : i2;
        const Instruction *cb = ca == i1 ? i2 : i1;
        m_mhp_pairs.insert({ca, cb});
        num_pairs++;
      }
    }
  }

  errs() << "Found " << num_pairs << " MHP pairs\n";
}

bool MHPAnalysis::mayHappenInParallel(const Instruction *i1,
                                      const Instruction *i2) const {
  if (!i1 || !i2) {
    return true;
  }
  if (m_single_main_thread_only) {
    return false;
  }
  // Symmetric memoization (keyed by pointer identity, order-independent).
  const Instruction *a = std::less<const Instruction *>{}(i1, i2) ? i1 : i2;
  const Instruction *b = a == i1 ? i2 : i1;
  auto cacheResult = [this, a, b](bool value) {
    if (a && b && m_mhp_cache.size() < kMaxPairCacheEntries) {
      m_mhp_cache.emplace(InstPair{a, b}, value);
    }
    return value;
  };
  if (a && b) {
    auto it = m_mhp_cache.find({a, b});
    if (it != m_mhp_cache.end())
      return it->second;
  }

  if (i1 == i2) {
    ThreadID tid = getThreadID(i1);
    bool parallel_self =
        tid == kUnknownThread || multiInstanceThreadMayOverlap(tid);
    return cacheResult(parallel_self);
  }

  // Same single-instance thread cannot overlap itself.
  if (isInSameThread(i1, i2))
    return cacheResult(false);

  // Initialization phase in main is single-threaded: instructions observed
  // before the first reachable thread/task creation in thread 0 cannot race
  // with child-thread nodes.
  ThreadID t1 = getThreadID(i1);
  ThreadID t2 = getThreadID(i2);
  if (t1 == t2 && t1 != kUnknownThread && multiInstanceThreadMayOverlap(t1)) {
    return cacheResult(true);
  }
  if (!m_openmp_task_exclusions.empty() &&
      m_openmp_task_exclusions.count(normalizeThreadPair(t1, t2))) {
    return cacheResult(false);
  }
  if (!m_pre_fork_main_nodes.empty()) {
    const bool i1PreForkMain = (t1 == 0) && isAlwaysPreForkMain(i1);
    const bool i2PreForkMain = (t2 == 0) && isAlwaysPreForkMain(i2);
    if ((i1PreForkMain && t2 != 0 && t2 != kUnknownThread) ||
        (i2PreForkMain && t1 != 0 && t1 != kUnknownThread))
      return cacheResult(false);
  }

  // Fast path: check precomputed MHP pairs
  if (!m_mhp_pairs.empty() && isPrecomputedMHP(i1, i2))
    return cacheResult(true);

  bool r = !hasStructuralOrderRelation(i1, i2) &&
           !hasStructuralOrderRelation(i2, i1);
  return cacheResult(r);
}

bool MHPAnalysis::isPrecomputedMHP(const Instruction *i1,
                                   const Instruction *i2) const {
  if (m_mhp_pairs.empty()) {
    return false;
  }
  // Pairs are stored in canonical (pointer-sorted) order so a single probe
  // suffices for a symmetric lookup.
  const Instruction *a = std::less<const Instruction *>{}(i1, i2) ? i1 : i2;
  const Instruction *b = a == i1 ? i2 : i1;
  return m_mhp_pairs.count({a, b}) != 0;
}

InstructionSet
MHPAnalysis::getParallelInstructions(const Instruction *inst) const {
  InstructionSet result;
  if (m_precompute_mhp_pairs) {
    for (const auto &pair : m_mhp_pairs) {
      if (pair.a == inst) {
        result.insert(pair.b);
      } else if (pair.b == inst) {
        result.insert(pair.a);
      }
    }
  } else {
    for (const auto &entry : m_inst_to_thread) {
      if (mayHappenInParallel(inst, entry.first)) {
        result.insert(entry.first);
      }
    }
  }

  return result;
}

bool MHPAnalysis::mustBeSequential(const Instruction *i1,
                                   const Instruction *i2) const {
  if (!i1 || !i2) {
    return false;
  }
  return !mayHappenInParallel(i1, i2);
}

ThreadID MHPAnalysis::getThreadID(const Instruction *inst) const {
  auto it = m_inst_to_thread.find(inst);
  return it != m_inst_to_thread.end() ? it->second : kUnknownThread;
}

InstructionSet MHPAnalysis::getInstructionsInThread(ThreadID tid) const {
  InstructionSet result;
  for (const auto &pair : m_inst_to_thread) {
    if (pair.second == tid) {
      result.insert(pair.first);
    }
  }
  return result;
}

std::set<LockID> MHPAnalysis::getLocksHeldAt(const Instruction *inst) const {
  // LockSet analysis is optional - only available if enabled
  if (m_lockset) {
    return m_lockset->getMayLockSetAt(inst);
  }
  // If lockset analysis not run, return empty set
  return std::set<LockID>();
}

ThreadID MHPAnalysis::allocateThreadID() { return m_next_thread_id++; }

void MHPAnalysis::mapInstructionToThread(const Instruction *inst,
                                         ThreadID tid) {
  if (!inst) {
    return;
  }

  if (m_has_unresolved_fork &&
      m_thread_entry_candidates.find(inst->getFunction()) !=
          m_thread_entry_candidates.end()) {
    m_inst_to_thread[inst] = kUnknownThread;
    return;
  }

  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    m_inst_to_thread[inst] = tid;
    return;
  }

  if (it->second == kUnknownThread || it->second == tid) {
    return;
  }

  it->second = kUnknownThread;
}

bool MHPAnalysis::isInstructionThreadAmbiguous(const Instruction *inst) const {
  if (!inst) {
    return true;
  }
  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    return true;
  }
  return it->second == kUnknownThread;
}

bool MHPAnalysis::isMainThreadSpawnNode(const SyncNode *node) const {
  if (!node || !m_tfg || node->getThreadID() != 0) {
    return false;
  }

  for (SyncNode *succ : node->getSuccessors()) {
    if (succ->getThreadID() == 0) {
      continue;
    }
    if (m_tfg->hasEdgeKind(node, succ, EdgeKind::Create)) {
      return true;
    }
  }

  return false;
}

void MHPAnalysis::recomputePreForkMainNodes() {
  m_pre_fork_main_nodes.clear();
  if (!m_tfg || m_has_unresolved_fork || m_tfg->getAllThreads().size() <= 1) {
    return;
  }

  auto collectReachableInMainThread =
      [](const std::vector<SyncNode *> &starts) {
        std::deque<const SyncNode *> worklist;
        std::unordered_set<const SyncNode *> visited;
        for (SyncNode *start : starts) {
          if (start && visited.insert(start).second) {
            worklist.push_back(start);
          }
        }

        while (!worklist.empty()) {
          const SyncNode *current = worklist.front();
          worklist.pop_front();
          for (SyncNode *succ : current->getSuccessors()) {
            if (succ->getThreadID() != 0) {
              continue;
            }
            if (visited.insert(succ).second) {
              worklist.push_back(succ);
            }
          }
        }
        return visited;
      };

  SyncNode *main_entry = m_tfg->getThreadEntryNode(0);
  if (!main_entry) {
    return;
  }

  std::vector<SyncNode *> thread_zero_nodes = m_tfg->getNodesInThread(0);
  std::vector<SyncNode *> spawn_nodes;
  spawn_nodes.reserve(thread_zero_nodes.size());
  for (SyncNode *node : thread_zero_nodes) {
    if (isMainThreadSpawnNode(node)) {
      spawn_nodes.push_back(node);
    }
  }
  const auto reachable_from_main =
      collectReachableInMainThread(std::vector<SyncNode *>{main_entry});
  const auto reachable_from_spawn = collectReachableInMainThread(spawn_nodes);
  for (SyncNode *node : thread_zero_nodes) {
    if (!reachable_from_main.count(node)) {
      continue;
    }
    if (!reachable_from_spawn.count(node)) {
      m_pre_fork_main_nodes.insert(node);
    }
  }
}

bool MHPAnalysis::isAlwaysPreForkMain(const Instruction *inst) const {
  if (!inst || !m_tfg) {
    return false;
  }

  std::vector<SyncNode *> nodes = m_tfg->getNodes(inst, 0);
  if (nodes.empty()) {
    return false;
  }
  for (SyncNode *node : nodes) {
    if (!m_pre_fork_main_nodes.count(node)) {
      return false;
    }
  }
  return true;
}

bool MHPAnalysis::hasStructuralOrderRelation(const Instruction *i1,
                                             const Instruction *i2) const {
  if (!i1 || !i2 || i1 == i2) {
    return false;
  }
  const ThreadID t1 = getThreadID(i1);
  const ThreadID t2 = getThreadID(i2);
  if (t1 == kUnknownThread || t2 == kUnknownThread) {
    return false;
  }

  auto it = m_order_cache.find({i1, i2});
  if (it != m_order_cache.end())
    return it->second;

  auto cacheOrder = [this, i1, i2](bool value) {
    if (m_order_cache.size() < kMaxPairCacheEntries) {
      m_order_cache.emplace(InstPair{i1, i2}, value);
    }
    return value;
  };

  if (m_tfg->hasReachabilityIndex() && !m_tfg->canThreadReach(t1, t2)) {
    return cacheOrder(false);
  }

  std::vector<SyncNode *> start_nodes = m_tfg->getNodes(i1, t1);
  std::vector<SyncNode *> end_nodes = m_tfg->getNodes(i2, t2);

  if (start_nodes.empty() || end_nodes.empty()) {
    return cacheOrder(false);
  }

  constexpr uint32_t kRestrictedMask =
      (uint32_t{1} << static_cast<unsigned>(EdgeKind::Join)) |
      (uint32_t{1} << static_cast<unsigned>(EdgeKind::Barrier)) |
      (uint32_t{1} << static_cast<unsigned>(EdgeKind::TaskDepend)) |
      (uint32_t{1} << static_cast<unsigned>(EdgeKind::TaskWait)) |
      (uint32_t{1} << static_cast<unsigned>(EdgeKind::TaskCompletion));

  const bool has_index = m_tfg->hasReachabilityIndex();
  const size_t max_id = m_tfg->getMaxNodeID();

  auto can_reach = [this, has_index, max_id](
                       SyncNode *start, SyncNode *end) -> bool {
    if (!start || !end) {
      return false;
    }
    if (start == end) {
      return true;
    }

    const ThreadID end_tid = end->getThreadID();
    int end_topo = -1;
    size_t end_scc = 0;
    if (has_index) {
      if (!m_tfg->canNodeReachThread(start, end_tid)) {
        return false;
      }
      const int start_topo = m_tfg->getGlobalTopologicalOrder(start);
      end_topo = m_tfg->getGlobalTopologicalOrder(end);
      end_scc = m_tfg->getGlobalSCCID(end);
      if (start_topo >= 0 && end_topo >= 0 && start_topo > end_topo &&
          m_tfg->getGlobalSCCID(start) != end_scc) {
        return false;
      }
    }

    if (m_order_visit_stamp.size() <= max_id) {
      m_order_visit_stamp.resize(max_id + 1, 0);
    }
    ++m_order_current_stamp;
    if (m_order_current_stamp == 0) {
      std::fill(m_order_visit_stamp.begin(), m_order_visit_stamp.end(), 0);
      m_order_current_stamp = 1;
    }
    const uint32_t stamp = m_order_current_stamp;

    m_order_bfs_queue.clear();
    m_order_visit_stamp[start->getNodeID()] = stamp;
    m_order_bfs_queue.push_back(start);

    size_t head = 0;
    while (head < m_order_bfs_queue.size()) {
      SyncNode *current = m_order_bfs_queue[head++];
      const bool has_restricted =
          (current->getOutgoingEdgeKindMask() & kRestrictedMask) != 0;
      for (SyncNode *succ : current->getSuccessors()) {
        if (has_restricted) {
          const uint32_t edge_mask =
              current->getEdgeMaskTo(succ) & kRestrictedMask;
          if (edge_mask != 0) {
            bool usable_edge = true;
            for (EdgeKind kind :
                 {EdgeKind::Join, EdgeKind::Barrier, EdgeKind::TaskDepend,
                  EdgeKind::TaskWait, EdgeKind::TaskCompletion}) {
              if ((edge_mask &
                   (uint32_t{1} << static_cast<unsigned>(kind))) != 0 &&
                  !synchronizationEdgeMustOrderTarget(current, succ, end,
                                                      kind)) {
                usable_edge = false;
                break;
              }
            }
            if (!usable_edge) {
              continue;
            }
          }
        }
        if (succ == end) {
          return true;
        }
        const Instruction *succ_inst = succ->getInstruction();
        if (succ_inst && isInstructionThreadAmbiguous(succ_inst)) {
          continue;
        }
        if (has_index) {
          if (!m_tfg->canNodeReachThread(succ, end_tid)) {
            continue;
          }
          if (end_topo >= 0) {
            const int succ_topo = m_tfg->getGlobalTopologicalOrder(succ);
            if (succ_topo > end_topo &&
                m_tfg->getGlobalSCCID(succ) != end_scc) {
              continue;
            }
          }
        }
        const size_t succ_id = succ->getNodeID();
        if (m_order_visit_stamp[succ_id] != stamp) {
          m_order_visit_stamp[succ_id] = stamp;
          m_order_bfs_queue.push_back(succ);
        }
      }
    }
    return false;
  };

  for (SyncNode *start_node : start_nodes) {
    for (SyncNode *end_node : end_nodes) {
      if (!can_reach(start_node, end_node)) {
        return cacheOrder(false);
      }
    }
  }
  return cacheOrder(true);
}

bool MHPAnalysis::joinEdgeMustOrderTarget(const SyncNode *join_node,
                                          const SyncNode *target_node) const {
  if (!join_node || !target_node ||
      join_node->getThreadID() != target_node->getThreadID()) {
    return false;
  }
  const Instruction *join_inst = join_node->getInstruction();
  const Instruction *target_inst = target_node->getInstruction();
  if (!join_inst || !target_inst) {
    return false;
  }

  const Instruction *current_inst = join_inst;
  CallContextID current_ctx = join_node->getCallContextID();
  while (current_inst) {
    const Function *current_func = current_inst->getFunction();
    const DominatorTree &DT = getDomTree(current_func);
    if (current_func == target_inst->getFunction() &&
        current_ctx == target_node->getCallContextID()) {
      return DT.dominates(current_inst, target_inst);
    }

    bool dominates_all_exits = false;
    for (const BasicBlock &bb : *current_func) {
      const Instruction *terminator = bb.getTerminator();
      if (!isa<ReturnInst>(terminator) && !isa<ResumeInst>(terminator)) {
        continue;
      }
      if (!DT.dominates(current_inst, terminator)) {
        return false;
      }
      dominates_all_exits = true;
    }
    if (!dominates_all_exits || current_ctx == 0 ||
        (current_ctx & kMergedContextBit) != 0) {
      return false;
    }

    const SyncNode *caller = m_tfg->getNodeByID(current_ctx);
    if (caller && caller->getThreadID() != join_node->getThreadID()) {
      caller = nullptr;
    }
    if (!caller || !caller->getInstruction()) {
      return false;
    }
    current_inst = caller->getInstruction();
    current_ctx = caller->getCallContextID();
  }
  return false;
}

bool MHPAnalysis::synchronizationEdgeMustOrderTarget(
    const SyncNode *from, const SyncNode *to, const SyncNode *target_node,
    EdgeKind kind) const {
  if (kind == EdgeKind::Join || kind == EdgeKind::TaskWait ||
      kind == EdgeKind::TaskCompletion) {
    return joinEdgeMustOrderTarget(to, target_node);
  }
  if (kind == EdgeKind::Barrier) {
    for (const auto &barrier : m_barrier_waits) {
      for (const auto &phase : barrier.second) {
        for (const BarrierParticipant &participant : phase.second) {
          if (std::find(participant.continuations.begin(),
                        participant.continuations.end(),
                        to) != participant.continuations.end()) {
            return joinEdgeMustOrderTarget(participant.arrival, target_node);
          }
        }
      }
    }
    return false;
  }
  if (kind == EdgeKind::TaskDepend) {
    return from && to;
  }
  return false;
}

bool MHPAnalysis::isInSameThread(const Instruction *i1,
                                 const Instruction *i2) const {
  ThreadID t1 = getThreadID(i1);
  ThreadID t2 = getThreadID(i2);

  if (t1 == kUnknownThread || t2 == kUnknownThread) {
    return false;
  }

  if (t1 != t2) {
    return false;
  }

  // If they are in the same thread, we must check if this thread
  // can have multiple active instances (e.g., created in a loop).
  // If so, two instructions from the "same" static thread can run in parallel.
  if (m_multi_instance_threads.count(t1)) {
    return !multiInstanceThreadMayOverlap(t1);
  }

  return true;
}

// ============================================================================
// Fork-Join Helper Methods
// ============================================================================

bool MHPAnalysis::isAncestorThread(ThreadID ancestor,
                                   ThreadID descendant) const {
  ThreadID current = descendant;
  while (m_thread_parents.find(current) != m_thread_parents.end()) {
    ThreadID parent = m_thread_parents.at(current);
    if (parent == ancestor) {
      return true;
    }
    current = parent;
  }
  return false;
}

bool MHPAnalysis::isForkSite(const Instruction *inst) const {
  return m_fork_to_thread.find(inst) != m_fork_to_thread.end();
}

bool MHPAnalysis::isJoinSite(const Instruction *inst) const {
  return m_join_to_thread.find(inst) != m_join_to_thread.end();
}

ThreadID MHPAnalysis::getForkedThreadID(const Instruction *fork_inst) const {
  auto it = m_fork_to_thread.find(fork_inst);
  return it != m_fork_to_thread.end() && it->second.size() == 1
             ? *it->second.begin()
             : 0;
}

ThreadID MHPAnalysis::getJoinedThreadID(const Instruction *join_inst) const {
  auto it = m_join_to_thread.find(join_inst);
  return it != m_join_to_thread.end() ? it->second : 0;
}

bool MHPAnalysis::isMultiInstanceThread(ThreadID tid) const {
  return m_multi_instance_threads.count(tid) != 0;
}

bool MHPAnalysis::multiInstanceThreadMayOverlap(ThreadID tid) const {
  if (!isMultiInstanceThread(tid)) {
    return false;
  }
  auto cached = m_multi_instance_overlap_cache.find(tid);
  if (cached != m_multi_instance_overlap_cache.end()) {
    return cached->second;
  }

  auto fork_it = m_thread_fork_sites.find(tid);
  const Instruction *fork_inst =
      fork_it != m_thread_fork_sites.end() ? fork_it->second : nullptr;
  if (!fork_inst) {
    return (m_multi_instance_overlap_cache[tid] = true);
  }

  const Function *parent = fork_inst->getFunction();
  const DominatorTree &DT = getDomTree(parent);
  LoopInfo loops(const_cast<DominatorTree &>(DT));
  Loop *fork_loop = loops.getLoopFor(fork_inst->getParent());
  if (!fork_loop) {
    return (m_multi_instance_overlap_cache[tid] = true);
  }

  for (const Instruction &inst : instructions(parent)) {
    if (!m_thread_api->isTDJoin(&inst) ||
        !fork_loop->contains(inst.getParent()) ||
        !DT.dominates(fork_inst, &inst)) {
      continue;
    }

    const Instruction *joined_fork =
        m_join_target_analysis
            ? m_join_target_analysis->getDefiniteFeasibleJoinedFork(&inst)
            : nullptr;
    bool only_repeated_site_ambiguity = false;
    if (!joined_fork && m_join_target_analysis) {
      const JoinResolution resolution =
          m_join_target_analysis->getJoinResolution(&inst);
      only_repeated_site_ambiguity =
          !resolution.has_unknown_live_instance &&
          resolution.feasible_forks.size() == 1 &&
          resolution.feasible_forks.front() == fork_inst &&
          std::all_of(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      [](JoinAmbiguityReason reason) {
                        return reason == JoinAmbiguityReason::RepeatedForkSite;
                      });
    }
    if (joined_fork != fork_inst && !only_repeated_site_ambiguity) {
      continue;
    }

    bool orders_every_backedge = true;
    SmallVector<BasicBlock *, 4> latches;
    fork_loop->getLoopLatches(latches);
    for (BasicBlock *latch : latches) {
      if (!DT.dominates(&inst, latch->getTerminator())) {
        orders_every_backedge = false;
        break;
      }
    }
    if (orders_every_backedge) {
      return (m_multi_instance_overlap_cache[tid] = false);
    }
  }

  return (m_multi_instance_overlap_cache[tid] = true);
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void MHPAnalysis::Statistics::print(raw_ostream &os) const {
  os << "MHP Analysis Statistics:\n";
  os << "========================\n";
  os << "Threads:          " << num_threads << "\n";
  os << "Forks:            " << num_forks << "\n";
  os << "Joins:            " << num_joins << "\n";
  os << "Locks:            " << num_locks << "\n";
  os << "Unlocks:          " << num_unlocks << "\n";
  os << "Regions:          " << num_regions << "\n";
  os << "MHP Pairs:        " << num_mhp_pairs << "\n";
  os << "Ordered Pairs:    " << num_ordered_pairs << "\n";
}

MHPAnalysis::Statistics MHPAnalysis::getStatistics() const {
  Statistics stats{};

  if (m_tfg) {
    stats.num_threads = m_tfg->getAllThreads().size();
    stats.num_forks = m_tfg->getNodesOfType(SyncNodeType::THREAD_FORK).size();
    stats.num_joins = m_tfg->getNodesOfType(SyncNodeType::THREAD_JOIN).size();
    stats.num_locks = m_tfg->getNodesOfType(SyncNodeType::LOCK_ACQUIRE).size();
    stats.num_unlocks =
        m_tfg->getNodesOfType(SyncNodeType::LOCK_RELEASE).size();
  }

  if (m_region_analysis) {
    stats.num_regions = m_region_analysis->getAllRegions().size();
  }

  stats.num_mhp_pairs = m_mhp_pairs.size();

  return stats;
}

void MHPAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void MHPAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== MHP Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== Thread Flow Graph ===\n";
  if (m_tfg) {
    m_tfg->print(os);
  }

  os << "\n=== Thread Region Analysis ===\n";
  if (m_region_analysis) {
    m_region_analysis->print(os);
  }

  // Optional: Lock Set Analysis (only if enabled)
  if (m_lockset) {
    os << "\n=== Lock Set Analysis ===\n";
    m_lockset->print(os);
  }

  os << "\n=== MHP Pairs (sample) ===\n";
  if (m_instruction_ordinals.empty() && !m_mhp_pairs.empty()) {
    auto &ordinals = const_cast<std::unordered_map<const Instruction *, size_t> &>(
        m_instruction_ordinals);
    size_t next_ordinal = 0;
    for (const Function &func : m_module) {
      for (const Instruction &inst : instructions(func)) {
        ordinals.emplace(&inst, next_ordinal++);
      }
    }
  }
  std::vector<InstPair> ordered_pairs(m_mhp_pairs.begin(), m_mhp_pairs.end());
  std::sort(ordered_pairs.begin(), ordered_pairs.end(),
            [this](const InstPair &lhs, const InstPair &rhs) {
              if (lhs.a != rhs.a) {
                return instructionCanonicalLess(lhs.a, rhs.a);
              }
              return instructionCanonicalLess(lhs.b, rhs.b);
            });
  size_t count = 0;
  for (const auto &pair : ordered_pairs) {
    os << "MHP: ";
    pair.a->print(os);
    os << " ||| ";
    pair.b->print(os);
    os << "\n";

    if (++count >= 20) {
      if (ordered_pairs.size() > count) {
        os << "... (" << (ordered_pairs.size() - count) << " more pairs)\n";
      }
      break;
    }
  }
}

void MHPAnalysis::dumpThreadFlowGraph(const std::string &filename) const {
  if (m_tfg) {
    m_tfg->dumpToFile(filename);
    errs() << "Thread flow graph dumped to " << filename << "\n";
  }
}

void MHPAnalysis::dumpMHPMatrix(raw_ostream &os) const {
  os << "MHP Matrix:\n";
  os << "===========\n";
  // Matrix visualization would go here
  // This is a placeholder for a more sophisticated visualization
}

// =========================================================================
// Dominator helpers
// =========================================================================

const DominatorTree &MHPAnalysis::getDomTree(const Function *func) const {
  auto it = m_dom_cache.find(func);
  if (it != m_dom_cache.end()) {
    return *(it->second);
  }
  auto DT = std::make_unique<DominatorTree>();
  DT->recalculate(*const_cast<Function *>(func));
  auto *dtPtr = DT.get();
  m_dom_cache[func] = std::move(DT);
  return *dtPtr;
}

// =========================================================================
// Program Order Helpers (Precise Happens-Before for Same Thread)
// =========================================================================

std::vector<SyncNode *>
MHPAnalysis::getBarrierContinuations(const SyncNode *arrival) const {
  std::vector<SyncNode *> continuations;
  if (!arrival || !arrival->getInstruction()) {
    return continuations;
  }
  const Instruction *barrier_inst = arrival->getInstruction();
  const ThreadID tid = arrival->getThreadID();
  const CallContextID ctx = arrival->getCallContextID();

  bool stopped_at_next_wait = false;
  if (const Instruction *next = barrier_inst->getNextNode()) {
    if (m_thread_api->isTDBarWait(next)) {
      stopped_at_next_wait = true;
    } else {
      if (SyncNode *next_node = m_tfg->getNode(next, tid, ctx)) {
        continuations.push_back(next_node);
      }
    }
  }

  if (barrier_inst->isTerminator()) {
    for (const BasicBlock *succ : successors(barrier_inst->getParent())) {
      if (succ->empty()) {
        continue;
      }
      if (m_thread_api->isTDBarWait(&succ->front())) {
        stopped_at_next_wait = true;
        continue;
      }
      if (SyncNode *succ_node = m_tfg->getNode(&succ->front(), tid, ctx)) {
        continuations.push_back(succ_node);
      }
    }
  }

  if (continuations.empty() && !stopped_at_next_wait) {
    continuations.push_back(const_cast<SyncNode *>(arrival));
  }

  return continuations;
}

std::optional<size_t>
MHPAnalysis::getBarrierPhaseOrdinal(const Instruction *barrier_inst,
                                    const Value *barrier) const {
  if (!barrier_inst || !barrier_inst->getFunction()) {
    return std::nullopt;
  }

  const Function *func = barrier_inst->getFunction();
  struct PhaseState {
    size_t count = 0;
    bool pending_split = false;

    bool operator==(const PhaseState &other) const {
      return count == other.count && pending_split == other.pending_split;
    }
  };
  struct PhaseStateHash {
    size_t operator()(const PhaseState &state) const {
      return std::hash<size_t>{}(state.count) ^
             (std::hash<bool>{}(state.pending_split) << 1);
    }
  };
  constexpr size_t kAmbiguous = std::numeric_limits<size_t>::max();
  std::unordered_map<const BasicBlock *,
                     std::unordered_set<PhaseState, PhaseStateHash>>
      in_states;
  std::deque<const BasicBlock *> worklist;
  in_states[&func->getEntryBlock()].insert({0, false});
  worklist.push_back(&func->getEntryBlock());

  auto transferBlock = [&](const BasicBlock *bb, PhaseState state,
                           std::optional<size_t> &target_phase) {
    for (const Instruction &inst : *bb) {
      if (&inst == barrier_inst) {
        target_phase = state.count;
      }
      if (!isa<CallBase>(inst) || !m_thread_api->isTDBarWait(&inst)) {
        continue;
      }
      const Value *candidate = m_thread_api->getBarrierVal(&inst);
      if (!candidate ||
          candidate->stripPointerCasts() !=
              (barrier ? barrier->stripPointerCasts() : nullptr)) {
        continue;
      }
      const auto type = m_thread_api->getType(cast<CallBase>(&inst));
      if (type == ThreadAPI::TD_BARRIER_ARRIVE) {
        state.pending_split = true;
      } else if (type == ThreadAPI::TD_BARRIER_WAIT_CPP20 &&
                 state.pending_split) {
        state.pending_split = false;
        ++state.count;
      } else {
        state.pending_split = false;
        ++state.count;
      }
    }
    return state;
  };

  std::unordered_set<size_t> phases;
  size_t updates = 0;
  const size_t update_limit = std::max<size_t>(32, func->size() * 8);
  while (!worklist.empty() && updates++ < update_limit) {
    const BasicBlock *bb = worklist.front();
    worklist.pop_front();
    std::unordered_set<PhaseState, PhaseStateHash> outputs;
    for (PhaseState input : in_states[bb]) {
      if (input.count == kAmbiguous) {
        outputs.insert({kAmbiguous, false});
        continue;
      }
      std::optional<size_t> target_phase;
      PhaseState output = transferBlock(bb, input, target_phase);
      if (target_phase) {
        phases.insert(*target_phase);
      }
      outputs.insert(output);
    }
    if (outputs.size() > 4) {
      outputs.clear();
      outputs.insert({kAmbiguous, false});
    }
    for (const BasicBlock *succ : successors(bb)) {
      bool changed = false;
      for (PhaseState output : outputs) {
        changed |= in_states[succ].insert(output).second;
      }
      if (in_states[succ].size() > 4) {
        in_states[succ].clear();
        changed = in_states[succ].insert({kAmbiguous, false}).second || changed;
      }
      if (changed) {
        worklist.push_back(succ);
      }
    }
  }

  if (updates >= update_limit || phases.size() != 1 ||
      phases.count(kAmbiguous)) {
    return std::nullopt;
  }
  return *phases.begin();
}
