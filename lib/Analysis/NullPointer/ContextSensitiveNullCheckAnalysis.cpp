/*
 *  Author: rainoftime
 *  Date: 2025-03
 *  Description: Context-sensitive null check analysis
 */


#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include "Analysis/NullPointer/ContextSensitiveLocalNullCheckAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"
#include "LLVMUtils/RecursiveTimer.h"

using namespace llvm;

static cl::opt<unsigned> CSRound("csnca-round", cl::init(2), cl::Hidden, cl::desc("# rounds for context-sensitive NCA"));
static cl::opt<unsigned> CSMaxContextDepth("csnca-max-depth", cl::init(3), cl::Hidden,
                                         cl::desc("Maximum depth of calling context to consider for NCA."));
// Define our own verbose option
static cl::opt<bool> CSVerbose("cs-verbose", cl::desc("Enable verbose output for context-sensitive analysis"), cl::init(false));
// Add option to control per-function statistics
static cl::opt<bool> CSPrintPerFunction("cs-print-per-function", cl::desc("Print per-function statistics for context-sensitive analysis"), cl::init(false));

char ContextSensitiveNullCheckAnalysis::ID = 0;
static RegisterPass<ContextSensitiveNullCheckAnalysis> X("csnca", "context-sensitive null check analysis.");

ContextSensitiveNullCheckAnalysis::ContextSensitiveNullCheckAnalysis() 
    : ModulePass(ID), MaxContextDepth(CSMaxContextDepth) {
}

ContextSensitiveNullCheckAnalysis::~ContextSensitiveNullCheckAnalysis() {
    for (auto &It: AnalysisMap) delete It.second;
    decltype(AnalysisMap)().swap(AnalysisMap);
}

void ContextSensitiveNullCheckAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<ContextSensitiveNullFlowAnalysis>();
}

bool ContextSensitiveNullCheckAnalysis::runOnModule(Module &M) {
    // record time
    RecursiveTimer TR("Running Context-Sensitive NullCheckAnalysis");

    // get the context-sensitive null flow analysis
    auto *NFA = &getAnalysis<ContextSensitiveNullFlowAnalysis>();

    // Start with empty context for all functions
    Context EmptyContext;
    std::set<std::pair<Function*, Context>> FuncsWithContexts;
    
    for (auto &F: M) {
        if (!F.empty()) {
            AnalysisMap[{&F, EmptyContext}] = nullptr;
            FuncsWithContexts.insert({&F, EmptyContext});
        }
    }

    // Generate analysis for each function with its context
    unsigned Count = 1;
    do {
        errs() << "CSNCA Iteration " << Count << "\n";
        
        // Process functions with contexts
        for (auto &FuncCtx : FuncsWithContexts) {
            Function *F = FuncCtx.first;
            const Context &Ctx = FuncCtx.second;
            
            // Create and run analysis for this function and context
            if (!AnalysisMap[{F, Ctx}]) {
                auto *LNCA = new ContextSensitiveLocalNullCheckAnalysis(NFA, F, Ctx);
                LNCA->run();
                AnalysisMap[{F, Ctx}] = LNCA;
                
                if (CSVerbose) {
                    errs() << "  Generated analysis for function " << F->getName() 
                          << " with context " << NFA->getContextString(Ctx) << "\n";
                }
            }
        }
        
        // This is where we'd normally call NFA->recompute, but since we've simplified
        // that part, we'll just ensure we've analyzed all functions
        FuncsWithContexts.clear();
            
    } while (Count++ < CSRound);
    
    // Ensure all functions have an analysis
    for (auto &F : M) {
        if (!F.empty()) {
            auto It = AnalysisMap.find({&F, EmptyContext});
            if (It == AnalysisMap.end() || !It->second) {
                auto *LNCA = new ContextSensitiveLocalNullCheckAnalysis(NFA, &F, EmptyContext);
                LNCA->run();
                AnalysisMap[{&F, EmptyContext}] = LNCA;
                
                if (CSVerbose) {
                    errs() << "  Generated fallback analysis for function " << F.getName() << "\n";
                }
            }
        }
    }

    // Build the k-limited context map for sound analysis
    buildKLimitedContextMap();

    // Collect and print statistics
    unsigned TotalPtrInsts = 0;
    unsigned NotNullPtrInsts = 0;
    std::map<Function*, std::pair<unsigned, unsigned>> FunctionStats;
    
    for (auto &F : M) {
        if (F.empty()) continue;
        
        unsigned FuncTotalPtrs = 0;
        unsigned FuncNotNullPtrs = 0;
        
        // Collect all contexts for this function
        std::vector<Context> FunctionContexts;
        for (auto &Entry : AnalysisMap) {
            if (Entry.first.first == &F && Entry.second != nullptr) {
                FunctionContexts.push_back(Entry.first.second);
            }
        }
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                // Count pointer operands
                for (unsigned i = 0; i < I.getNumOperands(); i++) {
                    Value *Op = I.getOperand(i);
                    if (Op->getType()->isPointerTy()) {
                        TotalPtrInsts++;
                        FuncTotalPtrs++;
                        
                        // Check if this pointer is NOT_NULL in ANY analyzed context
                        bool IsNotNull = false;
                        for (const Context &Ctx : FunctionContexts) {
                            if (!mayNull(Op, &I, Ctx)) {
                                IsNotNull = true;
                                break;
                            }
                        }
                        
                        // If it's not null in any context, count it as NOT_NULL
                        if (IsNotNull) {
                            NotNullPtrInsts++;
                            FuncNotNullPtrs++;
                        }
                    }
                }
            }
        }
        
        FunctionStats[&F] = {FuncTotalPtrs, FuncNotNullPtrs};
    }
    
    errs() << "\n=== Context-Sensitive Analysis Statistics ===\n";
    errs() << "Total pointer operands: " << TotalPtrInsts << "\n";
    errs() << "Pointer operands proven NOT_NULL: " << NotNullPtrInsts << "\n";
    errs() << "Percentage of NOT_NULL pointers: " << 
        (TotalPtrInsts > 0 ? (NotNullPtrInsts * 100.0 / TotalPtrInsts) : 0) << "%\n";
    
    // Only print per-function statistics if enabled
    if (CSPrintPerFunction) {
        errs() << "\nPer-function statistics:\n";
        for (auto &Stat : FunctionStats) {
            Function *F = Stat.first;
            unsigned FuncTotal = Stat.second.first;
            unsigned FuncNotNull = Stat.second.second;
            
            if (FuncTotal > 0) {
                errs() << "  " << F->getName() << ": " 
                       << FuncNotNull << "/" << FuncTotal << " NOT_NULL pointers ("
                       << (FuncNotNull * 100.0 / FuncTotal) << "%)\n";
            }
        }
    }
    errs() << "================================================\n\n";

    // Print verbose output if requested
    if (CSVerbose) {
        errs() << "\n=== Context-Sensitive Null Check Analysis Results ===\n";
        
        // Print all functions in the module
        errs() << "Functions in module:\n";
        for (auto &F : M) {
            errs() << "  " << F.getName() << (F.empty() ? " (empty)" : "") << "\n";
        }
        errs() << "\n";
        
        // Print all entries in the analysis map
        errs() << "Entries in AnalysisMap:\n";
        for (auto &Entry : AnalysisMap) {
            errs() << "  Function: " << Entry.first.first->getName() 
                   << ", Context: " << getContextString(Entry.first.second)
                   << ", Analysis: " << (Entry.second ? "present" : "null") << "\n";
        }
        errs() << "\n";
        
        for (auto &F : M) {
            if (F.empty()) continue;
            
            errs() << "Function: " << F.getName() << "\n";
            
            // Print results for each context
            bool foundContexts = false;
            for (auto &Entry : AnalysisMap) {
                if (Entry.first.first != &F) continue;
                
                foundContexts = true;
                const Context &Ctx = Entry.first.second;
                auto *LNCA = Entry.second;
                if (!LNCA) continue;
                
                errs() << "  Context: " << getContextString(Ctx) << "\n";
                
                // Print pointer analysis results for this function and context
                bool foundPointers = false;
                for (auto &BB : F) {
                    for (auto &I : BB) {
                        for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                            Value *Op = I.getOperand(i);
                            if (!Op->getType()->isPointerTy()) continue;
                            
                            foundPointers = true;
                            bool MayBeNull = LNCA->mayNull(Op, &I);
                            errs() << "    " << (MayBeNull ? "MAY_NULL" : "NOT_NULL") 
                                   << ": " << *Op << " at " << I << "\n";
                        }
                    }
                }
                
                if (!foundPointers) {
                    errs() << "    No pointer operands found in this function\n";
                }
                
                errs() << "\n";
            }
            
            if (!foundContexts) {
                errs() << "  No contexts analyzed for this function\n\n";
            }
        }
        errs() << "=== End of Analysis Results ===\n\n";
    }

    return false;
}

// Get a k-limited context (most recent k call sites)
Context ContextSensitiveNullCheckAnalysis::getKLimitedContext(const Context &Ctx) const {
    if (Ctx.size() <= MaxContextDepth) {
        return Ctx;
    }
    
    // Get the k most recent call sites (keep the suffix)
    Context KLimitedCtx;
    auto startPos = Ctx.size() - MaxContextDepth;
    KLimitedCtx.insert(KLimitedCtx.end(), Ctx.begin() + startPos, Ctx.end());
    return KLimitedCtx;
}

// Build the KLimitedContextMap for sound k-limiting
void ContextSensitiveNullCheckAnalysis::buildKLimitedContextMap() {
    // Clear any existing mappings
    KLimitedContextMap.clear();
    
    // Build a map from k-limited contexts to all full contexts that share that k-suffix
    for (const auto &Entry : AnalysisMap) {
        const Function* F = Entry.first.first;
        const Context& FullCtx = Entry.first.second;
        
        // Get the k-limited version of this context
        Context KLimitedCtx = getKLimitedContext(FullCtx);
        
        // Add this full context to the set of contexts with the same k-limited version
        std::pair<Function*, Context> key = {const_cast<Function*>(F), KLimitedCtx};
        KLimitedContextMap[key].insert(FullCtx);
    }
    
    if (CSVerbose) {
        errs() << "\nK-Limited Context Mappings (k=" << MaxContextDepth << "):\n";
        for (const auto &Entry : KLimitedContextMap) {
            errs() << "  Function: " << Entry.first.first->getName() 
                  << ", K-Limited Context: " << getContextString(Entry.first.second) << "\n";
            errs() << "    Maps to " << Entry.second.size() << " full context(s):\n";
            for (const auto &FullCtx : Entry.second) {
                errs() << "      " << getContextString(FullCtx) << "\n";
            }
        }
        errs() << "\n";
    }
}

// Check if a pointer may be null in any context with the same k-suffix
bool ContextSensitiveNullCheckAnalysis::mayNullInAnyMatchingContext(Value *Ptr, Instruction *Inst, 
                                                                   const Context &KLimitedCtx) {
    // Look up all contexts that share the same k-suffix
    std::pair<Function*, Context> key = {Inst->getFunction(), KLimitedCtx};
    auto It = KLimitedContextMap.find(key);
    if (It == KLimitedContextMap.end() || It->second.empty()) {
        // No matching contexts found, conservatively return true (may be null)
        return true;
    }
    
    // Check all contexts that share this k-suffix
    // For sound analysis: if ANY context says it may be null, then the result is may be null
    for (const Context &FullCtx : It->second) {
        auto AnalysisIt = AnalysisMap.find({Inst->getFunction(), FullCtx});
        if (AnalysisIt == AnalysisMap.end() || !AnalysisIt->second) {
            // No analysis for this context, conservatively assume may be null
            return true;
        }
        
        // If this context's analysis says it may be null, then the overall result is may be null
        if (AnalysisIt->second->mayNull(Ptr, Inst)) {
            return true;
        }
    }
    
    // All contexts with the same k-suffix agree that the pointer is NOT_NULL
    return false;
}

bool ContextSensitiveNullCheckAnalysis::mayNull(Value *Ptr, Instruction *Inst, const Context &Ctx) {
    // First check if the context-sensitive flow analysis says it's not null in this context
    auto *NFA = &getAnalysis<ContextSensitiveNullFlowAnalysis>();
    if (NFA->notNull(Ptr, Ctx)) {
        return false; // If flow analysis proves NOT_NULL in this context, then it's definitely NOT_NULL
    }
    
    // Get the k-limited version of this context (last k call sites)
    Context KLimitedCtx = getKLimitedContext(Ctx);
    
    // First, try exact context match if possible
    std::pair<Function*, Context> exKey = {Inst->getFunction(), Ctx};
    auto ExactIt = AnalysisMap.find(exKey);
    if (ExactIt != AnalysisMap.end() && ExactIt->second) {
        return ExactIt->second->mayNull(Ptr, Inst);
    }
    
    // Otherwise, use sound k-limiting: merge results from all contexts with same k-suffix
    return mayNullInAnyMatchingContext(Ptr, Inst, KLimitedCtx);
}

std::string ContextSensitiveNullCheckAnalysis::getContextString(const Context& Ctx) const {
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