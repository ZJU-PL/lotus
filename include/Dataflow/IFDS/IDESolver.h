/*
 * IDE Solver
 *
 * This header provides the IDE (Interprocedural Distributive Environment)
 * solver implementation for the IFDS framework with:
 * - Summary edge reuse for efficient interprocedural analysis
 * - Edge function composition memoization
 */

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifds {

// ============================================================================
// IDE Solver
// ============================================================================

template<typename Problem>
class IDESolver {
public:
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;
    using EdgeFunction = typename Problem::EdgeFunction;
    using EdgeFunctionPtr = std::shared_ptr<EdgeFunction>;

    IDESolver(Problem& problem);

    void solve(const llvm::Module& module);

    // Query interface
    Value get_value_at(const llvm::Instruction* inst, const Fact& fact) const;
    const std::unordered_map<const llvm::Instruction*,
                            std::unordered_map<Fact, Value>>& get_all_values() const;

private:
    // Summary edge structure for interprocedural reuse
    struct Summary {
        const llvm::CallInst* call;
        Fact call_fact;
        Fact return_fact;
        EdgeFunctionPtr phi;  // Composed edge function through callee
        
        bool operator==(const Summary& other) const {
            return call == other.call && 
                   call_fact == other.call_fact && 
                   return_fact == other.return_fact;
        }
    };
    
    struct SummaryHash {
        size_t operator()(const Summary& s) const {
            size_t h1 = std::hash<const llvm::CallInst*>{}(s.call);
            size_t h2 = std::hash<Fact>{}(s.call_fact);
            size_t h3 = std::hash<Fact>{}(s.return_fact);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    // Path edge with associated edge function
    struct PathEdge {
        const llvm::Instruction* inst;
        Fact fact;
        EdgeFunctionPtr phi;  // Accumulated edge function to this point
        
        bool operator==(const PathEdge& other) const {
            return inst == other.inst && fact == other.fact;
        }
    };
    
    struct PathEdgeHash {
        size_t operator()(const PathEdge& pe) const {
            size_t h1 = std::hash<const llvm::Instruction*>{}(pe.inst);
            size_t h2 = std::hash<Fact>{}(pe.fact);
            return h1 ^ (h2 << 1);
        }
    };

    // Composition cache key
    struct ComposePair {
        EdgeFunctionPtr f1;
        EdgeFunctionPtr f2;
        
        bool operator==(const ComposePair& other) const {
            return f1 == other.f1 && f2 == other.f2;
        }
    };
    
    struct ComposePairHash {
        size_t operator()(const ComposePair& cp) const {
            return std::hash<EdgeFunctionPtr>{}(cp.f1) ^ 
                   (std::hash<EdgeFunctionPtr>{}(cp.f2) << 1);
        }
    };

    // Helper: memoized composition
    EdgeFunctionPtr compose_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2);
    
    // Helper: create shared pointer to edge function
    EdgeFunctionPtr make_edge_function(const EdgeFunction& ef);

    Problem& m_problem;
    
    // Results: instruction -> fact -> value
    std::unordered_map<const llvm::Instruction*, std::unordered_map<Fact, Value>> m_values;
    
    // Summary edges indexed by call site
    std::unordered_set<Summary, SummaryHash> m_summaries;
    std::unordered_map<const llvm::CallInst*, std::vector<Summary>> m_summary_index;
    
    // Composition memoization table
    std::unordered_map<ComposePair, EdgeFunctionPtr, ComposePairHash> m_compose_cache;
    
    // Path edges at call sites (for summary application)
    std::unordered_map<const llvm::CallInst*, std::unordered_set<PathEdge, PathEdgeHash>> m_path_edges_at_call;
};

} // namespace ifds
