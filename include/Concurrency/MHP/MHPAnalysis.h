/**
 * @file MHPAnalysis.h
 * @brief Production-ready May-Happen-in-Parallel (MHP) Analysis
 *
 * This file provides a comprehensive MHP analysis framework for determining
 * which program statements may execute concurrently in a multithreaded program.
 *
 * Key Features:
 * - Thread-flow graph construction
 * - Fork-join analysis
 * - Lock-based synchronization analysis
 * - Condition variable analysis
 * - Barrier synchronization support
 * - Efficient query interface
 * - Comprehensive debugging support
 *
 * @author rainoftime
 * @date 2025
 */

#pragma once

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/API/CppAtomics.h"
#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/Core/ConcurrencyContext.h"
#include "Concurrency/Core/ThreadFlowGraph.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/MHP/ThreadRegionAnalysis.h"
#include "Concurrency/OpenMP/OpenMPSemantics.h"
#include "Concurrency/Thread/Join/JoinTargetAnalysis.h"
#include "Concurrency/Thread/ThreadMultiplicity.h"

namespace ThreadLocal {
class ThreadLocalAnalysis;
} // namespace ThreadLocal

#include <bitset>
#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace mhp {

// ============================================================================
// Forward Declarations
// ============================================================================

class ThreadFlowGraph;
class SyncNode;
class MHPAnalysis;
class ThreadRegionAnalysis;

// ============================================================================
// Type Definitions
// ============================================================================

using InstructionVector = std::vector<const llvm::Instruction *>;
using LockID = const llvm::Value *;

// ============================================================================
// MHP Analysis
// ============================================================================

/**
 * @brief Main May-Happen-in-Parallel analysis
 *
 * Computes which pairs of program statements may execute concurrently in a
 * multithreaded program. Takes into account:
 * - Thread creation and termination (fork-join)
 * - Lock-based synchronization
 * - Condition variables
 * - Barriers
 *
 * Usage:
 *   MHPAnalysis mhp(module);
 *   mhp.analyze();
 *   if (mhp.mayHappenInParallel(inst1, inst2)) {
 *     // inst1 and inst2 may execute concurrently
 *   }
 */
class MHPAnalysis : public IMHPAnalysis {
public:
  explicit MHPAnalysis(llvm::Module &module,
                       lotus::AliasAnalysisWrapper *aliasAnalysis = nullptr);
  explicit MHPAnalysis(concurrency::ConcurrencyContext &context);
  ~MHPAnalysis();

  // Main analysis entry point
  void analyze() override;

  // Alias analysis used internally (lazily initialized on demand if not
  // supplied by a ConcurrencyContext or caller).
  lotus::AliasAnalysisWrapper *getAliasAnalysis() const override;

  const OpenMP::OpenMPSemantics *getOpenMPSemantics() const override {
    return m_openmp_semantics.get();
  }

  // ========================================================================
  // Query Interface
  // ========================================================================

  /**
   * @brief Check if two instructions may execute in parallel
   *
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 and i2 may execute concurrently
   */
  bool mayHappenInParallel(const llvm::Instruction *i1,
                           const llvm::Instruction *i2) const override;

  /**
   * @brief Check if a pair is in the precomputed MHP set
   *
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if the pair was precomputed as MHP
   */
  bool isPrecomputedMHP(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const override;

  /**
   * @brief Get all instructions that may run in parallel with the given one
   *
   * @param inst Target instruction
   * @return Set of instructions that may execute concurrently with inst
   */
  InstructionSet
  getParallelInstructions(const llvm::Instruction *inst) const override;

  /**
   * @brief Check if two instructions must execute sequentially
   *
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 and i2 cannot execute concurrently
   */
  bool mustBeSequential(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const override;

  /**
   * @brief Get the thread ID that an instruction belongs to
   *
   * @param inst Target instruction
   * @return Thread ID, or kUnknownThread if the instruction is thread-ambiguous
   */
  ThreadID getThreadID(const llvm::Instruction *inst) const override;

  /**
   * @brief Get all instructions in a specific thread
   *
   * @param tid Thread ID
   * @return Set of instructions in the thread
   */
  InstructionSet getInstructionsInThread(ThreadID tid) const override;

  size_t getMhpPairCount() const override { return m_mhp_pairs.size(); }

  /**
   * @brief Get locks held at a specific instruction
   *
   * @param inst Target instruction
   * @return Set of locks that may be held at inst
   */
  std::set<LockID> getLocksHeldAt(const llvm::Instruction *inst) const;

  // ========================================================================
  // Statistics and Debugging
  // ========================================================================

  struct Statistics {
    size_t num_threads;
    size_t num_forks;
    size_t num_joins;
    size_t num_locks;
    size_t num_unlocks;
    size_t num_regions;
    size_t num_mhp_pairs;
    size_t num_ordered_pairs;

    void print(llvm::raw_ostream &os) const;
  };

  Statistics getStatistics() const;
  void printStatistics(llvm::raw_ostream &os) const override;
  void printResults(llvm::raw_ostream &os) const override;

  // Component access for advanced users
  const ThreadFlowGraph &getThreadFlowGraph() const override { return *m_tfg; }
  size_t getAnalysisGeneration() const override { return m_analysis_generation; }
  bool joinEdgeMustOrderTarget(const SyncNode *join_node,
                               const SyncNode *target_node) const override;
  bool instructionMayExecuteMultipleTimes(
      const llvm::Instruction *inst) const override {
    return !m_thread_multiplicity ||
           m_thread_multiplicity->instructionMayExecuteMultipleTimes(inst);
  }
  bool isDetachedThread(ThreadID tid) const {
    return m_detached_threads.count(tid) != 0;
  }
  bool synchronizationEdgeMustOrderTarget(const SyncNode *from,
                                          const SyncNode *to,
                                          const SyncNode *target_node,
                                          EdgeKind kind) const;
  const ThreadRegionAnalysis &getThreadRegionAnalysis() const {
    ensureThreadRegionAnalysis();
    return *m_region_analysis;
  }

  // Optional: LockSetAnalysis for more precise race detection
  LockSetAnalysis *getLockSetAnalysis() const { return m_lockset.get(); }
  void enableLockSetAnalysis();
  bool isSingleMainThreadOnly() const { return m_single_main_thread_only; }
  bool isSingleThreaded() const override { return m_single_main_thread_only; }

  // Thread-local storage analysis
  // Optional: precompute and store the full MHP pair relation.
  // Warning: can be O(N^2) in number of instructions.
  void enableMHPPrecomputation(bool enable = true) {
    m_precompute_mhp_pairs = enable;
  }

  // Visualization
  void dumpThreadFlowGraph(const std::string &filename) const;
  void dumpMHPMatrix(llvm::raw_ostream &os) const;

private:
  struct InstPair {
    const llvm::Instruction *a;
    const llvm::Instruction *b;
    bool operator==(const InstPair &o) const { return a == o.a && b == o.b; }
  };
  struct InstPairHash {
    size_t operator()(const InstPair &p) const {
      size_t h1 = std::hash<const void *>{}(p.a);
      size_t h2 = std::hash<const void *>{}(p.b);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };
  struct ThreadPairHash {
    size_t operator()(const std::pair<ThreadID, ThreadID> &p) const {
      return std::hash<ThreadID>()(p.first) ^
             (std::hash<ThreadID>()(p.second) << 1U);
    }
  };

  bool instructionCanonicalLess(const llvm::Instruction *lhs,
                                const llvm::Instruction *rhs) const;

  llvm::Module &m_module;
  ThreadAPI *m_thread_api;
  concurrency::ConcurrencyContext *m_context = nullptr;

  // Analysis components
  std::unique_ptr<ThreadFlowGraph> m_tfg;
  std::unique_ptr<LockSetAnalysis> m_lockset; // Optional
  mutable std::unique_ptr<ThreadRegionAnalysis> m_region_analysis;

  // Configuration
  bool m_enable_lockset_analysis = false;
  bool m_precompute_mhp_pairs = false;

  // MHP results — use unordered_set for O(1) average lookup instead of O(log
  // N). Pairs are stored in canonical order (a <= b by pointer) to allow
  // symmetric lookup with a single probe.
  std::unordered_set<InstPair, InstPairHash> m_mhp_pairs;

  // Instruction to thread mapping
  std::unordered_map<const llvm::Instruction *, ThreadID> m_inst_to_thread;
  std::unordered_map<const llvm::Instruction *, size_t> m_instruction_ordinals;

  // Alias Analysis (borrowed from caller/context or lazily constructed)
  mutable lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  mutable std::unique_ptr<lotus::AliasAnalysisWrapper> m_owned_alias_analysis;

  // Call Graph for interprocedural analysis
  std::unique_ptr<llvm::CallGraph> m_owned_call_graph;
  llvm::CallGraph *m_call_graph = nullptr;
  std::unique_ptr<mhp::JoinTargetAnalysis> m_join_target_analysis;
  std::unique_ptr<concurrency::ThreadMultiplicityAnalysis>
      m_owned_thread_multiplicity;
  concurrency::ThreadMultiplicityAnalysis *m_thread_multiplicity = nullptr;

  // Thread ID allocation
  ThreadID m_next_thread_id = 1; // 0 is reserved for main thread

  // Multi-instance thread tracking
  std::unordered_set<ThreadID> m_multi_instance_threads;

  // Fork-join tracking
  std::unordered_map<ThreadID, const llvm::Instruction *>
      m_thread_fork_sites; // Thread -> fork instruction
  std::unordered_map<ThreadID, ThreadID> m_thread_parents; // Child -> Parent
  std::unordered_map<ThreadID, std::vector<ThreadID>>
      m_thread_children; // Parent -> Children
  std::unordered_map<const llvm::Instruction *, std::unordered_set<ThreadID>>
      m_fork_to_thread; // Fork inst -> context-specific created threads
  std::unordered_map<const SyncNode *, ThreadID> m_fork_node_to_thread;
  std::unordered_map<const llvm::Instruction *, ThreadID>
      m_join_to_thread; // Join inst -> joined thread
  std::unordered_set<ThreadID> m_detached_threads;

  // Value tracking for pthread_t variables. Exact fork-node lookup is used
  // first so repeated helper invocations do not share child identities.
  std::unordered_map<const llvm::Value *, std::unordered_set<ThreadID>>
      m_pthread_value_to_threads; // pthread_t value -> possible thread IDs
  std::unordered_map<ThreadID, const llvm::Value *>
      m_thread_to_pthread_value; // thread ID -> pthread_t value

  // Condition variable tracking (region boundaries / structural bookkeeping)
  // Map condition variable -> list of signal/broadcast instructions
  std::unordered_map<const llvm::Value *, std::vector<SyncNode *>>
      m_condvar_signals;
  // Map condition variable -> list of wait instructions
  std::unordered_map<const llvm::Value *, std::vector<SyncNode *>>
      m_condvar_waits;

  // Barrier tracking (structural ordering between phases)
  struct BarrierParticipant {
    SyncNode *arrival = nullptr;
    std::vector<SyncNode *> continuations;
  };
  // Map barrier object -> phase ordinal -> barrier participants seen so far.
  std::unordered_map<
      const llvm::Value *,
      std::unordered_map<size_t, std::vector<BarrierParticipant>>>
      m_barrier_waits;
  std::unordered_map<const llvm::Value *, size_t> m_barrier_expected_counts;
  std::unordered_map<const llvm::Value *, size_t> m_barrier_init_sites;
  std::unordered_set<const llvm::Value *> m_uncertain_barrier_counts;

  // Per-thread set of functions already processed to avoid reprocessing
  std::unordered_map<
      ThreadID, std::unordered_map<CallContextID,
                                   std::unordered_set<const llvm::Function *>>>
      m_visited_functions_by_thread;
  std::unordered_map<ThreadID, std::vector<const llvm::Function *>>
      m_active_call_stack_by_thread;
  // Context-sensitive nodes in thread 0 that are proven to execute before any
  // reachable thread/task creation from main. These cannot run in parallel
  // with child-thread nodes.
  std::unordered_set<const SyncNode *> m_pre_fork_main_nodes;

  // Indirect fork handling (conservative)
  bool m_has_unresolved_fork = false;
  bool m_module_may_spawn_threads = false;
  std::unordered_set<const llvm::Function *> m_thread_entry_candidates;

  // Dominator tree cache used for loop/fork reasoning within a function
  mutable std::unordered_map<const llvm::Function *,
                             std::unique_ptr<llvm::DominatorTree>>
      m_dom_cache;

  // Query caches (keyed by instruction pointer identity).
  mutable std::unordered_map<InstPair, bool, InstPairHash> m_order_cache;
  mutable std::unordered_map<InstPair, bool, InstPairHash> m_mhp_cache;
  mutable std::unordered_map<ThreadID, bool> m_multi_instance_overlap_cache;
  mutable std::vector<uint32_t> m_order_visit_stamp;
  mutable uint32_t m_order_current_stamp = 0;
  mutable std::vector<SyncNode *> m_order_bfs_queue;
  bool m_has_multi_context_nodes = false;
  bool m_single_main_thread_only = false;
  size_t m_analysis_generation = 0;

  // Context-cloning budget state to prevent exponential TFG blowup on large
  // call graphs while preserving full precision on small/sync-relevant graphs.
  struct ThreadFuncKey {
    ThreadID tid;
    const llvm::Function *func;
    bool operator==(const ThreadFuncKey &other) const {
      return tid == other.tid && func == other.func;
    }
  };
  struct ThreadFuncKeyHash {
    size_t operator()(const ThreadFuncKey &key) const {
      size_t h1 = std::hash<ThreadID>{}(key.tid);
      size_t h2 = std::hash<const void *>{}(key.func);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };
  static constexpr CallContextID kMergedContextBit = CallContextID{1} << 63;
  static constexpr size_t kMaxContextsPerFunction = 4;
  static constexpr size_t kMaxSyncContextsPerFunction = 8;
  static constexpr size_t kMaxContextDepth = 6;
  static constexpr size_t kMaxClonedTFGNodes = 100000;
  static constexpr size_t kMaxPairCacheEntries = 250000;
  std::unordered_map<ThreadFuncKey, size_t, ThreadFuncKeyHash>
      m_func_context_count;
  std::unordered_map<ThreadFuncKey, CallContextID, ThreadFuncKeyHash>
      m_func_shared_context;
  CallContextID m_next_merged_context_id = 1;
  std::unordered_set<const llvm::Function *> m_sync_relevant_functions;
  void computeSyncRelevantFunctions();
  CallContextID chooseCalleeContext(ThreadID tid, const llvm::Function *target,
                                    SyncNode *call_node,
                                    bool force_merged = false,
                                    size_t queued_node_estimate = 0);

  struct PendingJoin {
    const llvm::Instruction *inst = nullptr;
    SyncNode *node = nullptr;
    ThreadID parent_tid = 0;
  };
  std::vector<PendingJoin> m_pending_joins;
  std::vector<const llvm::Instruction *> m_pending_detaches;

  // ========================================================================
  // Analysis Phases
  // ========================================================================

  /**
   * @brief Phase 1: Build thread-flow graph
   *
   * Constructs a graph representation of all threads, including
   * synchronization operations and inter-thread edges.
   */
  void buildThreadFlowGraph();

  /**
   * @brief Phase 2: Analyze lock sets (OPTIONAL)
   *
   * Computes the sets of locks held at each program point.
   * Only runs if enableLockSetAnalysis() was called.
   */
  void analyzeLockSets();

  /**
   * @brief Phase 3: Identify thread regions
   *
   * Divides each thread into regions separated by synchronization.
   */
  void ensureThreadRegionAnalysis() const;
  void analyzeThreadRegions();

  /**
   * @brief Phase 4: Compute MHP pairs
   *
   * Determines which pairs of instructions may execute in parallel.
   */
  void computeMHPPairs();

  /**
   * @brief Fallback: Instruction-level MHP computation (O(N^2))
   *
   * Used when region analysis is not available.
   */
  void computeMHPPairsInstructionLevel();

  std::vector<SyncNode *>
  getBarrierContinuations(const SyncNode *arrival) const;
  std::optional<size_t>
  getBarrierPhaseOrdinal(const llvm::Instruction *barrier_inst,
                         const llvm::Value *barrier) const;
  void finalizeBarrierPhases();
  void lowerOpenMPTasks(const OpenMP::OpenMPSemantics &semantics);
  void finalizePendingJoins();
  void recomputePreForkMainNodes();
  bool isMainThreadSpawnNode(const SyncNode *node) const;
  bool isAlwaysPreForkMain(const llvm::Instruction *inst) const;

  // ========================================================================
  // Helper Methods
  // ========================================================================

  void processFunction(const llvm::Function *func, ThreadID tid,
                       CallContextID ctx = 0);
  void processInstruction(const llvm::Instruction *inst, ThreadID tid,
                          SyncNode *&current_node);

  ThreadID allocateThreadID();
  void mapInstructionToThread(const llvm::Instruction *inst, ThreadID tid);

  // Fork-join analysis
  void handleThreadFork(
      const llvm::Instruction *fork_inst, SyncNode *node, ThreadID parent_tid,
      std::vector<std::pair<const llvm::Function *, ThreadID>>
          *deferred_spawns = nullptr);
  void handleThreadJoin(const llvm::Instruction *join_inst, SyncNode *node,
                        ThreadID parent_tid);
  void handleThreadDetach(const llvm::Instruction *detach_inst);
  bool resolveThreadDetach(const llvm::Instruction *detach_inst);
  void finalizePendingDetaches();

  // Value-tracing helper for pthread_t
  const llvm::Value *tracePthreadT(const llvm::Value *val) const;

  // Synchronization analysis
  void handleLockAcquire(const llvm::Instruction *lock_inst, SyncNode *node);
  void handleLockRelease(const llvm::Instruction *unlock_inst, SyncNode *node);
  void handleCondWait(const llvm::Instruction *wait_inst, SyncNode *node);
  void handleCondSignal(const llvm::Instruction *signal_inst, SyncNode *node);
  void handleBarrier(const llvm::Instruction *barrier_inst, SyncNode *node);

  // Ordering computation used internally to rule out structural overlap.
  bool hasStructuralOrderRelation(const llvm::Instruction *i1,
                                  const llvm::Instruction *i2) const;
  bool isInSameThread(const llvm::Instruction *i1,
                      const llvm::Instruction *i2) const;
  bool isInstructionThreadAmbiguous(const llvm::Instruction *inst) const;

  // Alias analysis helper

  // Fork-join helper methods
  bool isAncestorThread(ThreadID ancestor, ThreadID descendant) const;
  bool isForkSite(const llvm::Instruction *inst) const;
  bool isJoinSite(const llvm::Instruction *inst) const;
  ThreadID getForkedThreadID(const llvm::Instruction *fork_inst) const;
  ThreadID getJoinedThreadID(const llvm::Instruction *join_inst) const;
  bool isMultiInstanceThread(ThreadID tid) const;
  bool multiInstanceThreadMayOverlap(ThreadID tid) const;

  // Dominator helpers (intra-function)
  const llvm::DominatorTree &getDomTree(const llvm::Function *func) const;

  void enableIndirectForkConservatism();

  std::pair<ThreadID, ThreadID> normalizeThreadPair(ThreadID lhs,
                                                    ThreadID rhs) const;

  static constexpr ThreadID kUnknownThread =
      std::numeric_limits<ThreadID>::max();
  std::unordered_map<const llvm::Instruction *,
                     std::vector<std::pair<ThreadID, CallContextID>>>
      m_openmp_task_threads;
  std::unordered_set<std::pair<ThreadID, ThreadID>, ThreadPairHash>
      m_openmp_task_exclusions;
  std::unique_ptr<OpenMP::OpenMPSemantics> m_openmp_semantics;
};

} // namespace mhp

