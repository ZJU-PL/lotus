/*
 * Parallel IFDS Solver Implementation
 *
 // Lock order (must NOT be violated):
//   1) m_global_mutex
//   2) internal mutexes of concurrent containers

 * This implements a parallel version of the IFDS tabulation algorithm with:
 * - Thread-safe data structures using shared mutexes
 * - Multiple worker threads processing worklist items in parallel
 * - Proper termination detection and synchronization
 * - Performance monitoring and statistics
 */

#include "Dataflow/IFDS/IFDSSolvers.h"

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

#include <Support/ProgressBar.h>
#include <LLVMUtils/ThreadPool.h>

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

    size_t local_edges_processed = 0;
    size_t consecutive_empty_polls = 0;
    const size_t MAX_EMPTY_POLLS = 10;  // Try multiple times before considering termination
    bool marked_as_idle = false;  // Track if this thread contributed to idle count

    try {
        while (!m_terminate_flag.load()) {
            // Get a batch of work from the worklist
            auto batch = get_worklist_batch();

            if (batch.empty()) {
                // No work found - check for termination
                consecutive_empty_polls++;
                
                if (consecutive_empty_polls >= MAX_EMPTY_POLLS) {
                    // Sleep briefly to allow other threads to add work
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                    
                    // Check one final time before considering termination
                    if (m_worklist.empty()) {
                        // Signal that this thread is idle (only if not already marked)
                        if (!marked_as_idle) {
                            size_t idle_count = m_threads_in_current_epoch.fetch_add(1) + 1;
                            marked_as_idle = true;
                            
                            // Check if all active threads are idle
                            if (idle_count >= m_active_threads.load()) {
                                // Double-check worklist is still empty
                                if (m_worklist.empty()) {
                                    // Set termination flag - all threads will exit
                                    m_terminate_flag.store(true);
                                    signal_termination();
                                    break;
                                } else {
                                    // Work appeared, reset and continue
                                    m_threads_in_current_epoch.store(0);
                                    marked_as_idle = false;
                                    consecutive_empty_polls = 0;
                                }
                            } else {
                                // Not all threads idle, wait and recheck
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                // Unmark ourselves since we're going to try again
                                m_threads_in_current_epoch.fetch_sub(1);
                                marked_as_idle = false;
                                consecutive_empty_polls = 0;
                            }
                        } else {
                            // Already marked as idle, just wait
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    } else {
                        consecutive_empty_polls = 0;
                        // Found work in worklist, unmark if we were idle
                        if (marked_as_idle) {
                            m_threads_in_current_epoch.fetch_sub(1);
                            marked_as_idle = false;
                        }
                    }
                }
                continue;
            }

            // Found work - reset counters and process
            consecutive_empty_polls = 0;
            // Unmark ourselves as idle if we were marked
            if (marked_as_idle) {
                m_threads_in_current_epoch.fetch_sub(1);
                marked_as_idle = false;
            }

            // Process the batch
            for (const auto& edge : batch) {
                if (m_terminate_flag.load()) break;

                const llvm::Instruction* curr = edge.target_node;
                const Fact& fact = edge.target_fact;

                // Process different instruction types
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
                    // Handle regular call instructions (not invoke)
                    if (!llvm::isa<llvm::InvokeInst>(curr)) {
                        auto it = m_call_to_callee.find(call);
                        if (it != m_call_to_callee.end()) {
                            process_call_edge(call, it->second, fact);
                        } else {
                            process_call_to_return_edge(call, fact);
                        }
                    } else {
                        // Handle invoke instructions inline
                        auto* invoke = llvm::cast<llvm::InvokeInst>(curr);
                        if (const llvm::Function* callee = invoke->getCalledFunction()) {
                            process_call_edge(call, callee, fact);
                        } else {
                            process_call_to_return_edge(call, fact);
                        }
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
    signal_termination();
}

// ============================================================================
// Core IFDS Tabulation Algorithm Methods (Thread-Safe)
// ============================================================================

template<typename Problem>
bool ParallelIFDSSolver<Problem>::propagate_path_edge(const PathEdgeType& edge) {
    /* lock hierarchy: m_global_mutex → container mutexes */
    std::lock_guard<std::mutex> guard(m_global_mutex);

    // 1. insert into global path-edge set (takes P-lock AFTER global lock)
    if (!m_path_edges.insert(edge))
        return false;

    // 2. worklist & fact caches
    m_worklist.push_back(edge);
    m_entry_facts.union_with(edge.start_node,  edge.start_fact);
    m_exit_facts.union_with (edge.target_node, edge.target_fact);

    // 3. bookkeeping for call sites
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(edge.target_node)) {
        m_path_edges_at[call].insert(edge);  // safe: still inside guard

        // apply summaries that were discovered earlier
        auto sIt = m_summary_index.find(call);
        if (sIt != m_summary_index.end()) {
            const llvm::Instruction* retSite = get_return_site(call);
            if (retSite) {
                auto callee = m_call_to_callee.count(call) ? m_call_to_callee.at(call) : nullptr;
                for (const auto& summ : sIt->second)
                    if (summ.call_fact == edge.target_fact && callee && !callee->isDeclaration()) {
                        for (const auto& rf :
                             m_problem.return_flow(call, callee,
                                                   summ.return_fact, summ.call_fact)) {
                            PathEdgeType ne(edge.start_node, edge.start_fact, retSite, rf);
                            if (m_path_edges.insert(ne))
                                m_worklist.push_back(ne);
                        }
                    }
            }
        }
    }
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
    std::vector<PathEdgeType> new_edges;
    
    // ALWAYS generate call-to-return edges (textbook IFDS requirement)
    process_call_to_return_edge(call, fact);
    
    if (!callee || callee->isDeclaration()) {
        // External call - only C2R flow applies (already handled above)
        return;
    }

    // Get callee entry point
    const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();

    // Apply call flow function
    FactSet call_facts = m_problem.call_flow(call, callee, fact);

    for (const auto& call_fact : call_facts) {
        new_edges.emplace_back(call, fact, callee_entry, call_fact);
    }

    add_edges_to_worklist(new_edges);
    new_edges.clear();

    // Check if we have existing summary edges for this call and apply retroactively
    // BUG FIX: Iterate over ALL summaries that match the call_fact
    {
        std::lock_guard<std::mutex> lock(m_global_mutex);
        auto summary_it = m_summary_index.find(call);
        if (summary_it != m_summary_index.end()) {
            const llvm::Instruction* return_site = get_return_site(call);
            if (return_site) {
                // Apply ALL summaries that have matching call_fact
                // (there may be multiple with different return_facts)
                for (const auto& summary : summary_it->second) {
                    if (summary.call_fact == fact) {
                        // Apply this summary with CORRECT fact parameter
                        // The 4th parameter must be summary.call_fact, not the incoming fact
                        FactSet return_facts = m_problem.return_flow(call, callee,
                                                                   summary.return_fact, summary.call_fact);
                        for (const auto& return_fact : return_facts) {
                            new_edges.emplace_back(call, fact, return_site, return_fact);
                        }
                    }
                    // Note: we continue iterating to apply ALL matching summaries
                }
            }
        }
    } // lock released here

    // Add new edges outside the critical section to avoid deadlock
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

    for (const llvm::CallInst* call : it->second) {
        const llvm::Instruction* return_site = get_return_site(call);
        if (!return_site) continue;
        
        // Apply summary to all existing path edges ending at this call (retroactive application)
        // This is the core IFDS tabulation: for each path edge <s_p, d_1> -> <n, d_2> 
        // reaching call site n, and a summary edge <n, d_2, d_3>, 
        // create path edge <s_p, d_1> -> <return_site, d_4> where d_4 = return_flow(d_3, d_2)
        std::vector<PathEdgeType> new_edges;
        
        // Critical section: update summary edges and collect path edges to process
        {
            std::lock_guard<std::mutex> lock(m_global_mutex);
            auto path_it = m_path_edges_at.find(call);
            if (path_it != m_path_edges_at.end()) {
                for (const auto& path_edge : path_it->second) {
                    // path_edge.target_fact is the call_fact (d_2)
                    const Fact& call_fact = path_edge.target_fact;
                    
                    // Create summary edge with the actual call_fact
                    SummaryEdgeType new_summary(call, call_fact, fact);
                    
                    // Only process if this is a new summary
                    if (m_summary_edges.insert(new_summary)) {
                        // Add to index for fast lookup (set automatically deduplicates)
                        m_summary_index[call].insert(new_summary);
                        
                        // Apply return flow: exit_fact is 'fact', call_fact is from path edge
                        FactSet return_facts = m_problem.return_flow(call, func, fact, call_fact);
                        for (const auto& return_fact : return_facts) {
                            // Create path edge from the start of the calling context
                            new_edges.emplace_back(path_edge.start_node, path_edge.start_fact,
                                                 return_site, return_fact);
                        }
                    }
                }
            }
        } // lock released here
        
        // Add new edges outside the critical section to avoid deadlock
        if (!new_edges.empty()) {
            add_edges_to_worklist(new_edges);
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
    // Use bulk pop for better performance (single lock acquisition)
    return m_worklist.pop_batch(m_config.worklist_batch_size);
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
    // Termination is controlled by the termination flag set by epoch-based detection
    return m_terminate_flag.load();
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
    // If call is not a terminator, next instruction is the return site
    if (!call->isTerminator()) {
        return call->getNextNode();
    }
    
    // If call is a terminator (e.g., invoke), the return site is in a successor block
    // For invoke instructions, normal return is to the "normal" destination
    if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
        return &invoke->getNormalDest()->front();
    }
    
    // For other terminating calls, we may have multiple successors
    // Return the first successor's first instruction (conservative)
    const llvm::BasicBlock* parent = call->getParent();
    if (parent && parent->getTerminator() == call) {
        auto* term = parent->getTerminator();
        if (term->getNumSuccessors() > 0) {
            return &term->getSuccessor(0)->front();
        }
    }
    
    return nullptr;
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
                    // Handle regular call instructions
                    if (const llvm::Function* callee = call->getCalledFunction()) {
                        m_call_to_callee[call] = callee;
                        m_callee_to_calls[callee].push_back(call);
                    }
                }
                // Note: InvokeInst handling is done inline in worker thread 
                // since InvokeInst and CallInst are separate types in modern LLVM
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
                    // Handle branch instructions
                    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                        succs.push_back(&br->getSuccessor(i)->front());
                    }
                } else if (auto* sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
                    // Handle switch instructions
                    for (unsigned i = 0; i < sw->getNumSuccessors(); ++i) {
                        succs.push_back(&sw->getSuccessor(i)->front());
                    }
                } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    // Handle invoke instructions (both normal and exceptional edges)
                    // Normal destination (non-exceptional return)
                    succs.push_back(&invoke->getNormalDest()->front());
                    // Exceptional destination (unwind/exception handler)
                    succs.push_back(&invoke->getUnwindDest()->front());
                } else if (llvm::isa<llvm::ReturnInst>(&inst) || 
                          llvm::isa<llvm::UnreachableInst>(&inst)) {
                    // Returns and unreachable have no intraprocedural successors
                } else if (const llvm::Instruction* next = inst.getNextNode()) {
                    // Regular sequential flow
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

    // Initialize termination detection state
    m_terminate_flag.store(false);
    m_current_epoch.store(0);
    m_threads_in_current_epoch.store(0);

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
            // Handle regular call instructions (not invoke)
            if (!llvm::isa<llvm::InvokeInst>(curr)) {
                auto it = m_call_to_callee.find(call);
                if (it != m_call_to_callee.end()) {
                    process_call_edge(call, it->second, fact);
                } else {
                    process_call_to_return_edge(call, fact);
                }
            } else {
                // Handle invoke instructions inline
                auto* invoke = llvm::cast<llvm::InvokeInst>(curr);
                if (const llvm::Function* callee = invoke->getCalledFunction()) {
                    process_call_edge(call, callee, fact);
                } else {
                    process_call_to_return_edge(call, fact);
                }
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
// Explicit Template Instantiations
// ============================================================================

} // namespace ifds

// Explicit instantiation for commonly used solver(s)
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
template class ifds::ParallelIFDSSolver<ifds::TaintAnalysis>;
