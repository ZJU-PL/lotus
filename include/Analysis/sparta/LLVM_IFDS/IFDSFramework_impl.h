/*
 * IFDS/IDE Framework Template Implementation
 * 
 * This file contains the template implementations for the IFDS framework.
 * It's included by the main header to provide explicit template instantiation.
 */

#pragma once

#include <Analysis/sparta/LLVM_IFDS/IFDSFramework.h>

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

namespace sparta {
namespace ifds {

// ============================================================================
// PathEdge Implementation
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
    size_t h1 = std::hash<const llvm::Instruction*>{}(edge.start_node);
    size_t h2 = std::hash<Fact>{}(edge.start_fact);
    size_t h3 = std::hash<const llvm::Instruction*>{}(edge.target_node);
    size_t h4 = std::hash<Fact>{}(edge.target_fact);
    return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
}

// ============================================================================
// SummaryEdge Implementation
// ============================================================================

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
    size_t h1 = std::hash<const llvm::CallInst*>{}(edge.call_site);
    size_t h2 = std::hash<Fact>{}(edge.call_fact);
    size_t h3 = std::hash<Fact>{}(edge.return_fact);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
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
    std::vector<const llvm::Value*> pts_set;
    
    if (!m_alias_analysis) {
        // Conservative approximation - assume it points to itself
        pts_set.push_back(ptr);
        return pts_set;
    }
    
    // Note: Dyck AA doesn't directly provide points-to sets, only alias information
    // We need to approximate points-to by finding what this pointer may alias with
    // that represents memory objects (allocas, globals, etc.)
    
    const std::set<llvm::Value*>* alias_set = m_alias_analysis->getAliasSet(const_cast<llvm::Value*>(ptr));
    if (alias_set) {
        for (llvm::Value* v : *alias_set) {
            // Only include values that represent memory objects
            if (llvm::isa<llvm::AllocaInst>(v) || 
                llvm::isa<llvm::GlobalVariable>(v) ||
                llvm::isa<llvm::Argument>(v)) {
                pts_set.push_back(v);
            }
        }
    }
    
    // If no memory objects found, conservatively include the pointer itself
    if (pts_set.empty()) {
        pts_set.push_back(ptr);
    }
    
    return pts_set;
}

template<typename Fact>
std::vector<const llvm::Value*> IFDSProblem<Fact>::get_alias_set(const llvm::Value* val) const {
    std::vector<const llvm::Value*> alias_set;
    
    if (!m_alias_analysis) {
        alias_set.push_back(val);
        return alias_set;
    }
    
    const std::set<llvm::Value*>* dyck_alias_set = 
        m_alias_analysis->getAliasSet(const_cast<llvm::Value*>(val));
    if (dyck_alias_set) {
        for (llvm::Value* v : *dyck_alias_set) {
            alias_set.push_back(v);
        }
    } else {
        alias_set.push_back(val);
    }
    
    return alias_set;
}

// ============================================================================
// IDEProblem Implementation
// ============================================================================

template<typename Fact, typename Value>
typename IDEProblem<Fact, Value>::EdgeFunction 
IDEProblem<Fact, Value>::compose(const EdgeFunction& f1, const EdgeFunction& f2) const {
    return [f1, f2](const Value& v) { return f1(f2(v)); };
}

template<typename Fact, typename Value>
typename IDEProblem<Fact, Value>::EdgeFunction 
IDEProblem<Fact, Value>::identity() const {
    return [](const Value& v) { return v; };
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
    if (graph.m_entry) {
        return *graph.m_entry; 
    }
    // Return a default node if no entry is set
    return NodeId(nullptr, Fact{});
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
    if (it != graph.m_predecessors.end()) {
        return it->second;
    }
    return {};
}

template<typename Fact>
std::vector<typename ExplodedSupergraph<Fact>::EdgeId> 
ExplodedSupergraph<Fact>::successors(const Graph& graph, const NodeId& node) {
    auto it = graph.m_successors.find(node);
    if (it != graph.m_successors.end()) {
        return it->second;
    }
    return {};
}

template<typename Fact>
void ExplodedSupergraph<Fact>::add_edge(const Edge& edge) {
    m_edges.push_back(edge);
    m_successors[edge.source].push_back(edge);
    m_predecessors[edge.target].push_back(edge);
}

template<typename Fact>
void ExplodedSupergraph<Fact>::set_entry(const NodeId& entry) {
    m_entry = entry;
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
    if (it != m_entry_facts.end()) {
        return it->second;
    }
    return FactSet::bottom();
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet 
IFDSSolver<Problem>::get_facts_at_exit(const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    if (it != m_exit_facts.end()) {
        return it->second;
    }
    return FactSet::bottom();
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
    if (it != m_exit_facts.end()) {
        return it->second.contains(fact);
    }
    return false;
}

template<typename Problem>
std::unordered_map<typename IFDSSolver<Problem>::Node, typename IFDSSolver<Problem>::FactSet, typename IFDSSolver<Problem>::NodeHash> 
IFDSSolver<Problem>::get_all_results() const {
    std::unordered_map<Node, FactSet, NodeHash> results;
    typename Problem::FactType zero = m_problem.zero_fact();
    
    // Convert our new format to the old Node-based format
    for (const auto& entry : m_exit_facts) {
        const llvm::Instruction* inst = entry.first;
        const FactSet& facts = entry.second;
        
        if (!facts.empty()) {
            Node node(inst, zero);  // Use zero fact as placeholder
            results[node] = facts;
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
    
    // Check if we have existing summary edges for this call
    for (const auto& summary : m_summary_edges) {
        if (summary.call_site == call && summary.call_fact == fact) {
            // Apply existing summary
            const llvm::Instruction* return_site = get_return_site(call);
            if (return_site) {
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
            
            // Apply summary to all existing path edges ending at this call
            for (const auto& path_edge : m_path_edges) {
                if (path_edge.target_node == call) {
                    const llvm::Instruction* return_site = get_return_site(call);
                    if (return_site) {
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
    m_summary_edges.clear();
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
        FactSet initial_facts = m_problem.initial_facts(main_func);
        
        for (const auto& fact : initial_facts) {
            PathEdgeType initial_edge(entry, fact, entry, fact);
            propagate_path_edge(initial_edge);
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::run_tabulation() {
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
    }
}

template<typename Problem>
const llvm::Function* IFDSSolver<Problem>::get_main_function(const llvm::Module& module) {
    return module.getFunction("main");
}

// ============================================================================
// IDESolver Implementation
// ============================================================================

template<typename Problem>
IDESolver<Problem>::IDESolver(Problem& problem) : m_problem(problem) {}

template<typename Problem>
void IDESolver<Problem>::solve(const llvm::Module& /*module*/) {
    // TODO: Implement full IDE tabulation algorithm
    // This is a simplified version - full implementation would include
    // edge functions, value propagation, and meet-over-valid-paths computation
}

template<typename Problem>
typename IDESolver<Problem>::Value 
IDESolver<Problem>::get_value_at(const llvm::Instruction* inst, const typename Problem::FactType& fact) const {
    auto inst_it = m_values.find(inst);
    if (inst_it != m_values.end()) {
        auto fact_it = inst_it->second.find(fact);
        if (fact_it != inst_it->second.end()) {
            return fact_it->second;
        }
    }
    return m_problem.bottom_value();
}

template<typename Problem>
const std::unordered_map<const llvm::Instruction*, 
                        std::unordered_map<typename Problem::FactType, typename Problem::ValueType>>& 
IDESolver<Problem>::get_all_values() const {
    return m_values;
}

} // namespace ifds
} // namespace sparta
