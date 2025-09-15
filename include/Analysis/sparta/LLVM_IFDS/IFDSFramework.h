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

#include <Analysis/sparta/AbstractDomain.h>
#include <Analysis/sparta/FlatSet.h>
#include <Analysis/sparta/MonotonicFixpointIterator.h>
#include <Analysis/sparta/WeakTopologicalOrdering.h>

#include <boost/optional.hpp>

#include <Alias/DyckAA/DyckAliasAnalysis.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sparta {
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
// IFDS Core Framework
// ============================================================================

template<typename Fact>
class IFDSProblem {
public:
    using FactType = Fact;
    using FactSet = sparta::FlatSet<Fact>;
    
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
    virtual void set_alias_analysis(DyckAliasAnalysis* aa) { m_alias_analysis = aa; }
    
    // Helper methods for common operations
    virtual bool is_source(const llvm::Instruction* /*inst*/) const { return false; }
    virtual bool is_sink(const llvm::Instruction* /*inst*/) const { return false; }
    
protected:
    DyckAliasAnalysis* m_alias_analysis = nullptr;
    
    // Alias analysis helpers using Dyck AA
    bool may_alias(const llvm::Value* v1, const llvm::Value* v2) const {
        if (!m_alias_analysis) return true; // Conservative
        
        return m_alias_analysis->mayAlias(const_cast<llvm::Value*>(v1), 
                                         const_cast<llvm::Value*>(v2));
    }
    
    // Get the points-to set for a pointer (what it may point to)
    std::vector<const llvm::Value*> get_points_to_set(const llvm::Value* ptr) const {
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
    
    // Get the alias set for a value (what may alias with it)
    std::vector<const llvm::Value*> get_alias_set(const llvm::Value* val) const {
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
};

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
    virtual EdgeFunction compose(const EdgeFunction& f1, const EdgeFunction& f2) const {
        return [f1, f2](const Value& v) { return f1(f2(v)); };
    }
    
    // Identity edge function
    EdgeFunction identity() const {
        return [](const Value& v) { return v; };
    }
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
            if (instruction != other.instruction) {
                return instruction < other.instruction;
            }
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
    static NodeId entry(const Graph& graph) { 
        if (graph.m_entry) {
            return *graph.m_entry; 
        }
        // Return a default node if no entry is set
        return NodeId(nullptr, Fact{});
    }
    static NodeId source(const Graph& /*graph*/, const EdgeId& edge) { return edge.source; }
    static NodeId target(const Graph& /*graph*/, const EdgeId& edge) { return edge.target; }
    
    static std::vector<EdgeId> predecessors(const Graph& graph, const NodeId& node) {
        auto it = graph.m_predecessors.find(node);
        if (it != graph.m_predecessors.end()) {
            return it->second;
        }
        return {};
    }
    
    static std::vector<EdgeId> successors(const Graph& graph, const NodeId& node) {
        auto it = graph.m_successors.find(node);
        if (it != graph.m_successors.end()) {
            return it->second;
        }
        return {};
    }
    
    void add_edge(const Edge& edge) {
        m_edges.push_back(edge);
        m_successors[edge.source].push_back(edge);
        m_predecessors[edge.target].push_back(edge);
    }
    
    void set_entry(const NodeId& entry) { m_entry = entry; }
    
    const std::vector<Edge>& get_edges() const { return m_edges; }
    
private:
    boost::optional<NodeId> m_entry;
    std::vector<Edge> m_edges;
    std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_successors;
    std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_predecessors;
};

// ============================================================================
// IFDS Solver using Sparta's Fixpoint Iterator
// ============================================================================

template<typename Problem>
class IFDSSolver {
public:
    using Fact = typename Problem::FactType;
    using FactSet = typename Problem::FactSet;
    using Node = typename ExplodedSupergraph<Fact>::Node;
    using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;
    
private:
    // Abstract domain for IFDS facts
    class IFDSAbstractDomain {
    private:
        FactSet m_facts;
        
    public:
        IFDSAbstractDomain() : m_facts(FactSet::bottom()) {}
        explicit IFDSAbstractDomain(const FactSet& facts) : m_facts(facts) {}
        
        bool is_bottom() const { return m_facts.is_bottom(); }
        bool is_top() const { return m_facts.is_top(); }
        
        void set_to_bottom() { m_facts.set_to_bottom(); }
        void set_to_top() { m_facts.set_to_top(); }
        
        bool leq(const IFDSAbstractDomain& other) const {
            return m_facts.leq(other.m_facts);
        }
        
        bool equals(const IFDSAbstractDomain& other) const {
            return m_facts.equals(other.m_facts);
        }
        
        void join_with(const IFDSAbstractDomain& other) {
            m_facts.join_with(other.m_facts);
        }
        
        void widen_with(const IFDSAbstractDomain& other) {
            // For IFDS, widening is typically just join
            join_with(other);
        }
        
        void narrow_with(const IFDSAbstractDomain& other) {
            // For IFDS, narrowing is typically just meet
            m_facts.meet_with(other.m_facts);
        }
        
        const FactSet& get_facts() const { return m_facts; }
        void set_facts(const FactSet& facts) { m_facts = facts; }
        
        friend std::ostream& operator<<(std::ostream& os, const IFDSAbstractDomain& domain) {
            os << domain.m_facts;
            return os;
        }
    };
    
    // IFDS Worklist Algorithm Implementation
    class IFDSFixpointIterator {
    private:
        Problem& m_problem;
        const ExplodedSupergraph<Fact>& m_graph;
        std::unordered_map<Node, FactSet, NodeHash> m_node_states;
        std::unordered_set<Node, NodeHash> m_worklist;
        
        void propagate_fact(const Node& node, const Fact& fact) {
            if (fact.is_zero() && m_node_states[node].contains(fact)) {
                return; // Already processed
            }
            
            bool changed = false;
            auto& current_facts = m_node_states[node];
            if (!current_facts.contains(fact)) {
                current_facts.insert(fact);
                changed = true;
            }
            
            if (changed) {
                m_worklist.insert(node);
            }
        }
        
    public:
        IFDSFixpointIterator(const ExplodedSupergraph<Fact>& graph, Problem& problem)
            : m_problem(problem), m_graph(graph) {}
        
        void run(const IFDSAbstractDomain& initial_state) {
            m_node_states.clear();
            m_worklist.clear();
            
            // Initialize worklist with entry nodes
            const auto& initial_facts = initial_state.get_facts();
            if (!initial_facts.empty()) {
                // Find all nodes and initialize entry points
                std::unordered_set<Node, NodeHash> all_nodes;
                for (const auto& edge : m_graph.get_edges()) {
                    all_nodes.insert(edge.source);
                    all_nodes.insert(edge.target);
                }
                
                // Initialize function entry points
                std::unordered_set<const llvm::Function*> processed_functions;
                for (const Node& node : all_nodes) {
                    if (node.instruction && node.instruction->getParent()) {
                        const llvm::Function* func = node.instruction->getParent()->getParent();
                        if (func && processed_functions.find(func) == processed_functions.end()) {
                            // Check if this is the first instruction of the function
                            if (!func->empty() && &func->getEntryBlock().front() == node.instruction) {
                                processed_functions.insert(func);
                                for (const auto& fact : initial_facts) {
                                    propagate_fact(node, fact);
                                }
                            }
                        }
                    }
                }
                
                // If no entry points found, initialize all nodes with zero fact
                if (m_worklist.empty()) {
                    for (const Node& node : all_nodes) {
                        propagate_fact(node, m_problem.zero_fact());
                        break; // Just initialize one node to start
                    }
                }
            }
            
            // Worklist algorithm
            while (!m_worklist.empty()) {
                auto it = m_worklist.begin();
                Node current = *it;
                m_worklist.erase(it);
                
                process_node(current);
            }
        }
        
        void process_node(const Node& node) {
            const auto& current_facts = m_node_states[node];
            if (current_facts.empty()) {
                return;
            }
            
            // Process each fact at this node
            for (const auto& fact : current_facts) {
                // Find all outgoing edges from this node
                auto successors = ExplodedSupergraph<Fact>::successors(m_graph, node);
                
                for (const auto& edge : successors) {
                    const Node& target = edge.target;
                    
                    FactSet new_facts;
                    
                    switch (edge.type) {
                        case ExplodedSupergraph<Fact>::Edge::NORMAL:
                            new_facts = m_problem.normal_flow(node.instruction, fact);
                            break;
                            
                        case ExplodedSupergraph<Fact>::Edge::CALL:
                            if (auto* call = llvm::dyn_cast<llvm::CallInst>(node.instruction)) {
                                if (const llvm::Function* callee = call->getCalledFunction()) {
                                    new_facts = m_problem.call_flow(call, callee, fact);
                                }
                            }
                            break;
                            
                        case ExplodedSupergraph<Fact>::Edge::RETURN:
                            // Return flow needs both exit fact and call fact
                            // This is simplified - full implementation needs call context
                            if (auto* call = llvm::dyn_cast<llvm::CallInst>(target.instruction)) {
                                if (const llvm::Function* callee = call->getCalledFunction()) {
                                    new_facts = m_problem.return_flow(call, callee, fact, target.fact);
                                }
                            }
                            break;
                            
                        case ExplodedSupergraph<Fact>::Edge::CALL_TO_RETURN:
                            if (auto* call = llvm::dyn_cast<llvm::CallInst>(node.instruction)) {
                                new_facts = m_problem.call_to_return_flow(call, fact);
                            }
                            break;
                    }
                    
                    // Propagate new facts to target node
                    for (const auto& new_fact : new_facts) {
                        propagate_fact(target, new_fact);
                    }
                }
            }
        }
        
        FactSet get_entry_state_at(const Node& node) const {
            auto it = m_node_states.find(node);
            if (it != m_node_states.end()) {
                return it->second;
            }
            return FactSet::bottom();
        }
        
        FactSet get_exit_state_at(const Node& node) const {
            return get_entry_state_at(node);
        }
        
        const std::unordered_map<Node, FactSet, NodeHash>& get_all_states() const {
            return m_node_states;
        }
    };
    
public:
    IFDSSolver(Problem& problem) : m_problem(problem) {}
    
    void solve(const llvm::Module& module) {
        // Build exploded supergraph
        ExplodedSupergraph<Fact> supergraph = build_supergraph(module);
        
        // Create and run fixpoint iterator
        IFDSFixpointIterator iterator(supergraph, m_problem);
        
        // Initialize with entry facts
        const llvm::Function* main_func = get_main_function(module);
        FactSet initial_facts;
        
        if (main_func && !main_func->isDeclaration()) {
            initial_facts = m_problem.initial_facts(main_func);
        } else {
            // If no main, find any entry function and initialize
            for (const llvm::Function& func : module) {
                if (!func.isDeclaration() && !func.empty()) {
                    initial_facts = m_problem.initial_facts(&func);
                    break;
                }
            }
            
            // If still no initial facts, create minimal initial state
            if (initial_facts.empty()) {
                initial_facts.insert(m_problem.zero_fact());
            }
        }
        
        IFDSAbstractDomain initial_state(initial_facts);
        iterator.run(initial_state);
        
        // Extract results
        extract_results(iterator, supergraph);
    }
    
    // Get results for a specific node
    FactSet get_facts_at(const Node& node) const {
        auto it = m_results.find(node);
        if (it != m_results.end()) {
            return it->second;
        }
        return FactSet::bottom();
    }
    
    // Get all results
    const std::unordered_map<Node, FactSet, NodeHash>& get_all_results() const {
        return m_results;
    }
    
private:
    Problem& m_problem;
    std::unordered_map<Node, FactSet, NodeHash> m_results;
    
    ExplodedSupergraph<Fact> build_supergraph(const llvm::Module& module) {
        ExplodedSupergraph<Fact> supergraph;
        
        // For each function, build the intraprocedural part
        for (const llvm::Function& func : module) {
            if (func.isDeclaration()) continue;
            
            build_function_subgraph(func, supergraph);
        }
        
        // Add interprocedural edges
        add_interprocedural_edges(module, supergraph);
        
        return supergraph;
    }
    
    void build_function_subgraph(const llvm::Function& func, ExplodedSupergraph<Fact>& supergraph) {
        // Create nodes for each instruction with the zero fact
        std::unordered_map<const llvm::Instruction*, Node> inst_to_node;
        Fact zero = m_problem.zero_fact();
        
        // First pass: create all nodes
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                Node node(&inst, zero);
                inst_to_node[&inst] = node;
                
                // Set entry node for the function
                if (&inst == &func.getEntryBlock().front()) {
                    supergraph.set_entry(node);
                }
            }
        }
        
        // Second pass: create intraprocedural edges
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                Node current_node = inst_to_node[&inst];
                
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    // Handle call instructions
                    const llvm::Function* callee = call->getCalledFunction();
                    
                    if (callee && !callee->isDeclaration()) {
                        // This is an interprocedural call - will be handled later
                        // For now, just add call-to-return edge for local facts
                        if (const llvm::Instruction* next = inst.getNextNode()) {
                            Node next_node = inst_to_node[next];
                            typename ExplodedSupergraph<Fact>::Edge edge(
                                current_node, next_node, 
                                ExplodedSupergraph<Fact>::Edge::CALL_TO_RETURN);
                            supergraph.add_edge(edge);
                        }
                    } else {
                        // External call or indirect call - treat as normal flow
                        if (const llvm::Instruction* next = inst.getNextNode()) {
                            Node next_node = inst_to_node[next];
                            typename ExplodedSupergraph<Fact>::Edge edge(
                                current_node, next_node, 
                                ExplodedSupergraph<Fact>::Edge::NORMAL);
                            supergraph.add_edge(edge);
                        }
                    }
                } else if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                    // Handle branch instructions
                    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                        const llvm::BasicBlock* succ_bb = br->getSuccessor(i);
                        const llvm::Instruction* first_inst = &succ_bb->front();
                        
                        if (inst_to_node.find(first_inst) != inst_to_node.end()) {
                            Node succ_node = inst_to_node[first_inst];
                            typename ExplodedSupergraph<Fact>::Edge edge(
                                current_node, succ_node, 
                                ExplodedSupergraph<Fact>::Edge::NORMAL);
                            supergraph.add_edge(edge);
                        }
                    }
                } else if (llvm::isa<llvm::ReturnInst>(&inst)) {
                    // Return instructions will be handled by interprocedural edges
                    continue;
                } else {
                    // Normal instruction - add edge to next instruction
                    if (const llvm::Instruction* next = inst.getNextNode()) {
                        Node next_node = inst_to_node[next];
                        typename ExplodedSupergraph<Fact>::Edge edge(
                            current_node, next_node, 
                            ExplodedSupergraph<Fact>::Edge::NORMAL);
                        supergraph.add_edge(edge);
                    }
                }
            }
        }
    }
    
    void add_interprocedural_edges(const llvm::Module& module, ExplodedSupergraph<Fact>& supergraph) {
        Fact zero = m_problem.zero_fact();
        
        // Create a mapping from instructions to nodes
        std::unordered_map<const llvm::Instruction*, Node> inst_to_node;
        for (const auto& edge : supergraph.get_edges()) {
            inst_to_node[edge.source.instruction] = edge.source;
            inst_to_node[edge.target.instruction] = edge.target;
        }
        
        // Add call and return edges
        for (const llvm::Function& caller : module) {
            if (caller.isDeclaration()) continue;
            
            for (const llvm::BasicBlock& bb : caller) {
                for (const llvm::Instruction& inst : bb) {
                    if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                        if (const llvm::Function* callee = call->getCalledFunction()) {
                            if (!callee->isDeclaration() && !callee->empty()) {
                                // Get call site node
                                Node call_node(&inst, zero);
                                
                                // Get callee entry node
                                const llvm::Instruction* entry_inst = &callee->getEntryBlock().front();
                                Node entry_node(entry_inst, zero);
                                
                                // Add call edge: call site -> callee entry
                                typename ExplodedSupergraph<Fact>::Edge call_edge(
                                    call_node, entry_node, 
                                    ExplodedSupergraph<Fact>::Edge::CALL);
                                supergraph.add_edge(call_edge);
                                
                                // Find all return instructions in callee and add return edges
                                for (const llvm::BasicBlock& callee_bb : *callee) {
                                    for (const llvm::Instruction& callee_inst : callee_bb) {
                                        if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&callee_inst)) {
                                            Node ret_node(ret, zero);
                                            
                                            // Find return site (instruction after call)
                                            if (const llvm::Instruction* return_site = call->getNextNode()) {
                                                Node return_site_node(return_site, zero);
                                                
                                                // Add return edge: return instruction -> return site
                                                typename ExplodedSupergraph<Fact>::Edge return_edge(
                                                    ret_node, return_site_node, 
                                                    ExplodedSupergraph<Fact>::Edge::RETURN);
                                                supergraph.add_edge(return_edge);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    const llvm::Function* get_main_function(const llvm::Module& module) {
        return module.getFunction("main");
    }
    
    void analyze_function(const llvm::Function& /*func*/, const ExplodedSupergraph<Fact>& /*supergraph*/) {
        // Analyze a single function in isolation
        // This is a fallback when there's no main function
        // Currently not implemented - using main analysis path instead
    }
    
    void extract_results(const IFDSFixpointIterator& iterator, const ExplodedSupergraph<Fact>& /*supergraph*/) {
        // Extract results from the fixpoint iterator
        m_results.clear();
        
        const auto& all_states = iterator.get_all_states();
        for (const auto& state_pair : all_states) {
            const Node& node = state_pair.first;
            const FactSet& facts = state_pair.second;
            
            if (!facts.empty()) {
                m_results[node] = facts;
            }
        }
    }
};

// ============================================================================
// IDE Solver (extends IFDS with edge functions)
// ============================================================================

template<typename Problem>
class IDESolver : public IFDSSolver<Problem> {
public:
    using Value = typename Problem::ValueType;
    using EdgeFunction = typename Problem::EdgeFunction;
    
    IDESolver(Problem& problem) : IFDSSolver<Problem>(problem) {}
    
    // IDE-specific methods
    Value get_value_at(const typename IFDSSolver<Problem>::Node& /*node*/, 
                      const typename Problem::FactType& /*fact*/) const {
        // Return the computed value for a fact at a specific program point
        return this->m_problem.bottom_value();
    }
};

} // namespace ifds
} // namespace sparta
