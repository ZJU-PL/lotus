/*
 * IFDS/IDE Framework
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
#include <Alias/AliasAnalysisWrapper.h>
#include <Support/ADT/ThreadSafe.h>

#include <functional>
#include <unordered_map>
#include <vector>
#include <thread>
#include <set>
#include <memory>

// Forward declaration
namespace lotus {
class AliasAnalysisWrapper;
}

namespace ifds {

// Import thread-safe data structures from Support/ADT
using lotus::SimpleOptional;
using lotus::ThreadSafeSet;
using lotus::ThreadSafeMap;
using lotus::ThreadSafeVector;
using lotus::ShardedMap;

// ============================================================================
// Parallel IFDS Solver Configuration
// ============================================================================

struct ParallelIFDSConfig {
    size_t num_threads = std::thread::hardware_concurrency();
    bool enable_parallel_processing = true;
    enum class ParallelMode {
        WORKLIST_PARALLELISM,    // Parallel worklist processing (default)
        FUNCTION_PARALLELISM,    // Function-level parallelism
        HYBRID_PARALLELISM       // Combination of both
    };
    ParallelMode parallel_mode = ParallelMode::WORKLIST_PARALLELISM;

    // Worklist batch size for load balancing (tuned for performance)
    // Empirically optimal range is 256-1024 (?)
    size_t worklist_batch_size = 512;

    // Synchronization frequency (how often to sync shared data structures)
    size_t sync_frequency = 1000;
};

// ============================================================================
// Forward Declarations
// ============================================================================

template<typename Fact> class IFDSProblem;
template<typename Fact, typename Value> class IDEProblem;
template<typename Fact> class ExplodedSupergraph;

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
    
    bool operator==(const PathEdge& other) const {
        return start_node == other.start_node && target_node == other.target_node &&
               start_fact == other.start_fact && target_fact == other.target_fact;
    }
    bool operator<(const PathEdge& other) const {
        if (start_node != other.start_node) return start_node < other.start_node;
        if (target_node != other.target_node) return target_node < other.target_node;
        if (start_fact != other.start_fact) return start_fact < other.start_fact;
        return target_fact < other.target_fact;
    }
};

template<typename Fact>
struct PathEdgeHash {
    size_t operator()(const PathEdge<Fact>& edge) const {
        size_t h1 = std::hash<const llvm::Instruction*>{}(edge.start_node);
        size_t h2 = std::hash<const llvm::Instruction*>{}(edge.target_node);
        size_t h3 = std::hash<Fact>{}(edge.start_fact);
        size_t h4 = std::hash<Fact>{}(edge.target_fact);
        return ((h1 ^ (h2 << 1)) ^ (h3 << 2)) ^ (h4 << 3);
    }
};

template<typename Fact>
struct SummaryEdge {
    const llvm::CallInst* call_site;
    Fact call_fact;
    Fact return_fact;
    
    SummaryEdge(const llvm::CallInst* call, const Fact& c_fact, const Fact& r_fact)
        : call_site(call), call_fact(c_fact), return_fact(r_fact) {}
    
    bool operator==(const SummaryEdge& other) const {
        return call_site == other.call_site && call_fact == other.call_fact && return_fact == other.return_fact;
    }
    bool operator<(const SummaryEdge& other) const {
        if (call_site != other.call_site) return call_site < other.call_site;
        if (call_fact != other.call_fact) return call_fact < other.call_fact;
        return return_fact < other.return_fact;
    }
};

template<typename Fact>
struct SummaryEdgeHash {
    size_t operator()(const SummaryEdge<Fact>& edge) const {
        size_t h1 = std::hash<const llvm::CallInst*>{}(edge.call_site);
        size_t h2 = std::hash<Fact>{}(edge.call_fact);
        size_t h3 = std::hash<Fact>{}(edge.return_fact);
        return (h1 ^ (h2 << 1)) ^ (h3 << 2);
    }
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
    virtual void set_alias_analysis(lotus::AliasAnalysisWrapper* aa);
    
    // Helper methods for common operations
    virtual bool is_source(const llvm::Instruction* inst) const;
    virtual bool is_sink(const llvm::Instruction* inst) const;
    
protected:
    lotus::AliasAnalysisWrapper* m_alias_analysis = nullptr;
    
    // Alias analysis helper using AliasAnalysisWrapper
    bool may_alias(const llvm::Value* v1, const llvm::Value* v2) const;
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
        
        bool operator==(const Node& other) const {
            return instruction == other.instruction && fact == other.fact;
        }
        bool operator<(const Node& other) const {
            if (instruction != other.instruction) return instruction < other.instruction;
            return fact < other.fact;
        }
    };
    
    struct NodeHash {
        size_t operator()(const Node& node) const {
            size_t h1 = std::hash<const llvm::Instruction*>{}(node.instruction);
            size_t h2 = std::hash<Fact>{}(node.fact);
            return h1 ^ (h2 << 1);
        }
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
// Parallel IFDS Solver and IFDS Solver (Declarations moved to IFDSSolvers.h)
// ============================================================================

// Note: ParallelIFDSSolver and IFDSSolver class definitions have been moved
// to include/Dataflow/IFDS/IFDSSolvers.h to avoid duplication and improve
// modularity. Include that header for solver implementations.

} // namespace ifds

// Provide std::hash specializations for IFDS types used in unordered containers
namespace std {
template<typename Fact>
struct hash<ifds::PathEdge<Fact>> {
    size_t operator()(const ifds::PathEdge<Fact>& edge) const noexcept {
        return ifds::PathEdgeHash<Fact>{}(edge);
    }
};

template<typename Fact>
struct hash<ifds::SummaryEdge<Fact>> {
    size_t operator()(const ifds::SummaryEdge<Fact>& edge) const noexcept {
        return ifds::SummaryEdgeHash<Fact>{}(edge);
    }
};
}

// ============================================================================
// Template Implementation (moved to .cpp for explicit instantiation)
// ============================================================================

// Provide inline template implementations for commonly used helpers so that
// templated clients (e.g., taint analysis) can link without relying on a
// separate translation unit.

namespace ifds {

template<typename Fact>
inline void IFDSProblem<Fact>::set_alias_analysis(lotus::AliasAnalysisWrapper* aa) {
    m_alias_analysis = aa;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::is_source(const llvm::Instruction*) const {
    return false;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::is_sink(const llvm::Instruction*) const {
    return false;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::may_alias(const llvm::Value* v1, const llvm::Value* v2) const {
    if (!m_alias_analysis || !v1 || !v2) return false;
    return m_alias_analysis->mayAlias(v1, v2);
}

} // namespace ifds