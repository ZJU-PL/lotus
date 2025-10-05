/*
 *  Author: rainoftime
 *  Date: 2025-03
 *  Description: Context-sensitive null flow analysis
 */


#ifndef NULLPOINTER_CONTEXTSENSITIVENULLFLOWANALYSIS_H
#define NULLPOINTER_CONTEXTSENSITIVENULLFLOWANALYSIS_H

#include <llvm/Pass.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/CommandLine.h>
#include <set>
#include <map>
#include <unordered_map>
#include <string>

#include "Alias/DyckAA/DyckVFG.h"
#include "Analysis/NullPointer/AliasAnalysisAdapter.h"

using namespace llvm;

// Context sensitive context
typedef std::vector<CallInst *> Context;

// Function context pair
typedef std::pair<Function *, Context> FunctionContextPair;

// Hash function for FunctionContextPair
namespace std {
    template<>
    struct hash<FunctionContextPair> {
        size_t operator()(const FunctionContextPair &FCP) const {
            size_t H = hash<Function *>()(FCP.first);
            for (auto CI: FCP.second) {
                H += hash<CallInst *>()(CI);
            }
            return H;
        }
    };

    template<>
    struct equal_to<FunctionContextPair> {
        bool operator()(const FunctionContextPair &LHS, const FunctionContextPair &RHS) const {
            if (LHS.first != RHS.first) return false;
            if (LHS.second.size() != RHS.second.size()) return false;
            for (unsigned K = 0; K < LHS.second.size(); ++K) {
                if (LHS.second[K] != RHS.second[K]) return false;
            }
            return true;
        }
    };
}

// mapping from context to a set of nonnull args
typedef std::unordered_map<FunctionContextPair, std::set<std::pair<CallInst *, unsigned>>> NewNonNullEdgesMap;

class ContextSensitiveNullFlowAnalysis : public ModulePass {
private:
    // Alias analysis adapter - uses DyckAA
    AliasAnalysisAdapter *AAA;
    
    // VFG from DyckValueFlowAnalysis
    DyckVFG *VFG;
    
    // Max context depth
    unsigned MaxContextDepth;
    
    // NonNull edges collected during the analysis for each function & context
    NewNonNullEdgesMap NewNonNullEdges;
    
    // Internally created alias analysis adapter - needs to be deleted
    bool OwnsAliasAnalysisAdapter;

public:
    static char ID;

    ContextSensitiveNullFlowAnalysis();

    ~ContextSensitiveNullFlowAnalysis() override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;

    bool runOnModule(Module &M) override;

    // return true if Ptr can not be a null pointer
    bool notNull(Value *Ptr, Context Ctx) const;

    void add(Function *F, Context Ctx, Value *V1, Value *V2 = nullptr);

    void add(Function *F, Context Ctx, CallInst *CI, unsigned int K);

    void add(Function *F, Context Ctx, Value *Ret);

    // Helper method to get a context string for debugging
    std::string getContextString(const Context& Ctx) const;
    
    // Helper method to create a new context by extending an existing one
    Context extendContext(const Context& Ctx, CallInst* CI) const;
    
    // Recompute analysis with new non-null edges
    bool recompute(std::set<std::pair<Function*, Context>> &NewNonNullFunctionContexts);
};

#endif // NULLPOINTER_CONTEXTSENSITIVENULLFLOWANALYSIS_H 