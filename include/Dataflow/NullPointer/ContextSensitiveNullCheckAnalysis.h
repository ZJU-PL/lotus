/*
 *  Author: rainoftime
 *  Date: 2025-03
 *  Description: Context-sensitive null check analysis
 */

#ifndef NULLPOINTER_CONTEXTSENSITIVENULLCHECKANALYSIS_H
#define NULLPOINTER_CONTEXTSENSITIVENULLCHECKANALYSIS_H

#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <list>
#include <set>
#include <unordered_map>
#include <map>
#include "Dataflow/NullPointer/ContextSensitiveNullFlowAnalysis.h"

using namespace llvm;

class ContextSensitiveLocalNullCheckAnalysis;

class ContextSensitiveNullCheckAnalysis : public ModulePass {
private:
    // Maps a function and context to its local null check analysis
    std::map<std::pair<Function*, Context>, ContextSensitiveLocalNullCheckAnalysis*> AnalysisMap;
    
    // Maximum context depth to consider (to prevent context explosion)
    unsigned MaxContextDepth;
    
    // Maps a function and k-limited context to all contexts that share the same k suffix
    std::map<std::pair<Function*, Context>, std::set<Context>> KLimitedContextMap;

public:
    static char ID;

    ContextSensitiveNullCheckAnalysis();
    ~ContextSensitiveNullCheckAnalysis() override;

    bool runOnModule(Module &M) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override;

public:
    /// \p Ptr must be an operand of \p Inst
    /// return true if \p Ptr at \p Inst may be a null pointer in the given context
    bool mayNull(Value *Ptr, Instruction *Inst, const Context &Ctx);
    
    /// Helper method to get a context string for debugging
    std::string getContextString(const Context& Ctx) const;
    
    /// Get a k-limited context (most recent k call sites)
    Context getKLimitedContext(const Context &Ctx) const;
    
    /// Build the KLimitedContextMap for sound k-limiting
    void buildKLimitedContextMap();
    
    /// Check if a pointer may be null in any context with the same k-suffix
    bool mayNullInAnyMatchingContext(Value *Ptr, Instruction *Inst, const Context &KLimitedCtx);
};

#endif // NULLPOINTER_CONTEXTSENSITIVENULLCHECKANALYSIS_H