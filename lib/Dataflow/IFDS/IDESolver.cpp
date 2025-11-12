/*
 * IDE Solver Implementation
 * 
 * This implements the IDE (Interprocedural Distributive Environment) algorithm,
 * an extension of IFDS that propagates values along with dataflow facts.
 * 
 * Key features:
 * - Summary edge reuse: Callees are analyzed once per calling context
 * - Edge function composition memoization: Avoids redundant function compositions
 */

#include "Dataflow/IFDS/IDESolver.h"
#include "Dataflow/IFDS/IFDSFramework.h"

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

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
// IDESolver Implementation
// ============================================================================

template<typename Problem>
IDESolver<Problem>::IDESolver(Problem& problem) : m_problem(problem) {}

// ============================================================================
// Helper Methods
// ============================================================================

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr 
IDESolver<Problem>::make_edge_function(const EdgeFunction& ef) {
    return std::make_shared<EdgeFunction>(ef);
}

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr 
IDESolver<Problem>::compose_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2) {
    // Check cache first
    ComposePair key{f1, f2};
    auto it = m_compose_cache.find(key);
    if (it != m_compose_cache.end()) {
        return it->second;
    }
    
    // Compose and cache
    EdgeFunction composed = m_problem.compose(*f1, *f2);
    EdgeFunctionPtr result = make_edge_function(composed);
    m_compose_cache[key] = result;
    return result;
}

template<typename Problem>
void IDESolver<Problem>::solve(const llvm::Module& module) {
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;

    // Clear previous results and caches
    m_values.clear();
    m_summaries.clear();
    m_summary_index.clear();
    m_compose_cache.clear();
    m_path_edges_at_call.clear();

    // Build call graph
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> callee_to_calls;

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (const llvm::Function* callee = call->getCalledFunction()) {
                        call_to_callee[call] = callee;
                        callee_to_calls[callee].push_back(call);
                    }
                }
            }
        }
    }

    // Build CFG successors (including switch and invoke)
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> successors;
    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                std::vector<const llvm::Instruction*> succs;
                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                        succs.push_back(&br->getSuccessor(i)->front());
                    }
                } else if (auto* sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
                    for (unsigned i = 0; i < sw->getNumSuccessors(); ++i) {
                        succs.push_back(&sw->getSuccessor(i)->front());
                    }
                } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    succs.push_back(&invoke->getNormalDest()->front());
                    succs.push_back(&invoke->getUnwindDest()->front());
                } else if (llvm::isa<llvm::ReturnInst>(inst) || 
                          llvm::isa<llvm::UnreachableInst>(inst)) {
                    // No intraprocedural successors
                } else if (const llvm::Instruction* next = inst.getNextNode()) {
                    succs.push_back(next);
                }
                successors[&inst] = std::move(succs);
            }
        }
    }

    auto get_return_site = [](const llvm::CallInst* call) -> const llvm::Instruction* {
        if (!call->isTerminator()) {
            return call->getNextNode();
        }
        if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
            return &invoke->getNormalDest()->front();
        }
        return nullptr;
    };

    // Identity edge function (cached)
    EdgeFunctionPtr identity_func = make_edge_function(m_problem.identity());

    // Worklist of path edges with edge functions
    std::vector<PathEdge> worklist;
    std::unordered_set<PathEdge, PathEdgeHash> visited;

    auto queue_edge = [&](const llvm::Instruction* inst, const Fact& fact, 
                          EdgeFunctionPtr phi, const Value& incoming_value) {
        PathEdge pe{inst, fact, phi};
        
        // Check if we've seen this (inst, fact) pair
        if (visited.find(pe) != visited.end()) {
            // Already visited, but might need to update value
            Value& current = m_values[inst][fact];
            Value joined = m_problem.join(current, incoming_value);
            if (!(joined == current)) {
                current = joined;
                worklist.push_back(pe);
            }
        } else {
            // First time seeing this edge
            visited.insert(pe);
            Value& current = m_values[inst][fact];
            Value joined = m_problem.join(current, incoming_value);
            current = joined;
            worklist.push_back(pe);
        }
    };

    // Initialize: find entry point
    const llvm::Function* main_func = module.getFunction("main");
    if (!main_func) {
        for (const llvm::Function& f : module) {
            if (!f.isDeclaration() && !f.empty()) { 
                main_func = &f; 
                break; 
            }
        }
    }

    if (main_func && !main_func->empty()) {
        const llvm::Instruction* entry = &main_func->getEntryBlock().front();
        for (const auto& seed : m_problem.initial_facts(main_func)) {
            queue_edge(entry, seed, identity_func, m_problem.top_value());
        }
    }

    // Main tabulation loop
    while (!worklist.empty()) {
        PathEdge pe = worklist.back();
        worklist.pop_back();

        const llvm::Instruction* curr = pe.inst;
        const Fact& fact = pe.fact;
        EdgeFunctionPtr phi = pe.phi;
        const Value currVal = m_values[curr][fact];

        if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
            // Record path edge at call site for summary application
            m_path_edges_at_call[call].insert(pe);

            auto it_callee = call_to_callee.find(call);
            const llvm::Function* callee = (it_callee != call_to_callee.end()) ? it_callee->second : nullptr;

            // Always generate call-to-return edges
            const llvm::Instruction* ret_site = get_return_site(call);
            if (ret_site) {
                for (const auto& tgt_fact : m_problem.call_to_return_flow(call, fact)) {
                    auto ef = m_problem.call_to_return_edge_function(call, fact, tgt_fact);
                    EdgeFunctionPtr new_phi = compose_cached(make_edge_function(ef), phi);
                    queue_edge(ret_site, tgt_fact, new_phi, (*new_phi)(m_problem.top_value()));
                }
            }

            if (callee && !callee->isDeclaration()) {
                // Call into callee
                const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();
                for (const auto& callee_fact : m_problem.call_flow(call, callee, fact)) {
                    auto ef = m_problem.call_edge_function(call, fact, callee_fact);
                    EdgeFunctionPtr new_phi = compose_cached(make_edge_function(ef), phi);
                    queue_edge(callee_entry, callee_fact, new_phi, (*new_phi)(m_problem.top_value()));
                }

                // Apply existing summaries retroactively
                auto sum_it = m_summary_index.find(call);
                if (sum_it != m_summary_index.end()) {
                    for (const Summary& summary : sum_it->second) {
                        if (summary.call_fact == fact) {
                            // Apply summary: compose call edge + summary + return edge
                            EdgeFunctionPtr composed = compose_cached(summary.phi, phi);
                            Value result_val = (*composed)(m_problem.top_value());
                            queue_edge(ret_site, summary.return_fact, composed, result_val);
                        }
                    }
                }
            }

        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            // Return from function - create summaries
            const llvm::Function* func = ret->getFunction();
            auto it_calls = callee_to_calls.find(func);
            if (it_calls == callee_to_calls.end()) continue;

            for (const llvm::CallInst* call : it_calls->second) {
                const llvm::Instruction* ret_site = get_return_site(call);
                if (!ret_site) continue;

                // For each path edge ending at this call site
                auto path_it = m_path_edges_at_call.find(call);
                if (path_it == m_path_edges_at_call.end()) continue;

                for (const PathEdge& call_pe : path_it->second) {
                    const Fact& call_fact = call_pe.fact;

                    // Generate return facts
                    for (const auto& ret_fact : m_problem.return_flow(call, func, fact, call_fact)) {
                        auto ret_ef = m_problem.return_edge_function(call, fact, ret_fact);
                        EdgeFunctionPtr ret_phi = make_edge_function(ret_ef);

                        // Create summary: call_fact -> (ret_fact, phi composed with return)
                        EdgeFunctionPtr summary_phi = compose_cached(ret_phi, phi);
                        Summary new_summary{call, call_fact, ret_fact, summary_phi};

                        // Insert summary if new
                        auto insert_result = m_summaries.insert(new_summary);
                        if (insert_result.second) {
                            m_summary_index[call].push_back(new_summary);

                            // Apply summary to this call edge
                            EdgeFunctionPtr final_phi = compose_cached(summary_phi, call_pe.phi);
                            Value result_val = (*final_phi)(m_problem.top_value());
                            queue_edge(ret_site, ret_fact, final_phi, result_val);
                        }
                    }
                }
            }

        } else {
            // Normal intraprocedural flow
            auto succ_it = successors.find(curr);
            if (succ_it != successors.end()) {
                for (const llvm::Instruction* succ : succ_it->second) {
                    for (const auto& tgt_fact : m_problem.normal_flow(curr, fact)) {
                        auto ef = m_problem.normal_edge_function(curr, fact, tgt_fact);
                        EdgeFunctionPtr new_phi = compose_cached(make_edge_function(ef), phi);
                        queue_edge(succ, tgt_fact, new_phi, (*new_phi)(m_problem.top_value()));
                    }
                }
            }
        }
    }
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

