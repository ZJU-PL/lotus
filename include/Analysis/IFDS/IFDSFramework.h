/*
 * IFDS/IDE Framework for Sparta
 * 
 * This header provides a comprehensive IFDS/IDE framework built on top of 
 * the Sparta abstract interpretation library, with integration for LLVM alias analysis.
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

#include <set>


#include <Alias/DyckAA/DyckAliasAnalysis.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifds {

// ============================================================================
// Forward Declarations
// ============================================================================

template<typename Fact> class IFDSProblem;
template<typename Fact, typename Value> class IDEProblem;
template<typename Fact> class ExplodedSupergraph;
template<typename Problem> class IFDSSolver;
template<typename Problem> class IDESolver;

// ============================================================================
// IFDS Core Data Structures
// ============================================================================

template<typename Fact>
struct PathEdge {
    const llvm::Instruction* start_node;
    Fact start_fact;
    const llvm::Instruction* target_node;
    Fact target_fact;
    
    PathEdge(const llvm::Instruction* s_node, const Fact& s_fact,
             const llvm::Instruction* t_node, const Fact& t_fact)
        : start_node(s_node), start_fact(s_fact), target_node(t_node), target_fact(t_fact) {}
    
    bool operator==(const PathEdge& other) const;
    bool operator<(const PathEdge& other) const;
};

template<typename Fact>
struct PathEdgeHash {
    size_t operator()(const PathEdge<Fact>& edge) const;
};

template<typename Fact>
struct SummaryEdge {
    const llvm::CallInst* call_site;
    Fact call_fact;
    Fact return_fact;
    
    SummaryEdge(const llvm::CallInst* call, const Fact& c_fact, const Fact& r_fact)
        : call_site(call), call_fact(c_fact), return_fact(r_fact) {}
    
    bool operator==(const SummaryEdge& other) const;
    bool operator<(const SummaryEdge& other) const;
};

template<typename Fact>
struct SummaryEdgeHash {
    size_t operator()(const SummaryEdge<Fact>& edge) const;
};

// ============================================================================
// IFDS Problem Interface
// ============================================================================

template<typename Fact>
class IFDSProblem {
public:
    using FactType = Fact;
    using FactSet = std::set<Fact>;
    
    virtual ~IFDSProblem() = default;
    
    // Zero fact (lambda in IFDS terminology)
    virtual Fact zero_fact() const = 0;
    
    // Flow functions for different statement types
    virtual FactSet normal_flow(const llvm::Instruction* stmt, const Fact& fact) = 0;
    virtual FactSet call_flow(const llvm::CallInst* call, const llvm::Function* callee, const Fact& fact) = 0;
    virtual FactSet return_flow(const llvm::CallInst* call, const llvm::Function* callee, 
                               const Fact& exit_fact, const Fact& call_fact) = 0;
    virtual FactSet call_to_return_flow(const llvm::CallInst* call, const Fact& fact) = 0;
    
    // Initial facts at program entry
    virtual FactSet initial_facts(const llvm::Function* main) = 0;
    
    // Alias analysis integration
    virtual void set_alias_analysis(DyckAliasAnalysis* aa);
    
    // Helper methods for common operations
    virtual bool is_source(const llvm::Instruction* inst) const;
    virtual bool is_sink(const llvm::Instruction* inst) const;
    
protected:
    DyckAliasAnalysis* m_alias_analysis = nullptr;
    
    // Alias analysis helpers using Dyck AA
    bool may_alias(const llvm::Value* v1, const llvm::Value* v2) const;
    std::vector<const llvm::Value*> get_points_to_set(const llvm::Value* ptr) const;
    std::vector<const llvm::Value*> get_alias_set(const llvm::Value* val) const;
};

// ============================================================================
// IDE Problem Interface
// ============================================================================

template<typename Fact, typename Value>
class IDEProblem : public IFDSProblem<Fact> {
public:
    using ValueType = Value;
    using EdgeFunction = std::function<Value(const Value&)>;
    
    // Edge functions for IDE
    virtual EdgeFunction normal_edge_function(const llvm::Instruction* stmt, 
                                            const Fact& src_fact, const Fact& tgt_fact) = 0;
    virtual EdgeFunction call_edge_function(const llvm::CallInst* call, 
                                           const Fact& src_fact, const Fact& tgt_fact) = 0;
    virtual EdgeFunction return_edge_function(const llvm::CallInst* call, 
                                             const Fact& exit_fact, const Fact& ret_fact) = 0;
    virtual EdgeFunction call_to_return_edge_function(const llvm::CallInst* call, 
                                                     const Fact& src_fact, const Fact& tgt_fact) = 0;
    
    // Value domain operations
    virtual Value top_value() const = 0;
    virtual Value bottom_value() const = 0;
    virtual Value join(const Value& v1, const Value& v2) const = 0;
    
    // Edge function composition
    virtual EdgeFunction compose(const EdgeFunction& f1, const EdgeFunction& f2) const;
    
    // Identity edge function
    EdgeFunction identity() const;
};

// ============================================================================
// Exploded Supergraph Representation
// ============================================================================

template<typename Fact>
class ExplodedSupergraph {
public:
    struct Node {
        const llvm::Instruction* instruction;
        Fact fact;
        
        Node() : instruction(nullptr), fact() {}
        Node(const llvm::Instruction* inst, const Fact& f) : instruction(inst), fact(f) {}
        
        bool operator==(const Node& other) const;
        bool operator<(const Node& other) const;
    };
    
    struct NodeHash {
        size_t operator()(const Node& node) const;
    };
    
    struct Edge {
        Node source;
        Node target;
        enum Type { NORMAL, CALL, RETURN, CALL_TO_RETURN } type;
        
        Edge(const Node& src, const Node& tgt, Type t) : source(src), target(tgt), type(t) {}
    };
    
    using NodeId = Node;
    using EdgeId = Edge;
    using Graph = ExplodedSupergraph<Fact>;
    
    // GraphInterface implementation for Sparta fixpoint iterator
    static NodeId entry(const Graph& graph);
    static NodeId source(const Graph& graph, const EdgeId& edge);
    static NodeId target(const Graph& graph, const EdgeId& edge);
    static std::vector<EdgeId> predecessors(const Graph& graph, const NodeId& node);
    static std::vector<EdgeId> successors(const Graph& graph, const NodeId& node);
    
    void add_edge(const Edge& edge);
    void set_entry(const NodeId& entry);
    const std::vector<Edge>& get_edges() const;
    
private:
    std::unique_ptr<NodeId> m_entry;
    std::vector<Edge> m_edges;
    std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_successors;
    std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_predecessors;
};

// ============================================================================
// IFDS Solver
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
    const std::unordered_set<PathEdge<Fact>, PathEdgeHash<Fact>>& get_path_edges() const;
    
    // Get all summary edges (for debugging/analysis)
    const std::unordered_set<SummaryEdge<Fact>, SummaryEdgeHash<Fact>>& get_summary_edges() const;
    
    // Check if a fact reaches a specific instruction
    bool fact_reaches(const Fact& fact, const llvm::Instruction* inst) const;
    
    // Legacy compatibility methods for existing tools
    std::unordered_map<Node, FactSet, NodeHash> get_all_results() const;
    FactSet get_facts_at(const Node& node) const;

private:
    using PathEdgeType = PathEdge<Fact>;
    using SummaryEdgeType = SummaryEdge<Fact>;
    
    Problem& m_problem;
    
    // Progress tracking
    bool m_show_progress = false;
    
    // Core tabulation tables
    std::unordered_set<PathEdgeType, PathEdgeHash<Fact>> m_path_edges;
    std::unordered_set<SummaryEdgeType, SummaryEdgeHash<Fact>> m_summary_edges;
    std::vector<PathEdgeType> m_worklist;
    
    // Tabulation tables for efficiency
    std::unordered_map<const llvm::Instruction*, FactSet> m_entry_facts;
    std::unordered_map<const llvm::Instruction*, FactSet> m_exit_facts;
    
    // Indexed summary edges for O(1) lookup (call_site -> list of summary edges)
    std::unordered_map<const llvm::CallInst*, std::vector<SummaryEdgeType>> m_summary_index;
    
    // Indexed path edges by target node for O(1) lookup
    std::unordered_map<const llvm::Instruction*, std::vector<PathEdgeType>> m_path_edges_at;
    
    // Call graph information
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> m_call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> m_callee_to_calls;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::ReturnInst*>> m_function_returns;
    
    // CFG navigation helpers
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_successors;
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_predecessors;
    
    // Core IFDS Tabulation Algorithm Methods
    void propagate_path_edge(const PathEdgeType& edge);
    void process_normal_edge(const llvm::Instruction* curr, const llvm::Instruction* next, const Fact& fact);
    void process_call_edge(const llvm::CallInst* call, const llvm::Function* callee, const Fact& fact);
    void process_return_edge(const llvm::ReturnInst* ret, const Fact& fact);
    void process_call_to_return_edge(const llvm::CallInst* call, const Fact& fact);
    
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
// IDE Solver
// ============================================================================

template<typename Problem>
class IDESolver {
public:
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;
    using EdgeFunction = typename Problem::EdgeFunction;
    
    IDESolver(Problem& problem);
    
    void solve(const llvm::Module& module);
    
    // Query interface
    Value get_value_at(const llvm::Instruction* inst, const Fact& fact) const;
    const std::unordered_map<const llvm::Instruction*, 
                            std::unordered_map<Fact, Value>>& get_all_values() const;

private:
    Problem& m_problem;
    std::unordered_map<const llvm::Instruction*, 
                      std::unordered_map<Fact, Value>> m_values;
};

} // namespace ifds

// ============================================================================
// Template Implementation (moved to .cpp for explicit instantiation)
// ============================================================================

// Template implementations are now in IFDSFramework.cpp
// This reduces compilation time and improves code organization