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
 * @author Lotus Analysis Framework
 * @date 2025
 */

#ifndef MHP_ANALYSIS_H
#define MHP_ANALYSIS_H

#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Analysis/Concurrency/ThreadAPI.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

using InstructionSet = std::unordered_set<const llvm::Instruction *>;
using InstructionVector = std::vector<const llvm::Instruction *>;
using ThreadID = size_t;
using LockID = const llvm::Value *;

// ============================================================================
// Synchronization Node Types
// ============================================================================

/**
 * @brief Types of synchronization nodes in the thread-flow graph
 */
enum class SyncNodeType {
  THREAD_START,       ///< Program entry point
  THREAD_FORK,        ///< pthread_create or similar
  THREAD_JOIN,        ///< pthread_join or similar
  THREAD_EXIT,        ///< pthread_exit or return from thread function
  LOCK_ACQUIRE,       ///< Lock acquisition (mutex lock)
  LOCK_RELEASE,       ///< Lock release (mutex unlock)
  COND_WAIT,          ///< Condition variable wait
  COND_SIGNAL,        ///< Condition variable signal
  COND_BROADCAST,     ///< Condition variable broadcast
  BARRIER_WAIT,       ///< Barrier synchronization
  REGULAR_INST,       ///< Regular instruction
  FUNCTION_CALL,      ///< Function call (non-thread API)
  FUNCTION_RETURN     ///< Function return
};

/**
 * @brief Synchronization node in the thread-flow graph
 */
class SyncNode {
public:
  SyncNode(const llvm::Instruction *inst, SyncNodeType type, ThreadID tid)
      : m_instruction(inst), m_type(type), m_thread_id(tid), m_node_id(next_id++) {}

  const llvm::Instruction *getInstruction() const { return m_instruction; }
  SyncNodeType getType() const { return m_type; }
  ThreadID getThreadID() const { return m_thread_id; }
  size_t getNodeID() const { return m_node_id; }

  // Synchronization-specific data
  void setLockValue(const llvm::Value *lock) { m_lock_value = lock; }
  const llvm::Value *getLockValue() const { return m_lock_value; }

  void setCondValue(const llvm::Value *cond) { m_cond_value = cond; }
  const llvm::Value *getCondValue() const { return m_cond_value; }

  void setForkedThread(ThreadID tid) { m_forked_thread = tid; }
  ThreadID getForkedThread() const { return m_forked_thread; }

  void setJoinedThread(ThreadID tid) { m_joined_thread = tid; }
  ThreadID getJoinedThread() const { return m_joined_thread; }

  // Predecessors and successors
  void addPredecessor(SyncNode *pred) { m_predecessors.push_back(pred); }
  void addSuccessor(SyncNode *succ) { m_successors.push_back(succ); }

  const std::vector<SyncNode *> &getPredecessors() const {
    return m_predecessors;
  }
  const std::vector<SyncNode *> &getSuccessors() const {
    return m_successors;
  }

  // For debugging
  void print(llvm::raw_ostream &os) const;
  std::string toString() const;

private:
  const llvm::Instruction *m_instruction;
  SyncNodeType m_type;
  ThreadID m_thread_id;
  size_t m_node_id;

  // Synchronization-specific data
  const llvm::Value *m_lock_value = nullptr;
  const llvm::Value *m_cond_value = nullptr;
  ThreadID m_forked_thread = 0;
  ThreadID m_joined_thread = 0;

  // Graph structure
  std::vector<SyncNode *> m_predecessors;
  std::vector<SyncNode *> m_successors;

  static size_t next_id;
};

// ============================================================================
// Thread Flow Graph
// ============================================================================

/**
 * @brief Thread-flow graph representation
 * 
 * Represents the control flow and synchronization structure of a multithreaded
 * program. Each thread has its own flow graph, and synchronization edges
 * connect different threads.
 */
class ThreadFlowGraph {
public:
  ThreadFlowGraph() = default;
  ~ThreadFlowGraph();

  // Node management
  SyncNode *createNode(const llvm::Instruction *inst, SyncNodeType type,
                       ThreadID tid);
  SyncNode *getNode(const llvm::Instruction *inst) const;
  const std::vector<SyncNode *> &getAllNodes() const { return m_all_nodes; }

  // Thread management
  void addThread(ThreadID tid, const llvm::Function *entry);
  const llvm::Function *getThreadEntry(ThreadID tid) const;
  std::vector<ThreadID> getAllThreads() const;

  // Entry and exit nodes
  void setThreadEntry(ThreadID tid, SyncNode *entry);
  void setThreadExit(ThreadID tid, SyncNode *exit);
  SyncNode *getThreadEntry(ThreadID tid);
  SyncNode *getThreadExit(ThreadID tid);

  // Graph construction helpers
  void addIntraThreadEdge(SyncNode *from, SyncNode *to);
  void addInterThreadEdge(SyncNode *from, SyncNode *to);

  // Query interface
  std::vector<SyncNode *> getNodesOfType(SyncNodeType type) const;
  std::vector<SyncNode *> getNodesInThread(ThreadID tid) const;

  // Debugging and visualization
  void print(llvm::raw_ostream &os) const;
  void printAsDot(llvm::raw_ostream &os) const;
  void dumpToFile(const std::string &filename) const;

private:
  std::vector<SyncNode *> m_all_nodes;
  std::unordered_map<const llvm::Instruction *, SyncNode *> m_inst_to_node;
  std::unordered_map<ThreadID, const llvm::Function *> m_thread_entries;
  std::unordered_map<ThreadID, SyncNode *> m_thread_entry_nodes;
  std::unordered_map<ThreadID, SyncNode *> m_thread_exit_nodes;
};

// ============================================================================
// Thread Region Analysis
// ============================================================================

/**
 * @brief Divides program into thread regions based on synchronization
 * 
 * A thread region is a maximal sequence of instructions within a single thread
 * that are not separated by any synchronization operations. Regions are the
 * basic units for MHP analysis.
 */
class ThreadRegionAnalysis {
public:
  struct Region {
    size_t region_id;
    ThreadID thread_id;
    SyncNode *start_node;
    SyncNode *end_node;
    InstructionSet instructions;

    // Synchronization constraints
    std::set<size_t> must_precede;    // Regions that must execute before this
    std::set<size_t> must_follow;     // Regions that must execute after this
    std::set<size_t> may_be_parallel; // Regions that may run in parallel
  };

  ThreadRegionAnalysis(const ThreadFlowGraph &tfg)
      : m_tfg(tfg) {}

  void analyze();

  // Query interface
  const Region *getRegion(size_t region_id) const;
  const Region *getRegionContaining(const llvm::Instruction *inst) const;
  const std::vector<std::unique_ptr<Region>> &getAllRegions() const {
    return m_regions;
  }

  void print(llvm::raw_ostream &os) const;

private:
  const ThreadFlowGraph &m_tfg;

  std::vector<std::unique_ptr<Region>> m_regions;
  std::unordered_map<const llvm::Instruction *, const Region *>
      m_inst_to_region;

  void identifyRegions();
  void computeOrderingConstraints();
  void computeParallelism();
};

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
class MHPAnalysis {
public:
  explicit MHPAnalysis(llvm::Module &module);
  ~MHPAnalysis() = default;

  // Main analysis entry point
  void analyze();

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
                           const llvm::Instruction *i2) const;

  /**
   * @brief Get all instructions that may run in parallel with the given one
   * 
   * @param inst Target instruction
   * @return Set of instructions that may execute concurrently with inst
   */
  InstructionSet getParallelInstructions(const llvm::Instruction *inst) const;

  /**
   * @brief Check if two instructions must execute sequentially
   * 
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 and i2 cannot execute concurrently
   */
  bool mustBeSequential(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const;

  /**
   * @brief Check if instruction i1 must execute before i2
   * 
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 must execute before i2 in all executions
   */
  bool mustPrecede(const llvm::Instruction *i1,
                   const llvm::Instruction *i2) const;

  /**
   * @brief Get the thread ID that an instruction belongs to
   * 
   * @param inst Target instruction
   * @return Thread ID, or 0 if main thread
   */
  ThreadID getThreadID(const llvm::Instruction *inst) const;

  /**
   * @brief Get all instructions in a specific thread
   * 
   * @param tid Thread ID
   * @return Set of instructions in the thread
   */
  InstructionSet getInstructionsInThread(ThreadID tid) const;

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
  void printStatistics(llvm::raw_ostream &os) const;
  void printResults(llvm::raw_ostream &os) const;

  // Component access for advanced users
  const ThreadFlowGraph &getThreadFlowGraph() const { return *m_tfg; }
  const ThreadRegionAnalysis &getThreadRegionAnalysis() const {
    return *m_region_analysis;
  }
  
  // Optional: LockSetAnalysis for more precise race detection
  LockSetAnalysis *getLockSetAnalysis() const { return m_lockset.get(); }
  void enableLockSetAnalysis();

  // Visualization
  void dumpThreadFlowGraph(const std::string &filename) const;
  void dumpMHPMatrix(llvm::raw_ostream &os) const;

private:
  llvm::Module &m_module;
  ThreadAPI *m_thread_api;

  // Analysis components
  std::unique_ptr<ThreadFlowGraph> m_tfg;
  std::unique_ptr<LockSetAnalysis> m_lockset;  // Optional
  std::unique_ptr<ThreadRegionAnalysis> m_region_analysis;
  
  // Configuration
  bool m_enable_lockset_analysis = false;

  // MHP results
  std::set<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_mhp_pairs;

  // Instruction to thread mapping
  std::unordered_map<const llvm::Instruction *, ThreadID> m_inst_to_thread;

  // Thread ID allocation
  ThreadID m_next_thread_id = 1; // 0 is reserved for main thread

  // Fork-join tracking
  std::unordered_map<ThreadID, const llvm::Instruction *>
      m_thread_fork_sites;                                   // Thread -> fork instruction
  std::unordered_map<ThreadID, ThreadID> m_thread_parents;   // Child -> Parent
  std::unordered_map<ThreadID, std::vector<ThreadID>> m_thread_children; // Parent -> Children
  std::unordered_map<const llvm::Instruction *, ThreadID>
      m_fork_to_thread;                                      // Fork inst -> created thread
  std::unordered_map<const llvm::Instruction *, ThreadID>
      m_join_to_thread;                                      // Join inst -> joined thread
  
  // Value tracking for pthread_t variables
  std::unordered_map<const llvm::Value *, ThreadID>
      m_pthread_value_to_thread;                             // pthread_t value -> thread ID
  std::unordered_map<ThreadID, const llvm::Value *>
      m_thread_to_pthread_value;                             // thread ID -> pthread_t value

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
  void analyzeThreadRegions();

  /**
   * @brief Phase 4: Compute MHP pairs
   * 
   * Determines which pairs of instructions may execute in parallel.
   */
  void computeMHPPairs();

  // ========================================================================
  // Helper Methods
  // ========================================================================

  void processFunction(const llvm::Function *func, ThreadID tid);
  void processInstruction(const llvm::Instruction *inst, ThreadID tid,
                          SyncNode *&current_node);

  ThreadID allocateThreadID();
  void mapInstructionToThread(const llvm::Instruction *inst, ThreadID tid);

  // Fork-join analysis
  void handleThreadFork(const llvm::Instruction *fork_inst, SyncNode *node);
  void handleThreadJoin(const llvm::Instruction *join_inst, SyncNode *node);

  // Synchronization analysis
  void handleLockAcquire(const llvm::Instruction *lock_inst, SyncNode *node);
  void handleLockRelease(const llvm::Instruction *unlock_inst, SyncNode *node);
  void handleCondWait(const llvm::Instruction *wait_inst, SyncNode *node);
  void handleCondSignal(const llvm::Instruction *signal_inst, SyncNode *node);
  void handleBarrier(const llvm::Instruction *barrier_inst, SyncNode *node);

  // Ordering computation
  bool hasHappenBeforeRelation(const llvm::Instruction *i1,
                                const llvm::Instruction *i2) const;
  bool isInSameThread(const llvm::Instruction *i1,
                      const llvm::Instruction *i2) const;
  bool isOrderedByLocks(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const;
  bool isOrderedByForkJoin(const llvm::Instruction *i1,
                           const llvm::Instruction *i2) const;
  
  // Fork-join helper methods
  bool isAncestorThread(ThreadID ancestor, ThreadID descendant) const;
  bool isForkSite(const llvm::Instruction *inst) const;
  bool isJoinSite(const llvm::Instruction *inst) const;
  ThreadID getForkedThreadID(const llvm::Instruction *fork_inst) const;
  ThreadID getJoinedThreadID(const llvm::Instruction *join_inst) const;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Get string name for synchronization node type
 */
llvm::StringRef getSyncNodeTypeName(SyncNodeType type);

/**
 * @brief Check if a node type represents a synchronization operation
 */
bool isSynchronizationNode(SyncNodeType type);

/**
 * @brief Check if a node type represents thread creation/termination
 */
bool isThreadBoundaryNode(SyncNodeType type);

} // namespace mhp

#endif // MHP_ANALYSIS_H
