/*
 * IFDS Solvers
 *
 * This header provides IFDS solver implementations:
 * - IFDSSolver: Sequential IFDS solver
 * - ParallelIFDSSolver: Parallel IFDS solver with multi-threading support
 */

#pragma once

#include "Dataflow/IFDS/IFDSFramework.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace ifds {

// Forward declarations
template<typename Problem> class ParallelIFDSSolver;

// ============================================================================
// IFDS Solver (Sequential)
// ============================================================================

template<typename Problem>
class IFDSSolver {
public:
    using Fact = typename Problem::FactType;
    using FactSet = typename Problem::FactSet;
    using Node = typename ExplodedSupergraph<Fact>::Node;
    using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;

    IFDSSolver(Problem& problem);

    void solve(const llvm::Module& module);

    // Enable/disable progress bar display during analysis
    void set_show_progress(bool show) { m_show_progress = show; }

    // Query interface for analysis results
    FactSet get_facts_at_entry(const llvm::Instruction* inst) const;
    FactSet get_facts_at_exit(const llvm::Instruction* inst) const;

    // Get all path edges (for debugging/analysis)
    void get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const;

    // Get all summary edges (for debugging/analysis)
    void get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const;

    // Check if a fact reaches a specific instruction
    bool fact_reaches(const Fact& fact, const llvm::Instruction* inst) const;

    // Legacy compatibility methods for existing tools
    std::unordered_map<Node, FactSet, NodeHash> get_all_results() const;
    FactSet get_facts_at(const Node& node) const;

private:
    using PathEdgeType = PathEdge<Fact>;
    using SummaryEdgeType = SummaryEdge<Fact>;

    Problem& m_problem;
    bool m_show_progress = false;

    // Simple sequential data structures (no thread-safety needed)
    std::set<PathEdgeType> m_path_edges;
    std::set<SummaryEdgeType> m_summary_edges;
    std::vector<PathEdgeType> m_worklist;
    std::unordered_map<const llvm::Instruction*, FactSet> m_entry_facts;
    std::unordered_map<const llvm::Instruction*, FactSet> m_exit_facts;

    // Indexed data structures for fast lookup
    std::unordered_map<const llvm::CallInst*, std::set<SummaryEdgeType>> m_summary_index;
    std::unordered_map<const llvm::Instruction*, std::set<PathEdgeType>> m_path_edges_at;

    // Call graph information (read-only after initialization)
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> m_call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> m_callee_to_calls;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::ReturnInst*>> m_function_returns;

    // CFG navigation helpers (read-only after initialization)
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_successors;
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_predecessors;

    // Core IFDS Tabulation Algorithm Methods
    bool propagate_path_edge(const PathEdgeType& edge);
    void process_normal_edge(const llvm::Instruction* curr, const llvm::Instruction* next, const Fact& fact);
    void process_call_edge(const llvm::CallInst* call, const llvm::Function* callee, const Fact& fact);
    void process_return_edge(const llvm::ReturnInst* ret, const Fact& fact);
    void process_call_to_return_edge(const llvm::CallInst* call, const Fact& fact);

    // Helper methods
    const llvm::Instruction* get_return_site(const llvm::CallInst* call) const;
    std::vector<const llvm::Instruction*> get_successors(const llvm::Instruction* inst) const;

    // Initialization methods
    void initialize_call_graph(const llvm::Module& module);
    void build_cfg_successors(const llvm::Module& module);
    void initialize_worklist(const llvm::Module& module);
    void run_tabulation();

    const llvm::Function* get_main_function(const llvm::Module& module);
};

// ============================================================================
// Parallel IFDS Solver
// ============================================================================

template<typename Problem>
class ParallelIFDSSolver {
public:
    using Fact = typename Problem::FactType;
    using FactSet = typename Problem::FactSet;
    using Node = typename ExplodedSupergraph<Fact>::Node;
    using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;

    explicit ParallelIFDSSolver(Problem& problem, const ParallelIFDSConfig& config = ParallelIFDSConfig{});

    void solve(const llvm::Module& module);

    // Enable/disable progress bar display during analysis
    void set_show_progress(bool show) { m_show_progress = show; }

    // Configuration methods
    void set_config(const ParallelIFDSConfig& config) { m_config = config; }
    const ParallelIFDSConfig& get_config() const { return m_config; }

    // Query interface for analysis results
    FactSet get_facts_at_entry(const llvm::Instruction* inst) const;
    FactSet get_facts_at_exit(const llvm::Instruction* inst) const;

    // Get all path edges (for debugging/analysis)
    void get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const;

    // Get all summary edges (for debugging/analysis)
    void get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const;

    // Check if a fact reaches a specific instruction
    bool fact_reaches(const Fact& fact, const llvm::Instruction* inst) const;

    // Legacy compatibility methods for existing tools
    std::unordered_map<Node, FactSet, NodeHash> get_all_results() const;
    FactSet get_facts_at(const Node& node) const;

    // Performance statistics
    struct PerformanceStats {
        double total_time_seconds = 0.0;
        size_t total_edges_processed = 0;
        size_t total_path_edges = 0;
        size_t total_summary_edges = 0;
        double average_edges_per_second = 0.0;
        size_t max_worklist_size = 0;
    };
    PerformanceStats get_performance_stats() const { return m_stats; }

private:
    using PathEdgeType = PathEdge<Fact>;
    using SummaryEdgeType = SummaryEdge<Fact>;

    Problem& m_problem;
    ParallelIFDSConfig m_config;
    bool m_show_progress = false;

    // Thread-safe data structures with sharding for reduced lock contention
    ThreadSafeSet<PathEdgeType> m_path_edges;
    ThreadSafeSet<SummaryEdgeType> m_summary_edges;
    ThreadSafeVector<PathEdgeType> m_worklist;
    ShardedMap<const llvm::Instruction*, FactSet> m_entry_facts;
    ShardedMap<const llvm::Instruction*, FactSet> m_exit_facts;

    // Indexed data structures for fast lookup - use sets to avoid duplicates
    // Protected by sharded mutexes (computed per call site)
    std::unordered_map<const llvm::CallInst*, std::set<SummaryEdgeType>> m_summary_index;
    std::unordered_map<const llvm::Instruction*, std::set<PathEdgeType>> m_path_edges_at;

    // Call graph information (read-only after initialization)
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> m_call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> m_callee_to_calls;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::ReturnInst*>> m_function_returns;

    // CFG navigation helpers (read-only after initialization)
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_successors;
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_predecessors;

    // Synchronization and termination
    mutable std::mutex m_global_mutex;
    std::atomic<size_t> m_active_threads{0};
    std::atomic<size_t> m_edges_processed{0};
    std::condition_variable m_termination_cv;
    mutable std::mutex m_termination_mutex;
    std::atomic<bool> m_terminate_flag{false};
    
    // Epoch-based termination detection
    std::atomic<size_t> m_current_epoch{0};
    std::atomic<size_t> m_threads_in_current_epoch{0};

    // Performance tracking
    mutable PerformanceStats m_stats;
    std::chrono::steady_clock::time_point m_start_time;

    // Worker thread function
    void worker_thread_function();

    // Core IFDS Tabulation Algorithm Methods (thread-safe versions)
    bool propagate_path_edge(const PathEdgeType& edge);
    void process_normal_edge(const llvm::Instruction* curr, const llvm::Instruction* next, const Fact& fact);
    void process_call_edge(const llvm::CallInst* call, const llvm::Function* callee, const Fact& fact);
    void process_return_edge(const llvm::ReturnInst* ret, const Fact& fact);
    void process_call_to_return_edge(const llvm::CallInst* call, const Fact& fact);

    // Worklist operations
    std::vector<PathEdgeType> get_worklist_batch();
    void add_edges_to_worklist(const std::vector<PathEdgeType>& edges);

    // Termination and synchronization
    bool should_terminate() const;
    void wait_for_termination();
    void signal_termination();

    // Helper methods
    const llvm::Instruction* get_return_site(const llvm::CallInst* call) const;
    std::vector<const llvm::Instruction*> get_successors(const llvm::Instruction* inst) const;

    // Initialization methods
    void initialize_call_graph(const llvm::Module& module);
    void build_cfg_successors(const llvm::Module& module);
    void initialize_worklist(const llvm::Module& module);
    void run_parallel_tabulation();
    void run_sequential_tabulation();

    const llvm::Function* get_main_function(const llvm::Module& module);
};

} // namespace ifds
