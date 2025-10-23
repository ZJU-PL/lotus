/*
 * Parallel IFDS Solver Implementation
 *
 * This implements a parallel version of the IFDS tabulation algorithm with:
 * - Thread-safe data structures using shared mutexes
 * - Multiple worker threads processing worklist items in parallel
 * - Proper termination detection and synchronization
 * - Performance monitoring and statistics
 */

#include <Analysis/IFDS/IFDSSolvers.h>

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

#include <Support/ProgressBar.h>
#include <LLVMUtils/ThreadPool.h>

//#include <algorithm>
#include <random>
//#include <set>

namespace ifds {

// ============================================================================
// ParallelIFDSSolver Implementation
// ============================================================================

template<typename Problem>
ParallelIFDSSolver<Problem>::ParallelIFDSSolver(Problem& problem, const ParallelIFDSConfig& config)
    : m_problem(problem), m_config(config) {
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::solve(const llvm::Module& module) {
    // Record start time for performance statistics
    m_start_time = std::chrono::steady_clock::now();

    // Initialize data structures
    initialize_call_graph(module);
    build_cfg_successors(module);

    // Initialize worklist with entry facts
    initialize_worklist(module);

    // Record initial worklist size for statistics
    m_stats.max_worklist_size = m_worklist.size();

    // Run parallel tabulation algorithm
    run_parallel_tabulation();

    // Calculate performance statistics
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - m_start_time);
    m_stats.total_time_seconds = duration.count() / 1e6;
    m_stats.total_edges_processed = m_edges_processed.load();
    m_stats.total_path_edges = m_path_edges.size();
    m_stats.total_summary_edges = m_summary_edges.size();

    if (m_stats.total_time_seconds > 0) {
        m_stats.average_edges_per_second = m_stats.total_edges_processed / m_stats.total_time_seconds;
    }
}

template<typename Problem>
typename ParallelIFDSSolver<Problem>::FactSet
ParallelIFDSSolver<Problem>::get_facts_at_entry(const llvm::Instruction* inst) const {
    auto result = m_entry_facts.get(inst);
    return result.has_value() ? result.value() : FactSet{};
}

template<typename Problem>
typename ParallelIFDSSolver<Problem>::FactSet
ParallelIFDSSolver<Problem>::get_facts_at_exit(const llvm::Instruction* inst) const {
    auto result = m_exit_facts.get(inst);
    return result.has_value() ? result.value() : FactSet{};
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const {
    out_edges.clear();
    m_path_edges.for_each([&out_edges](const PathEdgeType& edge) {
        out_edges.push_back(edge);
    });
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const {
    out_edges.clear();
    m_summary_edges.for_each([&out_edges](const SummaryEdgeType& edge) {
        out_edges.push_back(edge);
    });
}

template<typename Problem>
bool ParallelIFDSSolver<Problem>::fact_reaches(const Fact& fact, const llvm::Instruction* inst) const {
    auto result = m_exit_facts.get(inst);
    return result.has_value() && result.value().find(fact) != result.value().end();
}

template<typename Problem>
std::unordered_map<typename ParallelIFDSSolver<Problem>::Node,
                  typename ParallelIFDSSolver<Problem>::FactSet,
                  typename ParallelIFDSSolver<Problem>::NodeHash>
ParallelIFDSSolver<Problem>::get_all_results() const {
    std::unordered_map<Node, FactSet, NodeHash> results;
    typename Problem::FactType zero = m_problem.zero_fact();

    m_exit_facts.for_each([&results, &zero](const auto& pair) {
        const llvm::Instruction* inst = pair.first;
        const FactSet& facts = pair.second;
        if (!facts.empty()) {
            results[Node(inst, zero)] = facts;
        }
    });

    return results;
}

template<typename Problem>
typename ParallelIFDSSolver<Problem>::FactSet
ParallelIFDSSolver<Problem>::get_facts_at(const Node& node) const {
    return get_facts_at_exit(node.instruction);
}

// ============================================================================
// Worker Thread Implementation
// ============================================================================

template<typename Problem>
void ParallelIFDSSolver<Problem>::worker_thread_function() {
    m_active_threads.fetch_add(1);

    // Seed random number generator for work stealing
    std::random_device rd;
    std::mt19937 gen(rd());

    size_t local_edges_processed = 0;

    try {
        while (!should_terminate()) {
            // Get a batch of work from the worklist
            auto batch = get_worklist_batch();

            // If no work available, break
            if (batch.empty()) {
                break;
            }

            // Process the batch
            for (const auto& edge : batch) {
                if (should_terminate()) break;

                const llvm::Instruction* curr = edge.target_node;
                const Fact& fact = edge.target_fact;

                // Process different instruction types
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
                    auto it = m_call_to_callee.find(call);
                    if (it != m_call_to_callee.end()) {
                        process_call_edge(call, it->second, fact);
                    } else {
                        process_call_to_return_edge(call, fact);
                    }
                } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
                    process_return_edge(ret, fact);
                } else {
                    // Normal intraprocedural flow
                    auto succs = get_successors(curr);
                    for (const llvm::Instruction* succ : succs) {
                        process_normal_edge(curr, succ, fact);
                    }
                }

                local_edges_processed++;
            }

            // Update global edge count periodically
            if (local_edges_processed % m_config.sync_frequency == 0) {
                m_edges_processed.fetch_add(local_edges_processed);
                local_edges_processed = 0;

                // Update max worklist size
                size_t current_worklist_size = m_worklist.size();
                if (current_worklist_size > m_stats.max_worklist_size) {
                    m_stats.max_worklist_size = current_worklist_size;
                }
            }
        }

        // Final update of edge count
        m_edges_processed.fetch_add(local_edges_processed);

    } catch (const std::exception& e) {
        llvm::errs() << "Worker thread exception: " << e.what() << "\n";
    }

    m_active_threads.fetch_sub(1);

    // If this is the last active thread, signal termination
    if (m_active_threads.load() == 0) {
        signal_termination();
    }
}

// ============================================================================
// Core IFDS Tabulation Algorithm Methods (Thread-Safe)
// ============================================================================

template<typename Problem>
bool ParallelIFDSSolver<Problem>::propagate_path_edge(const PathEdgeType& edge) {
    // Try to insert the edge - if already exists, return false
    if (!m_path_edges.insert(edge)) {
        return false;
    }

    // Add to worklist for processing
    m_worklist.push_back(edge);

    // Update entry/exit facts for queries (thread-safe)
    m_entry_facts.insert_or_assign(edge.start_node, FactSet{edge.start_fact});
    m_exit_facts.insert_or_assign(edge.target_node, FactSet{edge.target_fact});

    return true;
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::process_normal_edge(const llvm::Instruction* curr,
                                                     const llvm::Instruction* next,
                                                     const Fact& fact) {
    FactSet new_facts = m_problem.normal_flow(curr, fact);

    std::vector<PathEdgeType> new_edges;
    for (const auto& new_fact : new_facts) {
        new_edges.emplace_back(curr, fact, next, new_fact);
    }

    add_edges_to_worklist(new_edges);
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::process_call_edge(const llvm::CallInst* call,
                                                   const llvm::Function* callee,
                                                   const Fact& fact) {
    if (!callee || callee->isDeclaration()) {
        // External call - handle with call-to-return flow
        process_call_to_return_edge(call, fact);
        return;
    }

    // Get callee entry point
    const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();

    // Apply call flow function
    FactSet call_facts = m_problem.call_flow(call, callee, fact);

    std::vector<PathEdgeType> new_edges;
    for (const auto& call_fact : call_facts) {
        new_edges.emplace_back(call, fact, callee_entry, call_fact);
    }

    add_edges_to_worklist(new_edges);

    // Check if we have existing summary edges for this call
    auto summary_it = m_summary_index.find(call);
    if (summary_it != m_summary_index.end()) {
        const llvm::Instruction* return_site = get_return_site(call);
        if (return_site) {
            for (const auto& summary : summary_it->second) {
                if (summary.call_fact == fact) {
                    // Apply existing summary
                    FactSet return_facts = m_problem.return_flow(call, callee,
                                                               summary.return_fact, fact);
                    for (const auto& return_fact : return_facts) {
                        new_edges.emplace_back(call, fact, return_site, return_fact);
                    }
                }
            }
        }
    }

    if (!new_edges.empty()) {
        add_edges_to_worklist(new_edges);
    }
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::process_return_edge(const llvm::ReturnInst* ret,
                                                     const Fact& fact) {
    const llvm::Function* func = ret->getFunction();

    // Find all call sites for this function
    auto it = m_callee_to_calls.find(func);
    if (it == m_callee_to_calls.end()) return;

    SummaryEdgeType new_summary(nullptr, m_problem.zero_fact(), fact);

    for (const llvm::CallInst* call : it->second) {
        new_summary.call_site = call;

        // Check if this summary edge already exists
        if (m_summary_edges.insert(new_summary)) {
            // Add to index for fast lookup
            m_summary_index[call].push_back(new_summary);

            // Apply summary to all existing path edges ending at this call
            auto path_it = m_path_edges_at.find(call);
            if (path_it != m_path_edges_at.end()) {
                const llvm::Instruction* return_site = get_return_site(call);
                if (return_site) {
                    std::vector<PathEdgeType> new_edges;
                    for (const auto& path_edge : path_it->second) {
                        FactSet return_facts = m_problem.return_flow(call, func,
                                                                   fact, path_edge.target_fact);
                        for (const auto& return_fact : return_facts) {
                            new_edges.emplace_back(call, path_edge.target_fact,
                                                 return_site, return_fact);
                        }
                    }
                    if (!new_edges.empty()) {
                        add_edges_to_worklist(new_edges);
                    }
                }
            }
        }
    }
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::process_call_to_return_edge(const llvm::CallInst* call,
                                                            const Fact& fact) {
    const llvm::Instruction* return_site = get_return_site(call);
    if (!return_site) return;

    FactSet ctr_facts = m_problem.call_to_return_flow(call, fact);

    std::vector<PathEdgeType> new_edges;
    for (const auto& ctr_fact : ctr_facts) {
        new_edges.emplace_back(call, fact, return_site, ctr_fact);
    }

    add_edges_to_worklist(new_edges);
}

// ============================================================================
// Worklist Management
// ============================================================================

template<typename Problem>
std::vector<typename ParallelIFDSSolver<Problem>::PathEdgeType>
ParallelIFDSSolver<Problem>::get_worklist_batch() {
    std::vector<PathEdgeType> batch;
    size_t batch_size = m_config.worklist_batch_size;

    for (size_t i = 0; i < batch_size; ++i) {
        auto edge = m_worklist.pop_back();
        if (!edge.has_value()) break;
        batch.push_back(edge.value());
    }

    return batch;
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::add_edges_to_worklist(const std::vector<PathEdgeType>& edges) {
    for (const auto& edge : edges) {
        if (propagate_path_edge(edge)) {
            // New edge was added, continue processing
        }
    }
}


// ============================================================================
// Termination and Synchronization
// ============================================================================

template<typename Problem>
bool ParallelIFDSSolver<Problem>::should_terminate() const {
    // Terminate if worklist is empty and no threads are actively processing
    return m_worklist.empty() && m_active_threads.load() == 0;
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::wait_for_termination() {
    std::unique_lock<std::mutex> lock(m_termination_mutex);
    m_termination_cv.wait(lock, [this]() {
        return should_terminate();
    });
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::signal_termination() {
    std::lock_guard<std::mutex> lock(m_termination_mutex);
    m_termination_cv.notify_all();
}

// ============================================================================
// Helper Methods
// ============================================================================

template<typename Problem>
const llvm::Instruction* ParallelIFDSSolver<Problem>::get_return_site(const llvm::CallInst* call) const {
    return call->getNextNode();
}

template<typename Problem>
std::vector<const llvm::Instruction*>
ParallelIFDSSolver<Problem>::get_successors(const llvm::Instruction* inst) const {
    auto it = m_successors.find(inst);
    if (it != m_successors.end()) {
        return it->second;
    }
    return {};
}

// ============================================================================
// Initialization Methods
// ============================================================================

template<typename Problem>
void ParallelIFDSSolver<Problem>::initialize_call_graph(const llvm::Module& module) {
    m_call_to_callee.clear();
    m_callee_to_calls.clear();
    m_function_returns.clear();

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;

        // Collect return instructions
        std::vector<const llvm::ReturnInst*> returns;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                    returns.push_back(ret);
                } else if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (const llvm::Function* callee = call->getCalledFunction()) {
                        m_call_to_callee[call] = callee;
                        m_callee_to_calls[callee].push_back(call);
                    }
                }
            }
        }
        m_function_returns[&func] = returns;
    }
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::build_cfg_successors(const llvm::Module& module) {
    m_successors.clear();
    m_predecessors.clear();

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;

        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                std::vector<const llvm::Instruction*> succs;

                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                        succs.push_back(&br->getSuccessor(i)->front());
                    }
                } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                    // Returns have no intraprocedural successors
                } else if (const llvm::Instruction* next = inst.getNextNode()) {
                    succs.push_back(next);
                }

                m_successors[&inst] = succs;

                // Build predecessors map
                for (const llvm::Instruction* succ : succs) {
                    m_predecessors[succ].push_back(&inst);
                }
            }
        }
    }
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::initialize_worklist(const llvm::Module& module) {
    m_path_edges.clear();
    m_summary_edges.clear();
    m_worklist.clear();
    m_entry_facts.clear();
    m_exit_facts.clear();
    m_summary_index.clear();
    m_path_edges_at.clear();

    // Find main function or any entry function
    const llvm::Function* main_func = get_main_function(module);
    if (!main_func) {
        for (const llvm::Function& func : module) {
            if (!func.isDeclaration() && !func.empty()) {
                main_func = &func;
                break;
            }
        }
    }

    if (main_func && !main_func->empty()) {
        const llvm::Instruction* entry = &main_func->getEntryBlock().front();
        for (const auto& fact : m_problem.initial_facts(main_func)) {
            propagate_path_edge(PathEdgeType(entry, fact, entry, fact));
        }
    }
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::run_parallel_tabulation() {
    if (!m_config.enable_parallel_processing || m_config.num_threads <= 1) {
        // Fall back to sequential processing
        run_sequential_tabulation();
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(m_config.num_threads);

    // Create worker threads
    for (size_t i = 0; i < m_config.num_threads; ++i) {
        workers.emplace_back([this]() {
            worker_thread_function();
        });
    }

    // Wait for all threads to complete
    for (auto& worker : workers) {
        worker.join();
    }
}

template<typename Problem>
void ParallelIFDSSolver<Problem>::run_sequential_tabulation() {
    // Sequential fallback implementation (simplified)
    ProgressBar* progress = nullptr;
    size_t processed_edges = 0;
    size_t last_update = 0;
    const size_t update_interval = 100;

    if (m_show_progress) {
        progress = new ProgressBar("Sequential IFDS Analysis", ProgressBar::PBS_CharacterStyle, 0.01);
        llvm::outs() << "\n";
    }

    while (!m_worklist.empty()) {
        auto edge_opt = m_worklist.pop_back();
        if (!edge_opt.has_value()) break;

        const PathEdgeType& current_edge = edge_opt.value();
        const llvm::Instruction* curr = current_edge.target_node;
        const Fact& fact = current_edge.target_fact;

        // Process different instruction types (same as worker function)
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
            auto it = m_call_to_callee.find(call);
            if (it != m_call_to_callee.end()) {
                process_call_edge(call, it->second, fact);
            } else {
                process_call_to_return_edge(call, fact);
            }
        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            process_return_edge(ret, fact);
        } else {
            auto succs = get_successors(curr);
            for (const llvm::Instruction* succ : succs) {
                process_normal_edge(curr, succ, fact);
            }
        }

        processed_edges++;

        if (m_show_progress && processed_edges - last_update >= update_interval) {
            last_update = processed_edges;
            size_t total_path_edges = m_path_edges.size();
            size_t worklist_size = m_worklist.size();

            llvm::outs() << "\r\033[KProcessed: " << processed_edges
                        << " | Path edges: " << total_path_edges
                        << " | Worklist: " << worklist_size;
            llvm::outs().flush();
        }
    }

    if (m_show_progress) {
        llvm::outs() << "\r\033[K";
        progress->showProgress(1.0);
        llvm::outs() << "\nCompleted! Processed " << processed_edges
                    << " edges, discovered " << m_path_edges.size() << " path edges\n";
        delete progress;
    }
}

template<typename Problem>
const llvm::Function* ParallelIFDSSolver<Problem>::get_main_function(const llvm::Module& module) {
    return module.getFunction("main");
}

// ============================================================================
// IFDSSolver Implementation (Sequential wrapper around ParallelIFDSSolver)
// ============================================================================

template<typename Problem>
IFDSSolver<Problem>::IFDSSolver(Problem& problem) 
    : m_problem(problem) {
    // Create sequential config
    ParallelIFDSConfig config;
    config.enable_parallel_processing = false;
    config.num_threads = 1;
    m_parallel_solver = new ParallelIFDSSolver<Problem>(problem, config);
}

template<typename Problem>
IFDSSolver<Problem>::~IFDSSolver() {
    delete m_parallel_solver;
}

template<typename Problem>
void IFDSSolver<Problem>::solve(const llvm::Module& module) {
    m_parallel_solver->solve(module);
}

template<typename Problem>
void IFDSSolver<Problem>::set_show_progress(bool show) {
    m_parallel_solver->set_show_progress(show);
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at_entry(const llvm::Instruction* inst) const {
    return m_parallel_solver->get_facts_at_entry(inst);
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at_exit(const llvm::Instruction* inst) const {
    return m_parallel_solver->get_facts_at_exit(inst);
}

template<typename Problem>
void IFDSSolver<Problem>::get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const {
    m_parallel_solver->get_path_edges(out_edges);
}

template<typename Problem>
void IFDSSolver<Problem>::get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const {
    m_parallel_solver->get_summary_edges(out_edges);
}

template<typename Problem>
bool IFDSSolver<Problem>::fact_reaches(const Fact& fact, const llvm::Instruction* inst) const {
    return m_parallel_solver->fact_reaches(fact, inst);
}

template<typename Problem>
std::unordered_map<typename IFDSSolver<Problem>::Node, 
                   typename IFDSSolver<Problem>::FactSet, 
                   typename IFDSSolver<Problem>::NodeHash>
IFDSSolver<Problem>::get_all_results() const {
    return m_parallel_solver->get_all_results();
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at(const Node& node) const {
    return m_parallel_solver->get_facts_at(node);
}

// ============================================================================
// Explicit Template Instantiations
// ============================================================================

// Provide explicit instantiation for commonly used solver(s)

} // namespace ifds

// Explicit instantiation for commonly used solver(s)
#include <Analysis/IFDS/Clients/IFDSTaintAnalysis.h>
template class ifds::ParallelIFDSSolver<ifds::TaintAnalysis>;
template class ifds::IFDSSolver<ifds::TaintAnalysis>;
