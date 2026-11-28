/**
 * @file LockSetAnalysis.h
 * @brief Production-ready Lock Set Analysis for Multithreaded Programs
 *
 * This file provides a comprehensive lock set analysis that computes the
 * sets of locks that may or must be held at each program point. This is
 * essential for:
 * - Data race detection
 * - Deadlock detection
 * - MHP (May-Happen-in-Parallel) analysis
 * - Lock ordering verification
 *
 * Key Features:
 * - Intraprocedural lock set computation
 * - Interprocedural lock set propagation
 * - May-lockset analysis (over-approximation)
 * - Must-lockset analysis (under-approximation)
 * - Lock aliasing support
 * - Reentrant lock handling
 * - Support for try-lock operations
 *
 * @author rainoftime
 * @date 2026
 */

#pragma once

#include "Concurrency/LockSet/RAIILockTracker.h"
#include "Concurrency/API/ThreadAPI.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace llvm {
class CallGraph;
} // namespace llvm

namespace mhp {

// ============================================================================
// Type Definitions
// ============================================================================

using LockID = const llvm::Value *;
using LockSet = std::set<LockID>;
using RecursiveDepthMap = std::map<LockID, unsigned>;
using RAIIWrapperSet = std::set<const llvm::Value *>;

enum class MemoryAccessKind { Read, Write };

// ============================================================================
// Lock Set Analysis
// ============================================================================

/**
 * @brief Comprehensive lock set analysis for concurrent programs
 *
 * Computes may-locksets and must-locksets at each program point using
 * dataflow analysis. Handles:
 * - pthread_mutex_lock/unlock
 * - pthread_rwlock operations
 * - binary semaphores only when explicitly tagged as exclusion-capable
 * - Reentrant locks
 * - Try-lock operations
 *
 * Usage:
 *   LockSetAnalysis lsa(module);
 *   lsa.analyze();
 *   LockSet locks = lsa.getMayLockSetAt(inst);
 */
class LockSetAnalysis {
public:
  // ========================================================================
  // Construction and Analysis
  // ========================================================================

  /**
   * @brief Construct lock set analysis for a module
   * @param module The LLVM module to analyze
   */
  explicit LockSetAnalysis(llvm::Module &module);

  /**
   * @brief Construct lock set analysis for a single function
   * @param func The function to analyze
   */
  explicit LockSetAnalysis(llvm::Function &func);

  ~LockSetAnalysis() = default;

  /**
   * @brief Run the lock set analysis
   */
  void analyze();

  /**
   * @brief Set alias analysis wrapper for better precision
   * @param aa_wrapper Alias analysis wrapper instance
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa_wrapper) {
    m_alias_analysis = aa_wrapper;
    m_canonical_lock_cache.clear();
  }

  /**
   * @brief Set call graph for interprocedural analysis
   * @param cg LLVM CallGraph instance
   */
  void setCallGraph(llvm::CallGraph *cg) {
    m_call_graph = cg;
    m_uses_external_call_graph = cg != nullptr;
  }

  // ========================================================================
  // Query Interface
  // ========================================================================

  /**
   * @brief Get locks that may be held at instruction (union of read + write)
   * @param inst Target instruction
   * @return Set of locks that may be held
   */
  LockSet getMayLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get read locks that may be held (rwlock_rdlock)
   */
  LockSet getMayReadLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get write locks that may be held (mutex or rwlock_wrlock)
   */
  LockSet getMayWriteLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get locks that must be held at instruction
   */
  LockSet getMustLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get read locks that must be held
   */
  LockSet getMustReadLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get write locks that must be held
   */
  LockSet getMustWriteLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Check if a lock may be held at instruction
   * @param inst Target instruction
   * @param lock Lock value
   * @return true if lock may be held
   */
  bool mayHoldLock(const llvm::Instruction *inst, LockID lock) const;

  /**
   * @brief Check if a lock must be held at instruction
   * @param inst Target instruction
   * @param lock Lock value
   * @return true if lock must be held
   */
  bool mustHoldLock(const llvm::Instruction *inst, LockID lock) const;

  /**
   * @brief Get all instructions that may hold a specific lock
   * @param lock Lock value
   * @return Set of instructions
   */
  std::unordered_set<const llvm::Instruction *>
  getInstructionsHoldingLock(LockID lock) const;

  /**
   * @brief Check if two instructions may hold a common lock (mutex or rwlock).
   * For rwlocks: true if both hold same lock in write mode, or both in read
   * mode.
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if they may hold a common lock
   */
  bool mayHoldCommonLock(const llvm::Instruction *i1,
                         const llvm::Instruction *i2) const;

  /**
   * @brief Check if two instructions are definitely mutually excluded by the
   * same lock identity, taking shared-vs-exclusive modes into account.
   */
  bool mustMutuallyExclude(const llvm::Instruction *i1,
                           const llvm::Instruction *i2) const;

  bool mustHoldCommonLock(const llvm::Instruction *i1,
                          const llvm::Instruction *i2) const;

  bool mustMutuallyExclude(const llvm::Instruction *i1,
                           MemoryAccessKind access1,
                           const llvm::Instruction *i2,
                           MemoryAccessKind access2) const;

  /**
   * @brief Get all locks that may be held in a function
   * @param func Target function
   * @return Set of all locks
   */
  LockSet getAllLocksInFunction(const llvm::Function *func) const;

  const std::unordered_set<LockID> &getAllLocks() const { return m_all_locks; }

  /**
   * @brief Get lock acquire instructions for a specific lock
   * @param lock Lock value
   * @return Vector of acquire instructions
   */
  std::vector<const llvm::Instruction *> getLockAcquires(LockID lock) const;

  /**
   * @brief Get lock release instructions for a specific lock
   * @param lock Lock value
   * @return Vector of release instructions
   */
  std::vector<const llvm::Instruction *> getLockReleases(LockID lock) const;

  // ========================================================================
  // Advanced Queries
  // ========================================================================

  /**
   * @brief Check if a lock is reentrant (acquired multiple times in same path)
   * @param lock Lock value
   * @return true if lock may be reentrant
   */
  bool isReentrantLock(LockID lock) const;

  /**
   * @brief Get the lock nesting depth at an instruction
   * @param inst Target instruction
   * @return Maximum nesting depth (0 if no locks held)
   */
  size_t getLockNestingDepth(const llvm::Instruction *inst) const;

  /**
   * @brief Check if locks are acquired in consistent order
   * @param lock1 First lock
   * @param lock2 Second lock
   * @return true if order is consistent (no potential deadlock)
   */
  bool areLocksOrderedConsistently(LockID lock1, LockID lock2) const;

  /**
   * @brief Detect potential lock order inversions (deadlock candidates)
   * @return Pairs of locks that may cause deadlock
   */
  std::vector<std::pair<LockID, LockID>> detectLockOrderInversions() const;

  // ========================================================================
  // Lock Ordering Graph Queries
  // ========================================================================

  /**
   * @brief Get locks acquired while holding the given lock
   */
  std::vector<LockID> getLockOrderSuccessors(LockID lock) const;

  /**
   * @brief Get locks that are held when the given lock is acquired
   */
  std::vector<LockID> getLockOrderPredecessors(LockID lock) const;

  /**
   * @brief Detect all cycles in the lock ordering graph via DFS
   * @return Each cycle as a vector of LockIDs forming the cycle
   */
  std::vector<std::vector<LockID>> detectDeadlockCycles() const;

  // ========================================================================
  // Critical Section Analysis
  // ========================================================================

  /**
   * @brief Get all instructions where a lock is must-held
   */
  std::vector<const llvm::Instruction *>
  getProtectedInstructions(LockID lock) const;

  /**
   * @brief Get (acquire, release) pairs bounding each critical section
   */
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  getCriticalSections(LockID lock) const;

  /**
   * @brief Get the maximum instruction count in any critical section of a lock
   */
  size_t getMaxCriticalSectionSize(LockID lock) const;

  // ========================================================================
  // Condition Variable Validation
  // ========================================================================

  /**
   * @brief Check if the associated mutex is must-held at a cond_wait call
   */
  bool isLockHeldAtCondWait(const llvm::Instruction *cond_wait_inst) const;

  /**
   * @brief Find all cond_wait calls where the associated mutex is not must-held
   */
  std::vector<const llvm::Instruction *> getUnprotectedCondWaits() const;

  // ========================================================================
  // Statistics and Debugging
  // ========================================================================

  struct Statistics {
    size_t num_locks;           ///< Total number of distinct locks
    size_t num_acquires;        ///< Total lock acquire operations
    size_t num_releases;        ///< Total lock release operations
    size_t num_try_acquires;    ///< Total try-lock operations
    size_t max_nesting_depth;   ///< Maximum observed lock nesting
    size_t num_reentrant_locks; ///< Number of reentrant locks
    size_t
        num_potential_deadlocks; ///< Number of potential deadlocks (pairwise)
    size_t num_functions_with_locks;  ///< Functions containing lock ops
    size_t num_critical_sections;     ///< Total critical section count
    double avg_critical_section_size; ///< Average instructions per CS
    size_t max_critical_section_size; ///< Largest critical section
    size_t num_rwlocks;               ///< Distinct read-write locks
    size_t num_condvar_waits;         ///< Condition variable wait operations
    size_t num_unprotected_condvar_waits; ///< cond_waits without mutex held
    size_t num_deadlock_cycles;           ///< Cycles in lock ordering graph

    void print(llvm::raw_ostream &os) const;
  };

  Statistics getStatistics() const;
  void printStatistics(llvm::raw_ostream &os) const;
  void printResults(llvm::raw_ostream &os) const;
  void printLockSetsForFunction(const llvm::Function *func,
                                llvm::raw_ostream &os) const;

  // ========================================================================
  // Visualization
  // ========================================================================

  /**
   * @brief Dump lock acquisition graph as DOT format
   * @param filename Output file name
   */
  void dumpLockGraph(const std::string &filename) const;

  /**
   * @brief Print lock sets in a readable format
   * @param os Output stream
   */
  void print(llvm::raw_ostream &os) const;

private:
  // ========================================================================
  // Data Structures
  // ========================================================================

  llvm::Module *m_module;
  llvm::Function *m_single_function; // For single-function analysis
  ThreadAPI *m_thread_api;
  lotus::AliasAnalysisWrapper *m_alias_analysis;
  llvm::CallGraph *m_call_graph; // For interprocedural analysis
  std::unique_ptr<llvm::CallGraph> m_owned_call_graph;
  bool m_uses_external_call_graph = false;

  // Lockset results (combined for backward compat; read/write for rwlock)
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_locksets_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_locksets_exit;
  std::unordered_map<const llvm::Instruction *, LockSet> m_must_locksets_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_must_locksets_exit;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_read_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_read_locks_exit;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_may_write_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_write_locks_exit;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_must_read_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_must_read_locks_exit;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_must_write_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_must_write_locks_exit;
  std::unordered_map<const llvm::Instruction *, RecursiveDepthMap>
      m_may_recursive_depth_entry;
  std::unordered_map<const llvm::Instruction *, RecursiveDepthMap>
      m_may_recursive_depth_exit;
  std::unordered_map<const llvm::Instruction *, RecursiveDepthMap>
      m_must_recursive_depth_entry;
  std::unordered_map<const llvm::Instruction *, RecursiveDepthMap>
      m_must_recursive_depth_exit;
  std::unordered_map<const llvm::Instruction *, RAIIWrapperSet>
      m_may_raii_ownership_entry;
  std::unordered_map<const llvm::Instruction *, RAIIWrapperSet>
      m_may_raii_ownership_exit;
  std::unordered_map<const llvm::Instruction *, RAIIWrapperSet>
      m_must_raii_ownership_entry;
  std::unordered_map<const llvm::Instruction *, RAIIWrapperSet>
      m_must_raii_ownership_exit;

  // Lock tracking
  std::unordered_set<LockID> m_all_locks;
  std::unordered_map<LockID, std::vector<const llvm::Instruction *>>
      m_lock_acquires;
  std::unordered_map<LockID, std::vector<const llvm::Instruction *>>
      m_lock_releases;
  std::unordered_map<LockID, std::vector<const llvm::Instruction *>>
      m_lock_try_acquires;

  // Lock ordering tracking
  struct LockPair {
    LockID first;
    LockID second;

    bool operator==(const LockPair &other) const {
      return first == other.first && second == other.second;
    }

    struct Hash {
      size_t operator()(const LockPair &p) const {
        return std::hash<LockID>()(p.first) ^ std::hash<LockID>()(p.second);
      }
    };
  };

  std::unordered_set<LockPair, LockPair::Hash> m_observed_lock_orders;
  // Blocking, mode-incompatible order edges used for deadlock reporting.
  std::unordered_set<LockPair, LockPair::Hash> m_deadlock_wait_orders;
  std::unordered_set<LockID> m_reentrant_locks;

  // RAII lock tracking per function (type alias avoids C++11 >> parse issue)
  using RAIILockMap =
      std::multimap<const llvm::Value *, RAIILock::LockLifetime>;
  std::unordered_map<const llvm::Function *, RAIILockMap> m_raii_locks;

  // Interprocedural analysis data structures
  struct FunctionSummary {
    LockSet may_acquire_delta;      ///< Locks newly held on some exit path
    LockSet may_read_acquire_delta; ///< Read locks newly held on some exit path
    LockSet
        may_write_acquire_delta; ///< Write locks newly held on some exit path
    LockSet must_acquire_delta;  ///< Locks newly held on all normal exit paths
    LockSet must_read_acquire_delta;  ///< Read locks newly held on all exits
    LockSet must_write_acquire_delta; ///< Write locks newly held on all exits
    LockSet may_release_delta;  ///< Locks maybe released on some returning path
    LockSet must_release_delta; ///< Locks definitely released on all returning
                                ///< paths
    LockSet exceptional_may_acquire_delta;
    LockSet exceptional_may_read_acquire_delta;
    LockSet exceptional_may_write_acquire_delta;
    LockSet exceptional_must_acquire_delta;
    LockSet exceptional_must_read_acquire_delta;
    LockSet exceptional_must_write_acquire_delta;
    LockSet exceptional_may_release_delta;
    LockSet exceptional_must_release_delta;
    RecursiveDepthMap may_recursive_acquire_delta;
    RecursiveDepthMap must_recursive_acquire_delta;
    RecursiveDepthMap may_recursive_release_delta;
    RecursiveDepthMap must_recursive_release_delta;
    RecursiveDepthMap exceptional_may_recursive_acquire_delta;
    RecursiveDepthMap exceptional_must_recursive_acquire_delta;
    RecursiveDepthMap exceptional_may_recursive_release_delta;
    RecursiveDepthMap exceptional_must_recursive_release_delta;
    bool has_exceptional_exit = false;
    bool is_analyzed; ///< Whether function has been analyzed

    FunctionSummary() : is_analyzed(false) {}
  };

  std::unordered_map<const llvm::Function *, FunctionSummary>
      m_function_summaries;

  struct InvokeEdgeFacts {
    LockSet may_lockset;
    LockSet must_lockset;
    LockSet may_read_lockset;
    LockSet may_write_lockset;
    LockSet must_read_lockset;
    LockSet must_write_lockset;
    RecursiveDepthMap may_recursive_depth;
    RecursiveDepthMap must_recursive_depth;
  };
  std::unordered_map<const llvm::Instruction *, InvokeEdgeFacts>
      m_invoke_normal_exit;
  std::unordered_map<const llvm::Instruction *, InvokeEdgeFacts>
      m_invoke_unwind_exit;

  struct TryLockEdgeRefinement {
    const llvm::CallBase *call = nullptr;
    LockID lock = nullptr;
    ThreadAPI::LockSemanticKind mode = ThreadAPI::LockSemanticKind::None;
    bool success = false;
  };
  using CFGEdge = std::pair<const llvm::Instruction *, unsigned>;
  std::map<CFGEdge, std::vector<TryLockEdgeRefinement>>
      m_trylock_edge_refinements;
  mutable std::map<std::pair<const llvm::CallBase *, LockID>,
                   std::unique_ptr<llvm::GetElementPtrInst>>
      m_summary_lock_projections;

  mutable llvm::DenseMap<const llvm::Value *, LockID> m_canonical_lock_cache;

  // Pre-scanned lock metadata (computed once per analyze() call)
  unsigned m_recursive_acquire_limit = 1;
  std::unordered_set<const llvm::Function *> m_functions_with_direct_lock_ops;
  std::unordered_set<const llvm::Function *> m_functions_with_raii_locks;
  std::unordered_set<const llvm::Function *> m_functions_with_trylocks;
  std::unordered_set<const llvm::Function *> m_lock_relevant_functions;
  void prescanLockOperations();

  // ========================================================================
  // Analysis Implementation
  // ========================================================================

  /**
   * @brief Analyze a single function
   */
  void analyzeFunction(llvm::Function *func);

  /**
   * @brief Perform intraprocedural dataflow analysis
   */
  void computeIntraproceduralLockSets(llvm::Function *func);

  /**
   * @brief Perform interprocedural analysis
   */
  void computeInterproceduralLockSets();

  /**
   * @brief Transfer function for lockset analysis (combined set for backward
   * compat)
   */
  LockSet transfer(const llvm::Instruction *inst, const LockSet &in_set,
                   bool is_must) const;

  /**
   * @brief Transfer for read/write locks (rdlock, wrlock, unlock)
   */
  void transferReadWrite(const llvm::Instruction *inst, const LockSet &in_read,
                         const LockSet &in_write, LockSet &out_read,
                         LockSet &out_write, bool is_must) const;

  /**
   * @brief Merge locksets from multiple predecessors
   * @param sets Vector of locksets to merge
   * @param is_must True for must-analysis (intersection), false for may (union)
   * @return Merged lockset
   */
  LockSet merge(const std::vector<LockSet> &sets, bool is_must) const;

  /**
   * @brief Identify all locks in the program
   */
  void identifyLocks();

  /**
   * @brief Track lock ordering relationships
   */
  void trackLockOrdering();

  /**
   * @brief Check if two locks may alias
   */
  bool mayAlias(LockID lock1, LockID lock2) const;

public:
  // Shared identity queries for checker domains that compose lock effects.
  bool locksMustMatch(LockID lock1, LockID lock2) const;

  /**
   * @brief Get canonical lock value (handling aliases)
   */
  LockID getCanonicalLock(LockID lock) const;

  /**
   * @brief Resolve the underlying mutex for a RAII lock object when known
   */
  LockID getUnderlyingRAIILock(const llvm::Instruction *inst,
                               const llvm::Value *lock_obj) const;
  std::vector<LockID> getUnderlyingRAIILocks(const llvm::Instruction *inst,
                                             const llvm::Value *lock_obj) const;
  std::vector<LockID>
  getRAIILocksReleasedAt(const llvm::Instruction *inst,
                         bool include_maybe_owned = false) const;
  std::vector<LockID>
  getImpreciseRAIILocksEndingAt(const llvm::Instruction *inst) const;
  std::vector<LockID>
  getImpreciseRAIILocksInFunction(const llvm::Function *func) const;

  /**
   * @brief Resolve the lock identity for explicit C++ wrapper operations
   */
  LockID getCppWrapperLockValue(const llvm::Instruction *inst) const;

  /**
   * @brief Check if instruction is a lock operation
   */
  bool isLockOperation(const llvm::Instruction *inst) const;

  /**
   * @brief Get lock value from lock operation
   */
  LockID getLockValue(const llvm::Instruction *inst) const;

  LockID projectLockAtCall(const llvm::CallBase *call,
                           const llvm::Function *callee, LockID lock) const {
    return instantiateSummaryLock(call, callee, lock);
  }

private:
  /**
   * @brief Compute function summary for interprocedural analysis
   */
  void computeFunctionSummary(llvm::Function *func);

  /**
   * @brief Apply function summary at call site
   */
  void applyFunctionSummary(const llvm::CallBase *call,
                            const llvm::Function *callee, LockSet &may_locks,
                            LockSet &must_locks) const;
  bool applyExceptionalFunctionSummary(const llvm::CallBase *call,
                                       const llvm::Function *callee,
                                       LockSet &may_locks,
                                       LockSet &must_locks) const;
  LockID instantiateSummaryLock(const llvm::CallBase *call,
                                const llvm::Function *callee,
                                LockID lock) const;
  RAIIWrapperSet transferRAIIOwnership(const llvm::Instruction *inst,
                                       const RAIIWrapperSet &in,
                                       bool is_must) const;

  /**
   * @brief Get callees at a call site using CallGraph
   */
  std::set<llvm::Function *> getCallees(const llvm::CallBase *call) const;
  bool hasUnresolvedCalleeTarget(const llvm::CallBase *call) const;
  bool shouldInvalidateMustLockState(const llvm::CallBase *call) const;

  /**
   * @brief Perform bottom-up call graph traversal for interprocedural analysis
   */
  void bottomUpTraversal();

  /**
   * @brief Detect try-lock result tests and record edge-specific refinements
   */
  void detectTryLockSuccessBranches(llvm::Function *func);
};

} // namespace mhp
