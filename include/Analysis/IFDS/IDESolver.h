/*
 * IDE Solver
 *
 * This header provides the IDE (Interprocedural Distributive Environment)
 * solver implementation for the IFDS framework.
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

//#include <functional>
#include <unordered_map>

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
