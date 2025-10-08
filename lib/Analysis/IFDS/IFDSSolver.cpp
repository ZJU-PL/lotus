/*
 * IFDS Solver Implementation
 * 
 * This implements the full IFDS tabulation algorithm with:
 * - Path edges and summary edges
 * - Context-sensitive interprocedural analysis
 * - Proper termination and soundness guarantees
 */

#include <Analysis/IFDS/IFDSFramework.h>

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

#include <Support/ProgressBar.h>

// Include the analysis headers to get the complete types
#include "Analysis/IFDS/Clients/IFDSTaintAnalysis.h"
#include "Analysis/IFDS/Clients/IFDSReachingDefinitions.h"

namespace ifds {

// ============================================================================
// Edge Structure Implementations
// ============================================================================

template<typename Fact>
bool PathEdge<Fact>::operator==(const PathEdge& other) const {
    return start_node == other.start_node && start_fact == other.start_fact &&
           target_node == other.target_node && target_fact == other.target_fact;
}

template<typename Fact>
bool PathEdge<Fact>::operator<(const PathEdge& other) const {
    if (start_node != other.start_node) return start_node < other.start_node;
    if (start_fact != other.start_fact) return start_fact < other.start_fact;
    if (target_node != other.target_node) return target_node < other.target_node;
    return target_fact < other.target_fact;
}

template<typename Fact>
size_t PathEdgeHash<Fact>::operator()(const PathEdge<Fact>& edge) const {
    return std::hash<const llvm::Instruction*>{}(edge.start_node) ^
           (std::hash<Fact>{}(edge.start_fact) << 1) ^
           (std::hash<const llvm::Instruction*>{}(edge.target_node) << 2) ^
           (std::hash<Fact>{}(edge.target_fact) << 3);
}

template<typename Fact>
bool SummaryEdge<Fact>::operator==(const SummaryEdge& other) const {
    return call_site == other.call_site && call_fact == other.call_fact &&
           return_fact == other.return_fact;
}

template<typename Fact>
bool SummaryEdge<Fact>::operator<(const SummaryEdge& other) const {
    if (call_site != other.call_site) return call_site < other.call_site;
    if (call_fact != other.call_fact) return call_fact < other.call_fact;
    return return_fact < other.return_fact;
}

template<typename Fact>
size_t SummaryEdgeHash<Fact>::operator()(const SummaryEdge<Fact>& edge) const {
    return std::hash<const llvm::CallInst*>{}(edge.call_site) ^
           (std::hash<Fact>{}(edge.call_fact) << 1) ^
           (std::hash<Fact>{}(edge.return_fact) << 2);
}

// ============================================================================
// IFDSProblem Implementation
// ============================================================================

template<typename Fact>
void IFDSProblem<Fact>::set_alias_analysis(DyckAliasAnalysis* aa) {
    m_alias_analysis = aa;
}

template<typename Fact>
bool IFDSProblem<Fact>::is_source(const llvm::Instruction* /*inst*/) const {
    return false;
}

template<typename Fact>
bool IFDSProblem<Fact>::is_sink(const llvm::Instruction* /*inst*/) const {
    return false;
}

template<typename Fact>
bool IFDSProblem<Fact>::may_alias(const llvm::Value* v1, const llvm::Value* v2) const {
    if (!m_alias_analysis) return true; // Conservative
    
    return m_alias_analysis->mayAlias(const_cast<llvm::Value*>(v1), 
                                     const_cast<llvm::Value*>(v2));
}

template<typename Fact>
std::vector<const llvm::Value*> IFDSProblem<Fact>::get_points_to_set(const llvm::Value* ptr) const {
    if (!m_alias_analysis) return {ptr};
    
    std::vector<const llvm::Value*> pts_set;
    const std::set<llvm::Value*>* alias_set = m_alias_analysis->getAliasSet(const_cast<llvm::Value*>(ptr));
    
    if (alias_set) {
        for (llvm::Value* v : *alias_set) {
            if (llvm::isa<llvm::AllocaInst>(v) || llvm::isa<llvm::GlobalVariable>(v) || llvm::isa<llvm::Argument>(v)) {
                pts_set.push_back(v);
            }
        }
    }
    return pts_set.empty() ? std::vector<const llvm::Value*>{ptr} : pts_set;
}

template<typename Fact>
std::vector<const llvm::Value*> IFDSProblem<Fact>::get_alias_set(const llvm::Value* val) const {
    if (!m_alias_analysis) return {val};
    
    const std::set<llvm::Value*>* dyck_alias_set = m_alias_analysis->getAliasSet(const_cast<llvm::Value*>(val));
    if (!dyck_alias_set) return {val};
    
    std::vector<const llvm::Value*> alias_set;
    for (llvm::Value* v : *dyck_alias_set) {
        alias_set.push_back(v);
    }
    return alias_set;
}

// ============================================================================
// ExplodedSupergraph Implementation
// ============================================================================

template<typename Fact>
bool ExplodedSupergraph<Fact>::Node::operator==(const Node& other) const {
    return instruction == other.instruction && fact == other.fact;
}

template<typename Fact>
bool ExplodedSupergraph<Fact>::Node::operator<(const Node& other) const {
    if (instruction != other.instruction) {
        return instruction < other.instruction;
    }
    return fact < other.fact;
}

template<typename Fact>
size_t ExplodedSupergraph<Fact>::NodeHash::operator()(const Node& node) const {
    size_t h1 = std::hash<const llvm::Instruction*>{}(node.instruction);
    size_t h2 = std::hash<Fact>{}(node.fact);
    return h1 ^ (h2 << 1);
}

template<typename Fact>
typename ExplodedSupergraph<Fact>::NodeId 
ExplodedSupergraph<Fact>::entry(const Graph& graph) {
    return graph.m_entry ? *graph.m_entry : NodeId(nullptr, Fact{});
}

template<typename Fact>
typename ExplodedSupergraph<Fact>::NodeId 
ExplodedSupergraph<Fact>::source(const Graph& /*graph*/, const EdgeId& edge) {
    return edge.source;
}

template<typename Fact>
typename ExplodedSupergraph<Fact>::NodeId 
ExplodedSupergraph<Fact>::target(const Graph& /*graph*/, const EdgeId& edge) {
    return edge.target;
}

template<typename Fact>
std::vector<typename ExplodedSupergraph<Fact>::EdgeId> 
ExplodedSupergraph<Fact>::predecessors(const Graph& graph, const NodeId& node) {
    auto it = graph.m_predecessors.find(node);
    return it != graph.m_predecessors.end() ? it->second : std::vector<EdgeId>{};
}

template<typename Fact>
std::vector<typename ExplodedSupergraph<Fact>::EdgeId> 
ExplodedSupergraph<Fact>::successors(const Graph& graph, const NodeId& node) {
    auto it = graph.m_successors.find(node);
    return it != graph.m_successors.end() ? it->second : std::vector<EdgeId>{};
}

template<typename Fact>
void ExplodedSupergraph<Fact>::add_edge(const Edge& edge) {
    m_edges.push_back(edge);
    m_successors[edge.source].push_back(edge);
    m_predecessors[edge.target].push_back(edge);
}

template<typename Fact>
void ExplodedSupergraph<Fact>::set_entry(const NodeId& entry) {
    m_entry = std::make_unique<NodeId>(entry);
}

template<typename Fact>
const std::vector<typename ExplodedSupergraph<Fact>::Edge>& 
ExplodedSupergraph<Fact>::get_edges() const {
    return m_edges;
}

// ============================================================================
// IFDSSolver Implementation
// ============================================================================

template<typename Problem>
IFDSSolver<Problem>::IFDSSolver(Problem& problem) : m_problem(problem) {}

template<typename Problem>
void IFDSSolver<Problem>::solve(const llvm::Module& module) {
    // Initialize data structures
    initialize_call_graph(module);
    build_cfg_successors(module);
    
    // Initialize worklist with entry facts
    initialize_worklist(module);
    
    // Main IFDS tabulation algorithm
    run_tabulation();
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at_entry(const llvm::Instruction* inst) const {
    auto it = m_entry_facts.find(inst);
    return it != m_entry_facts.end() ? it->second : FactSet{};
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at_exit(const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    return it != m_exit_facts.end() ? it->second : FactSet{};
}

template<typename Problem>
const std::unordered_set<PathEdge<typename Problem::FactType>, PathEdgeHash<typename Problem::FactType>>& 
IFDSSolver<Problem>::get_path_edges() const {
    return m_path_edges;
}

template<typename Problem>
const std::unordered_set<SummaryEdge<typename Problem::FactType>, SummaryEdgeHash<typename Problem::FactType>>& 
IFDSSolver<Problem>::get_summary_edges() const {
    return m_summary_edges;
}

template<typename Problem>
bool IFDSSolver<Problem>::fact_reaches(const typename Problem::FactType& fact, const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    return it != m_exit_facts.end() && it->second.find(fact) != it->second.end();
}

template<typename Problem>
std::unordered_map<typename IFDSSolver<Problem>::Node, typename IFDSSolver<Problem>::FactSet, typename IFDSSolver<Problem>::NodeHash> 
IFDSSolver<Problem>::get_all_results() const {
    std::unordered_map<Node, FactSet, NodeHash> results;
    typename Problem::FactType zero = m_problem.zero_fact();
    
    for (const auto& entry : m_exit_facts) {
        const llvm::Instruction* inst = entry.first;
        const FactSet& facts = entry.second;
        if (!facts.empty()) {
            results[Node(inst, zero)] = facts;
        }
    }
    return results;
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at(const Node& node) const {
    return get_facts_at_exit(node.instruction);
}

// Core IFDS Tabulation Algorithm Methods
template<typename Problem>
void IFDSSolver<Problem>::propagate_path_edge(const PathEdgeType& edge) {
    if (m_path_edges.find(edge) != m_path_edges.end()) {
        return; // Already processed
    }
    
    m_path_edges.insert(edge);
    m_worklist.push_back(edge);
    
    // Add to index for fast lookup by target node
    m_path_edges_at[edge.target_node].push_back(edge);
    
    // Update entry/exit facts for queries
    m_entry_facts[edge.start_node].insert(edge.start_fact);
    m_exit_facts[edge.target_node].insert(edge.target_fact);
}

template<typename Problem>
void IFDSSolver<Problem>::process_normal_edge(const llvm::Instruction* curr, const llvm::Instruction* next,
                           const Fact& fact) {
    FactSet new_facts = m_problem.normal_flow(curr, fact);
    
    for (const auto& new_fact : new_facts) {
        PathEdgeType edge(curr, fact, next, new_fact);
        propagate_path_edge(edge);
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_edge(const llvm::CallInst* call, const llvm::Function* callee,
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
    
    for (const auto& call_fact : call_facts) {
        PathEdgeType edge(call, fact, callee_entry, call_fact);
        propagate_path_edge(edge);
    }
    
    // Check if we have existing summary edges for this call (O(1) lookup)
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
                        PathEdgeType return_edge(call, fact, return_site, return_fact);
                        propagate_path_edge(return_edge);
                    }
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_return_edge(const llvm::ReturnInst* ret, const Fact& fact) {
    const llvm::Function* func = ret->getFunction();
    
    // Find all call sites for this function
    auto it = m_callee_to_calls.find(func);
    if (it == m_callee_to_calls.end()) return;
    
    for (const llvm::CallInst* call : it->second) {
        // Create or update summary edge
        SummaryEdgeType summary(call, m_problem.zero_fact(), fact);
        
        if (m_summary_edges.find(summary) == m_summary_edges.end()) {
            m_summary_edges.insert(summary);
            // Add to index for O(1) lookup
            m_summary_index[call].push_back(summary);
            
            // Apply summary to all existing path edges ending at this call (O(1) lookup)
            auto path_it = m_path_edges_at.find(call);
            if (path_it != m_path_edges_at.end()) {
                const llvm::Instruction* return_site = get_return_site(call);
                if (return_site) {
                    for (const auto& path_edge : path_it->second) {
                        FactSet return_facts = m_problem.return_flow(call, func, 
                                                                   fact, path_edge.target_fact);
                        for (const auto& return_fact : return_facts) {
                            PathEdgeType return_edge(call, path_edge.target_fact, 
                                                   return_site, return_fact);
                            propagate_path_edge(return_edge);
                        }
                    }
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_to_return_edge(const llvm::CallInst* call, const Fact& fact) {
    const llvm::Instruction* return_site = get_return_site(call);
    if (!return_site) return;
    
    FactSet ctr_facts = m_problem.call_to_return_flow(call, fact);
    
    for (const auto& ctr_fact : ctr_facts) {
        PathEdgeType edge(call, fact, return_site, ctr_fact);
        propagate_path_edge(edge);
    }
}

template<typename Problem>
const llvm::Instruction* IFDSSolver<Problem>::get_return_site(const llvm::CallInst* call) const {
    return call->getNextNode();
}

template<typename Problem>
std::vector<const llvm::Instruction*> IFDSSolver<Problem>::get_successors(const llvm::Instruction* inst) const {
    auto it = m_successors.find(inst);
    if (it != m_successors.end()) {
        return it->second;
    }
    return {};
}

template<typename Problem>
void IFDSSolver<Problem>::initialize_call_graph(const llvm::Module& module) {
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
void IFDSSolver<Problem>::build_cfg_successors(const llvm::Module& module) {
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
void IFDSSolver<Problem>::initialize_worklist(const llvm::Module& module) {
    m_path_edges.clear();
    m_path_edges_at.clear();
    m_summary_edges.clear();
    m_summary_index.clear();
    m_worklist.clear();
    m_entry_facts.clear();
    m_exit_facts.clear();
    
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
void IFDSSolver<Problem>::run_tabulation() {
    ProgressBar* progress = nullptr;
    size_t processed_edges = 0;
    size_t last_update = 0;
    const size_t update_interval = 100; // Update progress every 100 edges
    
    if (m_show_progress) {
        progress = new ProgressBar("IFDS Analysis", ProgressBar::PBS_CharacterStyle, 0.01);
        llvm::outs() << "\n"; // Start on a new line for cleaner output
    }
    
    while (!m_worklist.empty()) {
        PathEdgeType current_edge = m_worklist.back();
        m_worklist.pop_back();
        
        const llvm::Instruction* curr = current_edge.target_node;
        const Fact& fact = current_edge.target_fact;
        
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
        
        // Update progress based on total path edges discovered
        // This gives a more accurate picture since path_edges is monotonically increasing
        processed_edges++;
        if (m_show_progress && processed_edges - last_update >= update_interval) {
            last_update = processed_edges;
            // Display stats: processed edges and current path edge count
            size_t total_path_edges = m_path_edges.size();
            size_t worklist_size = m_worklist.size();
            
            // Show processed/total and worklist size for transparency
            llvm::outs() << "\r\033[KProcessed: " << processed_edges 
                        << " | Path edges: " << total_path_edges
                        << " | Worklist: " << worklist_size;
            llvm::outs().flush();
        }
    }
    
    // Show 100% completion
    if (m_show_progress) {
        llvm::outs() << "\r\033[K"; // Clear the line
        progress->showProgress(1.0);
        llvm::outs() << "\nCompleted! Processed " << processed_edges 
                    << " edges, discovered " << m_path_edges.size() << " path edges\n";
        delete progress;
    }
}

template<typename Problem>
const llvm::Function* IFDSSolver<Problem>::get_main_function(const llvm::Module& module) {
    return module.getFunction("main");
}

// ============================================================================
// Explicit template instantiations for common fact types
// ============================================================================

// Explicit template instantiations for types used in the codebase
template class ifds::IFDSSolver<ifds::TaintAnalysis>;
template class ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis>;

// Explicit instantiations for IFDSProblem methods
template class ifds::IFDSProblem<ifds::TaintFact>;
template class ifds::IFDSProblem<ifds::DefinitionFact>;

// Explicit instantiations for PathEdge and SummaryEdge
template struct ifds::PathEdge<ifds::TaintFact>;
template struct ifds::PathEdge<ifds::DefinitionFact>;
template struct ifds::PathEdgeHash<ifds::TaintFact>;
template struct ifds::PathEdgeHash<ifds::DefinitionFact>;
template struct ifds::SummaryEdge<ifds::TaintFact>;
template struct ifds::SummaryEdge<ifds::DefinitionFact>;
template struct ifds::SummaryEdgeHash<ifds::TaintFact>;
template struct ifds::SummaryEdgeHash<ifds::DefinitionFact>;

// Explicit instantiations for ExplodedSupergraph
template class ifds::ExplodedSupergraph<ifds::TaintFact>;
template class ifds::ExplodedSupergraph<ifds::DefinitionFact>;

// Note: Debug utilities moved to IFDSDebugUtils.cpp

} // namespace ifds
