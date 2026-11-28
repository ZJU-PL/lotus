/**
 * @file ThreadFlowGraph.h
 * @brief Thread Flow Graph representation for concurrency analysis
 *
 * This file defines the core classes for representing thread control flow
 * and synchronization in multithreaded programs.
 *
 * @author rainoftime
 * @date 2025-2026
 */

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace mhp {

// ============================================================================
// Type Definitions
// ============================================================================

using ThreadID = size_t;
using CallContextID = size_t;

// ============================================================================
// Synchronization Node Types
// ============================================================================

/**
 * @brief Types of synchronization nodes in the thread-flow graph
 */
enum class SyncNodeType {
  THREAD_START,   ///< Program entry point
  THREAD_FORK,    ///< pthread_create or similar
  THREAD_JOIN,    ///< pthread_join or similar
  THREAD_EXIT,    ///< pthread_exit or return from thread function
  LOCK_ACQUIRE,   ///< Lock acquisition (mutex lock)
  LOCK_RELEASE,   ///< Lock release (mutex unlock)
  COND_WAIT,      ///< Condition variable wait
  COND_SIGNAL,    ///< Condition variable signal
  COND_BROADCAST, ///< Condition variable broadcast
  BARRIER_WAIT,   ///< Barrier synchronization
  REGULAR_INST,   ///< Regular instruction
  FUNCTION_CALL,  ///< Function call (non-thread API)
  FUNCTION_RETURN ///< Function return
};

/**
 * @brief Edge kind in the thread-flow graph (call, ret, create, join, signal)
 */
enum class EdgeKind {
  Control,       ///< Intra-thread control flow
  Call,          ///< Call site -> callee entry
  Ret,           ///< Callee exit -> return site
  Create,        ///< Fork -> thread entry
  Join,          ///< Thread exit -> join site
  Signal,        ///< signal(c) -> wait(c)
  Barrier,       ///< Barrier synchronization between threads
  TaskDepend,    ///< OpenMP task-dependence ordering
  TaskWait,      ///< OpenMP taskwait/taskgroup ordering
  TaskCompletion ///< OpenMP task-completion ordering
};

/**
 * @brief Synchronization node in the thread-flow graph
 */
class SyncNode {
public:
  SyncNode(const llvm::Instruction *inst, SyncNodeType type, ThreadID tid,
           CallContextID ctx, size_t node_id)
      : m_instruction(inst), m_type(type), m_thread_id(tid),
        m_call_context_id(ctx), m_node_id(node_id) {}

  const llvm::Instruction *getInstruction() const { return m_instruction; }
  SyncNodeType getType() const { return m_type; }
  ThreadID getThreadID() const { return m_thread_id; }
  CallContextID getCallContextID() const { return m_call_context_id; }
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
  void addPredecessor(SyncNode *pred);
  void addSuccessor(SyncNode *succ);
  void addSuccessorWithKind(SyncNode *succ, uint32_t kind_bit);
  uint32_t getEdgeMaskTo(const SyncNode *succ) const;
  uint32_t getOutgoingEdgeKindMask() const { return m_outgoing_edge_kinds; }
  bool hasAnyOutgoingEdgeKind(EdgeKind kind) const {
    return (m_outgoing_edge_kinds & (uint32_t{1} << static_cast<unsigned>(kind))) != 0;
  }

  const std::vector<SyncNode *> &getPredecessors() const {
    return m_predecessors;
  }
  const std::vector<SyncNode *> &getSuccessors() const { return m_successors; }

  // For debugging
  void print(llvm::raw_ostream &os) const;
  std::string toString() const;

private:
  const llvm::Instruction *m_instruction;
  SyncNodeType m_type;
  ThreadID m_thread_id;
  CallContextID m_call_context_id;
  size_t m_node_id;

  // Synchronization-specific data
  const llvm::Value *m_lock_value = nullptr;
  const llvm::Value *m_cond_value = nullptr;
  ThreadID m_forked_thread = 0;
  ThreadID m_joined_thread = 0;
  uint32_t m_outgoing_edge_kinds = 0;

  // Graph structure (compact inline dedup for low-degree nodes, lazy hash map
  // only for high-degree hubs).
  std::vector<SyncNode *> m_predecessors;
  std::vector<SyncNode *> m_successors;
  llvm::SmallVector<uint32_t, 2> m_successor_edge_masks;
  std::unique_ptr<std::unordered_set<SyncNode *>> m_predecessor_set;
  std::unique_ptr<std::unordered_map<const SyncNode *, size_t>> m_successor_index;
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
 *
 * The graph is a non-owning view of LLVM IR. Its Module, Instructions, and
 * Values must remain alive and structurally unchanged for the graph lifetime.
 */
class ThreadFlowGraph {
public:
  ThreadFlowGraph() = default;
  ~ThreadFlowGraph();
  ThreadFlowGraph(const ThreadFlowGraph &) = delete;
  ThreadFlowGraph &operator=(const ThreadFlowGraph &) = delete;
  ThreadFlowGraph(ThreadFlowGraph &&) = delete;
  ThreadFlowGraph &operator=(ThreadFlowGraph &&) = delete;

  // Node management
  SyncNode *createNode(const llvm::Instruction *inst, SyncNodeType type,
                       ThreadID tid, CallContextID ctx = 0);
  SyncNode *getNode(const llvm::Instruction *inst, ThreadID tid,
                    CallContextID ctx) const;
  SyncNode *getNode(const llvm::Instruction *inst, ThreadID tid) const;
  SyncNode *getNode(const llvm::Instruction *inst) const;
  SyncNode *getNodeByID(size_t node_id) const;
  std::vector<SyncNode *> getNodes(const llvm::Instruction *inst) const;
  std::vector<SyncNode *> getNodes(const llvm::Instruction *inst,
                                   ThreadID tid) const;
  const std::vector<SyncNode *> &getAllNodes() const { return m_all_nodes; }

  // Thread management
  void addThread(ThreadID tid, const llvm::Function *entry);
  const llvm::Function *getThreadEntry(ThreadID tid) const;
  std::vector<ThreadID> getAllThreads() const;

  // Entry and exit nodes
  void setThreadEntryNode(ThreadID tid, SyncNode *entry);
  void setThreadExitNode(ThreadID tid, SyncNode *exit);
  SyncNode *getThreadEntryNode(ThreadID tid) const;
  SyncNode *getThreadExitNode(ThreadID tid) const;
  std::vector<SyncNode *> getThreadExitNodes(ThreadID tid) const;

  // Graph construction helpers
  void addIntraThreadEdge(SyncNode *from, SyncNode *to);
  void addInterThreadEdge(SyncNode *from, SyncNode *to);
  void addCallEdge(SyncNode *call_site, SyncNode *callee_entry);
  void addRetEdge(SyncNode *callee_exit, SyncNode *return_site);
  void addInterThreadEdge(SyncNode *from, SyncNode *to, EdgeKind kind);
  std::optional<EdgeKind> getEdgeKind(const SyncNode *from,
                                      const SyncNode *to) const;
  bool hasEdgeKind(const SyncNode *from, const SyncNode *to,
                   EdgeKind kind) const;

  // Per-function exit node (for ret edges); key is (ThreadID, Function*)
  void setFunctionExitNode(ThreadID tid, const llvm::Function *func,
                           SyncNode *exit_node, CallContextID ctx = 0);
  SyncNode *getFunctionExitNode(ThreadID tid, const llvm::Function *func,
                                CallContextID ctx = 0) const;
  std::vector<SyncNode *> getFunctionExitNodes(ThreadID tid,
                                               const llvm::Function *func,
                                               CallContextID ctx = 0) const;

  // Query interface
  std::vector<SyncNode *> getNodesOfType(SyncNodeType type) const;
  std::vector<SyncNode *> getNodesInThread(ThreadID tid) const;

  // ========================================================================
  // Reachability index (SCC/topological pruning plus graph search)
  // ========================================================================

  void buildReachabilityIndex();
  bool hasReachabilityIndex() const { return m_index_built; }
  bool hasInterThreadEdges() const { return m_has_inter_thread_edges; }
  bool canReach(const SyncNode *from, const SyncNode *to) const;
  int getTopologicalOrder(const SyncNode *node) const;
  int getGlobalTopologicalOrder(const SyncNode *node) const;
  size_t getGlobalSCCID(const SyncNode *node) const;
  bool canThreadReach(ThreadID from_tid, ThreadID to_tid) const;
  bool canNodeReachThread(const SyncNode *node, ThreadID target_tid) const;
  size_t getMaxNodeID() const { return m_next_node_id; }
  const std::vector<SyncNode *> &getTopologicalOrderNodes(ThreadID tid) const;

  // Debugging and visualization
  void print(llvm::raw_ostream &os) const;
  void printAsDot(llvm::raw_ostream &os) const;
  void dumpToFile(const std::string &filename) const;

private:
  struct InstThreadKey {
    const llvm::Instruction *inst;
    ThreadID tid;
    CallContextID ctx;

    bool operator==(const InstThreadKey &other) const {
      return inst == other.inst && tid == other.tid && ctx == other.ctx;
    }
  };

  struct InstThreadKeyHash {
    size_t operator()(const InstThreadKey &key) const {
      size_t h1 = std::hash<const void *>{}(key.inst);
      size_t h2 = std::hash<ThreadID>{}(key.tid);
      size_t h3 = std::hash<CallContextID>{}(key.ctx);
      size_t seed = h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
      return seed ^ (h3 + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }
  };

  std::vector<SyncNode *> m_all_nodes;
  std::unordered_map<InstThreadKey, SyncNode *, InstThreadKeyHash>
      m_inst_thread_to_node;
  std::unordered_map<const llvm::Instruction *, std::vector<SyncNode *>>
      m_inst_to_nodes;
  std::unordered_map<ThreadID, std::vector<SyncNode *>> m_thread_nodes;
  std::unordered_map<ThreadID, const llvm::Function *> m_thread_entries;
  std::unordered_map<ThreadID, SyncNode *> m_thread_entry_nodes;
  std::unordered_map<ThreadID, std::vector<SyncNode *>> m_thread_exit_nodes;
  std::map<std::tuple<ThreadID, const llvm::Function *, CallContextID>,
           std::vector<SyncNode *>>
      m_func_exit_nodes;

  // Reachability index structures (flat arrays indexed by node ID for O(1) lookup)
  bool m_index_built = false;
  bool m_has_inter_thread_edges = false;
  std::vector<int> m_topo_order;
  std::vector<const SyncNode *> m_scc_representative;
  std::vector<int> m_global_topo_order;
  std::vector<size_t> m_global_scc_id;
  std::unordered_map<ThreadID, unsigned> m_thread_to_bit;
  std::vector<uint64_t> m_scc_reachable_thread_mask;
  std::unordered_map<ThreadID, std::unordered_set<ThreadID>> m_thread_reachability;
  std::unordered_map<ThreadID, std::vector<SyncNode *>> m_topo_nodes;
  mutable std::vector<uint32_t> m_visit_stamp;
  mutable uint32_t m_current_stamp = 0;
  mutable std::vector<const SyncNode *> m_bfs_queue;
  // Call-context zero denotes a root thread. Reserve node ID zero so a real
  // call site can never be mistaken for the root context.
  size_t m_next_node_id = 1;

  void addEdge(SyncNode *from, SyncNode *to, EdgeKind kind);
  void invalidateReachabilityIndex();
  void buildTopologicalOrder(ThreadID tid);
  void buildSCCs(ThreadID tid);
  void buildGlobalIndex();
  bool canReachViaIndex(const SyncNode *from, const SyncNode *to) const;
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

