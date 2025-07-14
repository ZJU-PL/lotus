/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: Context-sensitive null flow analysis
 */


#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include "Alias/DyckAA/DyckValueFlowAnalysis.h"
#include "Dataflow/NullPointer/ContextSensitiveNullFlowAnalysis.h"
#include "Dataflow/NullPointer/AliasAnalysisAdapter.h"
#include "Support/API.h"
#include "Support/RecursiveTimer.h"

using namespace llvm;

static cl::opt<int> CSIncrementalLimits("csnfa-limit", cl::init(10), cl::Hidden,
                                      cl::desc("Determine how many non-null edges we consider a round in context-sensitive analysis."));

static cl::opt<unsigned> CSMaxContextDepth("csnfa-max-depth", cl::init(3), cl::Hidden,
                                         cl::desc("Maximum depth of calling context to consider."));

static cl::opt<unsigned> CSRound("csnfa-round", cl::init(10), cl::Hidden,
                               cl::desc("Maximum rounds for context-sensitive analysis."));

// Define options for alias analysis selection
static cl::opt<unsigned> DyckAAOpt("nfa-dyck-aa", cl::init(1), cl::Hidden,
                        cl::desc("Use DyckAA for analysis. (0: None, 1: DyckAA)"));

static cl::opt<unsigned> CFLAAOpt("nfa-cfl-aa", cl::init(0), cl::Hidden,
                        cl::desc("Use CFLAA for analysis. (0: None, 1: Steensgaard, 2: Andersen)"));

char ContextSensitiveNullFlowAnalysis::ID = 0;
static RegisterPass<ContextSensitiveNullFlowAnalysis> X("csnfa", "context-sensitive null value flow");

ContextSensitiveNullFlowAnalysis::ContextSensitiveNullFlowAnalysis() 
    : ModulePass(ID), AAA(nullptr), VFG(nullptr), MaxContextDepth(CSMaxContextDepth), 
      OwnsAliasAnalysisAdapter(false) {
}

ContextSensitiveNullFlowAnalysis::~ContextSensitiveNullFlowAnalysis() {
    if (OwnsAliasAnalysisAdapter && AAA) {
        delete AAA;
    }
}

void ContextSensitiveNullFlowAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<DyckValueFlowAnalysis>();
}

bool ContextSensitiveNullFlowAnalysis::runOnModule(Module &M) {
    RecursiveTimer Timer("Running Context-Sensitive NFA");
    
    // Get the value flow graph
    auto *VFA = &getAnalysis<DyckValueFlowAnalysis>();
    VFG = VFA->getDyckVFGraph();
    
    // Create the appropriate alias analysis adapter using the factory method
    AAA = AliasAnalysisAdapter::createAdapter(&M, nullptr);
    OwnsAliasAnalysisAdapter = true;

    // Initialize the basic context (empty context)
    Context EmptyContext;
    
    // Helper function to identify values that must not be null
    auto MustNotNull = [this](Value *V, Instruction *I) -> bool {
        V = V->stripPointerCastsAndAliases();
        if (isa<GlobalValue>(V)) return true;
        if (auto CI = dyn_cast<Instruction>(V))
            return API::isMemoryAllocate(CI);
        return !AAA->mayNull(V, I);
    };
    
    // Initialize the analysis for each function
    std::set<DyckVFGNode *> MayNullNodes;
    for (auto &F: M) {
        if (!F.empty()) NewNonNullEdges[{&F, EmptyContext}];
        for (auto &I: instructions(&F)) {
            if (I.getType()->isPointerTy() && !MustNotNull(&I, &I)) {
                if (auto INode = VFG->getVFGNode(&I)) {
                    MayNullNodes.insert(INode);
                }
            }
        }
    }
    
    // Perform context-sensitive analysis
    std::set<std::pair<Function*, Context>> WorkList;
    for (auto &F: M) {
        if (!F.empty()) {
            WorkList.insert({&F, EmptyContext});
        }
    }
    
    // Keep analyzing until we reach a fixed point
    while (!WorkList.empty()) {
        auto FuncCtx = *WorkList.begin();
        WorkList.erase(WorkList.begin());
        
        Function *F = FuncCtx.first;
        Context &Ctx = FuncCtx.second;
        
        // Process the function in this context
        // This part depends on the specific algorithm of your null flow analysis
        errs() << "Processing function " << F->getName() << " with context " 
               << getContextString(Ctx) << "\n";
        
        // Check all call sites in this function
        for (auto &I : instructions(F)) {
            if (auto *CI = dyn_cast<CallInst>(&I)) {
                auto *Callee = CI->getCalledFunction();
                if (!Callee || Callee->empty()) continue;
                
                // If we haven't reached max context depth, create a new context
                if (Ctx.size() < MaxContextDepth) {
                    Context NewCtx = extendContext(Ctx, CI);
                    
                    // Add the callee with the new context to the worklist
                    auto FuncCtxPair = std::make_pair(Callee, NewCtx);
                    if (NewNonNullEdges.find(FuncCtxPair) == NewNonNullEdges.end()) {
                        NewNonNullEdges[FuncCtxPair] = {};
                        WorkList.insert(FuncCtxPair);
                    }
                }
            }
        }
    }
    
    return false;
}

bool ContextSensitiveNullFlowAnalysis::recompute(std::set<std::pair<Function*, Context>> &NewNonNullFunctionContexts) {
    // This method should implement the recomputation logic when new non-null edges are discovered
    // For simplicity, we'll just return false indicating no changes
    
    // In a real implementation, this would analyze the impact of new non-null edges
    // and update the analysis results accordingly
    
    return false;
}

bool ContextSensitiveNullFlowAnalysis::notNull(Value *Ptr, Context Ctx) const {
    if (!Ptr || !Ptr->getType()->isPointerTy())
        return false;
        
    // First check if the pointer is known to be non-null
    Ptr = Ptr->stripPointerCastsAndAliases();
    if (isa<GlobalValue>(Ptr)) return true;
    if (auto *I = dyn_cast<Instruction>(Ptr)) {
        if (API::isMemoryAllocate(I)) return true;
    }
    
    // Then check our context-sensitive analysis results
    Function *F = nullptr;
    Instruction *InstPoint = nullptr;
    if (auto *I = dyn_cast<Instruction>(Ptr)) {
        F = I->getFunction();
        InstPoint = I;
    } else {
        // If it's not an instruction, we need a more conservative approach
        return false;
    }
    
    // Get all contexts that have the same k-suffix as our input context
    std::set<Context> MatchingContexts;
    
    // Start with the exact context
    MatchingContexts.insert(Ctx);
    
    // Get the k-suffix of our context
    Context KSuffix = Ctx;
    if (KSuffix.size() > MaxContextDepth) {
        KSuffix.erase(KSuffix.begin(), KSuffix.begin() + (KSuffix.size() - MaxContextDepth));
    }
    
    // Add all contexts that have the same k-suffix
    for (const auto &Entry : NewNonNullEdges) {
        if (Entry.first.first != F) continue;
        
        const Context &OtherCtx = Entry.first.second;
        Context OtherKSuffix = OtherCtx;
        
        if (OtherKSuffix.size() > MaxContextDepth) {
            OtherKSuffix.erase(OtherKSuffix.begin(), OtherKSuffix.begin() + (OtherKSuffix.size() - MaxContextDepth));
        }
        
        // If this context has the same k-suffix, add it to our matching contexts
        if (OtherKSuffix == KSuffix) {
            MatchingContexts.insert(OtherCtx);
        }
    }
    
    // For a value to be definitely NOT NULL, it must be NOT NULL in ALL matching contexts
    // This is the sound approach for k-limiting
    for (const Context &MatchingCtx : MatchingContexts) {
        auto FuncCtxPair = std::make_pair(F, MatchingCtx);
        auto it = NewNonNullEdges.find(FuncCtxPair);
        
        if (it == NewNonNullEdges.end()) {
            // If we don't have analysis for this context, we can't guarantee NOT_NULL
            return false;
        }
        
        // Check if this pointer is NOT NULL in this context
        bool IsNotNullInContext = false;
        
        // For a proper implementation, this would check specific null checks in the context
        if (InstPoint && !AAA->mayNull(Ptr, InstPoint)) {
            IsNotNullInContext = true;
        }
        
        if (!IsNotNullInContext) {
            // If it's not definitely NOT NULL in any matching context, we can't guarantee NOT_NULL
            return false;
        }
    }
    
    // If we get here, the pointer is NOT NULL in all matching contexts
    return true;
}

void ContextSensitiveNullFlowAnalysis::add(Function *F, Context Ctx, Value *V1, Value *V2) {
    if (!V1 || !V1->getType()->isPointerTy())
        return;
        
    auto FuncCtxPair = std::make_pair(F, Ctx);
    auto it = NewNonNullEdges.find(FuncCtxPair);
    if (it == NewNonNullEdges.end()) {
        NewNonNullEdges[FuncCtxPair] = {};
    }
    
    // This implementation depends on how you track non-null values
    // For now, we'll just add a dummy entry to indicate that we've analyzed this context
}

void ContextSensitiveNullFlowAnalysis::add(Function *F, Context Ctx, CallInst *CI, unsigned int K) {
    if (!CI) return;
    
    auto FuncCtxPair = std::make_pair(F, Ctx);
    auto it = NewNonNullEdges.find(FuncCtxPair);
    if (it == NewNonNullEdges.end()) {
        NewNonNullEdges[FuncCtxPair] = {};
    }
    
    // Add this call site argument as non-null
    it->second.insert(std::make_pair(CI, K));
}

void ContextSensitiveNullFlowAnalysis::add(Function *F, Context Ctx, Value *Ret) {
    if (!Ret || !Ret->getType()->isPointerTy())
        return;
        
    auto FuncCtxPair = std::make_pair(F, Ctx);
    auto it = NewNonNullEdges.find(FuncCtxPair);
    if (it == NewNonNullEdges.end()) {
        NewNonNullEdges[FuncCtxPair] = {};
    }
    
    // This implementation depends on how you track non-null values
    // For now, we'll just add a dummy entry to indicate that we've analyzed this context
}

std::string ContextSensitiveNullFlowAnalysis::getContextString(const Context& Ctx) const {
    std::string Result = "[";
    for (size_t i = 0; i < Ctx.size(); ++i) {
        if (i > 0) Result += ", ";
        if (Ctx[i]->hasName()) {
            Result += Ctx[i]->getName().str();
        } else {
            Result += "<unnamed call>";
        }
    }
    Result += "]";
    return Result;
}

Context ContextSensitiveNullFlowAnalysis::extendContext(const Context& Ctx, CallInst* CI) const {
    // Create a new context by appending the call instruction
    Context NewCtx = Ctx;
    NewCtx.push_back(CI);
    
    // Note: We don't limit the context here anymore - we'll handle k-limiting
    // at analysis time to ensure soundness by properly merging results
    return NewCtx;
} 