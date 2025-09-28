/*
 *  Author: rainoftime
 *  Date: 2025-03
 *  Description: Context-sensitive local null check analysis
 */

#ifndef NULLPOINTER_CONTEXTSENSITIVELOCALNULLCHECKANALYSIS_H
#define NULLPOINTER_CONTEXTSENSITIVELOCALNULLCHECKANALYSIS_H

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Pass.h>
#include <set>
#include <unordered_map>
#include <map>

// #include "Alias/DyckAA/DyckValueFlowAnalysis.h"
#include "Analysis/NullPointer/NullEquivalenceAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"

using namespace llvm;

using Edge = std::pair<Instruction *, unsigned>;

class ContextSensitiveLocalNullCheckAnalysis {
private:
    /// Mapping an instruction to a mask, if ith bit of mask is set, it must not be null pointer
    std::unordered_map<Instruction *, int32_t> InstNonNullMap;

    /// Ptr -> ID
    std::unordered_map<Value *, size_t> PtrIDMap;

    /// Edge -> a BitVector, in which if IDth bit is set, the corresponding ptr is not null
    std::map<Edge, BitVector> DataflowFacts;

    /// unreachable edges collected during nca
    std::set<Edge> UnreachableEdges;

    /// The function we analyze
    Function *F;

    /// The calling context for this analysis
    Context Ctx;

    /// ptr groups
    NullEquivalenceAnalysis NEA;

    /// nfa
    ContextSensitiveNullFlowAnalysis *NFA;

    /// dt
    DominatorTree DT;

public:
    ContextSensitiveLocalNullCheckAnalysis(ContextSensitiveNullFlowAnalysis* NFA, Function *F, const Context &Ctx);

    ~ContextSensitiveLocalNullCheckAnalysis();

    /// \p Ptr must be an operand of \p Inst
    /// return true if \p Ptr at \p Inst may be a null pointer
    bool mayNull(Value *Ptr, Instruction *Inst);

    void run();

private:
    void nca();

    void merge(std::vector<Edge> &, BitVector &);

    void transfer(Edge, const BitVector &, BitVector &);

    void tag();

    void init();

    void label();

    void label(Edge);
};

#endif //NULLPOINTER_CONTEXTSENSITIVELOCALNULLCHECKANALYSIS_H 