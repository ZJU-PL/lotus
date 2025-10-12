/*
 * IDE Solver Implementation
 * 
 * This implements the IDE (Interprocedural Distributive Environment) algorithm,
 * an extension of IFDS that propagates values along with dataflow facts.
 */

#include <Analysis/IFDS/IDESolver.h>
#include <Analysis/IFDS/IFDSFramework.h>

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

template<typename Problem>
void IDESolver<Problem>::solve(const llvm::Module& module) {
    // Local helpers and data structures mirroring the IFDS solver's needs
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;

    auto bottom = m_problem.bottom_value();

    // Clear any previous results
    m_values.clear();

    // Build simple call graph info
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> callee_to_calls;

    // Collect return instructions per function
    std::unordered_map<const llvm::Function*, std::vector<const llvm::ReturnInst*>> function_returns;

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;
        std::vector<const llvm::ReturnInst*> returns;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                    returns.push_back(ret);
                } else if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (const llvm::Function* callee = call->getCalledFunction()) {
                        call_to_callee[call] = callee;
                        callee_to_calls[callee].push_back(call);
                    }
                }
            }
        }
        function_returns[&func] = std::move(returns);
    }

    // Build CFG successor map
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
                } else if (llvm::isa<llvm::ReturnInst>(inst)) {
                    // no intraprocedural successors
                } else if (const llvm::Instruction* next = inst.getNextNode()) {
                    succs.push_back(next);
                }
                successors[&inst] = std::move(succs);
            }
        }
    }

    auto get_return_site = [](const llvm::CallInst* call) -> const llvm::Instruction* {
        return call->getNextNode();
    };

    // Worklist of (instruction, fact)
    struct Node {
        const llvm::Instruction* inst;
        Fact fact;
        bool operator==(const Node& other) const {
            return inst == other.inst && fact == other.fact;
        }
    };

    struct NodeHash {
        size_t operator()(const Node& n) const {
            size_t h1 = std::hash<const llvm::Instruction*>{}(n.inst);
            size_t h2 = std::hash<Fact>{}(n.fact);
            return h1 ^ (h2 << 1);
        }
    };

    std::vector<Node> worklist;

    auto queue_if_changed = [&](const llvm::Instruction* I, const Fact& f, const Value& incoming) {
        Value& current = m_values[I][f];
        Value joined = m_problem.join(current, incoming);
        if (!(joined == current)) {
            current = joined;
            worklist.push_back(Node{I, f});
        }
    };

    // Initialization: pick main or first non-declaration function
    const llvm::Function* main_func = module.getFunction("main");
    if (!main_func) {
        for (const llvm::Function& f : module) {
            if (!f.isDeclaration() && !f.empty()) { main_func = &f; break; }
        }
    }

    if (main_func && !main_func->empty()) {
        const llvm::Instruction* entry = &main_func->getEntryBlock().front();
        for (const auto& seed : m_problem.initial_facts(main_func)) {
            // Seed with top value for each initial fact
            queue_if_changed(entry, seed, m_problem.top_value());
        }
    }

    // Main IDE worklist loop
    while (!worklist.empty()) {
        Node node = worklist.back();
        worklist.pop_back();

        const llvm::Instruction* curr = node.inst;
        const Fact& fact = node.fact;
        const Value currVal = m_values[curr][fact];

        if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
            auto itCallee = call_to_callee.find(call);
            const llvm::Function* callee = (itCallee != call_to_callee.end()) ? itCallee->second : nullptr;
            if (!callee || callee->isDeclaration()) {
                // call-to-return edge
                const llvm::Instruction* retSite = get_return_site(call);
                if (retSite) {
                    for (const auto& tgtFact : m_problem.call_to_return_flow(call, fact)) {
                        auto ef = m_problem.call_to_return_edge_function(call, fact, tgtFact);
                        queue_if_changed(retSite, tgtFact, ef(currVal));
                    }
                }
            } else {
                // call edge to callee entry
                const llvm::Instruction* calleeEntry = &callee->getEntryBlock().front();
                for (const auto& calleeFact : m_problem.call_flow(call, callee, fact)) {
                    auto ef = m_problem.call_edge_function(call, fact, calleeFact);
                    queue_if_changed(calleeEntry, calleeFact, ef(currVal));
                }
            }
        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            const llvm::Function* func = ret->getFunction();
            auto itCalls = callee_to_calls.find(func);
            if (itCalls != callee_to_calls.end()) {
                for (const llvm::CallInst* call : itCalls->second) {
                    const llvm::Instruction* retSite = get_return_site(call);
                    if (!retSite) continue;
                    // Iterate over facts that have values at the call site
                    auto callMapIt = m_values.find(call);
                    if (callMapIt == m_values.end()) continue;
                    for (const auto& cfPair : callMapIt->second) {
                        const Fact& callFact = cfPair.first;
                        for (const auto& retFact : m_problem.return_flow(call, func, fact, callFact)) {
                            auto ef = m_problem.return_edge_function(call, fact, retFact);
                            queue_if_changed(retSite, retFact, ef(currVal));
                        }
                    }
                }
            }
        } else {
            // normal intraprocedural edges
            auto succsIt = successors.find(curr);
            if (succsIt != successors.end()) {
                for (const llvm::Instruction* succ : succsIt->second) {
                    for (const auto& tgtFact : m_problem.normal_flow(curr, fact)) {
                        auto ef = m_problem.normal_edge_function(curr, fact, tgtFact);
                        queue_if_changed(succ, tgtFact, ef(currVal));
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

