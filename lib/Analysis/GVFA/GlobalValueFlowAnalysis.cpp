/**
 * @file GlobalValueFlowAnalysis.cpp
 * @brief Global Value Flow Analysis using Dyck VFG
 *
 * Tracks data flow from vulnerability sources to sinks with optimized
 * and detailed analysis modes, plus context-sensitive CFL reachability.
 */

#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <unordered_map>


#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/GVFA/ReachabilityAlgorithms.h"
#include "Support/RecursiveTimer.h"

using namespace llvm;

#define DEBUG_TYPE "dyck-gvfa"

// Command line options for analysis configuration
static cl::opt<bool> EnableOnlineQuery("enable-online-query",
                                       cl::desc("enable online query"),
                                       cl::init(false));

static cl::opt<bool> EnableOptVFA("enable-opt-vfa",
                                 cl::desc("enable optimized value flow analysis"),
                                 cl::init(true), cl::ReallyHidden);

// Mutex for thread-safe online query timing
static std::mutex ClearMutex;

// Helper macro for online query timing
#define TIME_ONLINE_QUERY(expr) \
    if (EnableOnlineQuery.getValue()) { \
        auto start_time = std::chrono::high_resolution_clock::now(); \
        auto res = (expr); \
        auto end_time = std::chrono::high_resolution_clock::now(); \
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count(); \
        std::lock_guard<std::mutex> L(ClearMutex); \
        SnapshotedOnlineTime += elapsed_time; \
        if (res) SuccsQueryCounter++; \
        return res; \
    }

//===----------------------------------------------------------------------===//
// Implementation
//===----------------------------------------------------------------------===//

// Constructor

DyckGlobalValueFlowAnalysis::DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG, 
                                                         DyckAliasAnalysis *DyckAA, 
                                                         DyckModRefAnalysis *DyckMRA)
    : VFG(VFG), DyckAA(DyckAA), DyckMRA(DyckMRA), M(M) {}

DyckGlobalValueFlowAnalysis::~DyckGlobalValueFlowAnalysis() = default;

// Set vulnerability checker

void DyckGlobalValueFlowAnalysis::setVulnerabilityChecker(std::unique_ptr<VulnerabilityChecker> checker) {
    VulnChecker = std::move(checker);
}

// Print online query timing

void DyckGlobalValueFlowAnalysis::printOnlineQueryTime(llvm::raw_ostream &O, const char *Title) const {
    if (EnableOnlineQuery.getValue()) {
        long Ms = SnapshotedOnlineTime / 1000;
        O << Title << " Time: " << Ms / 1000 << "." << (Ms % 1000) / 100
          << (Ms % 1000) / 10 << "s\n";
    }
}

//===----------------------------------------------------------------------===//
// Reachability tracking
//===----------------------------------------------------------------------===//

// Count reachability with bit mask

int DyckGlobalValueFlowAnalysis::count(const Value *V, int Mask) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return Mask & ~(Mask & It->second);
    } else {
        ReachabilityMap[V] = 0;
        return Mask;
    }
}

// Check if value counted

bool DyckGlobalValueFlowAnalysis::count(const Value *V) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return true;
    } else {
        ReachabilityMap[V] = 1;
        return false;
    }
}


// Count backward reachability

int DyckGlobalValueFlowAnalysis::backwardCount(const Value *V) {
    auto It = BackwardReachabilityMap.find(V);
    if (It != BackwardReachabilityMap.end()) {
        return It->second;
    } else {
        BackwardReachabilityMap[V] = 1;
        return 0;
    }
}


// Track all-pairs reachability

bool DyckGlobalValueFlowAnalysis::allCount(const Value *V, const Value *Src) {
    auto &Set = AllReachabilityMap[V];
    if (Set.count(Src)) {
        return true;
    } else {
        Set.insert(Src);
        return false;
    }
}


// Track all-pairs backward reachability

bool DyckGlobalValueFlowAnalysis::allBackwardCount(const Value *V, const Value *Sink) {
    auto &Set = AllBackwardReachabilityMap[V];
    if (Set.count(Sink)) {
        return true;
    } else {
        Set.insert(Sink);
        return false;
    }
}


//===----------------------------------------------------------------------===//
// Query interfaces
//===----------------------------------------------------------------------===//

// Check reachability with mask

int DyckGlobalValueFlowAnalysis::reachable(const Value *V, int Mask) {
    AllQueryCounter++;
    TIME_ONLINE_QUERY(onlineReachability(V) ? 1 : 0);
    
    auto It = ReachabilityMap.find(V);
    int UncoveredMask = (It != ReachabilityMap.end()) ? (Mask & ~(Mask & It->second)) : Mask;
    if (!UncoveredMask) SuccsQueryCounter++;
    return Mask & ~UncoveredMask;
}

// Check source reachability

bool DyckGlobalValueFlowAnalysis::srcReachable(const Value *V, const Value *Src) const {
    auto It = AllReachabilityMap.find(V);
    return (It != AllReachabilityMap.end()) && It->second.count(Src);
}

// Check backward reachability

bool DyckGlobalValueFlowAnalysis::backwardReachable(const Value *V) {
    if (Sinks.empty()) return true;
    AllQueryCounter++;
    TIME_ONLINE_QUERY(onlineReachability(V));
    
    auto It = BackwardReachabilityMap.find(V);
    if (It == BackwardReachabilityMap.end() || It->second == 0) {
        SuccsQueryCounter++;
        return false;
    }
    return true;
}

// Check sink reachability

bool DyckGlobalValueFlowAnalysis::backwardReachableSink(const Value *V) {
    AllQueryCounter++;
    TIME_ONLINE_QUERY(onlineReachability(V));
    
    auto It = AllBackwardReachabilityMap.find(V);
    if (It != AllBackwardReachabilityMap.end() && !It->second.empty()) {
        return true;
    }
    SuccsQueryCounter++;
    return false;
}

// Check all sinks reachability

bool DyckGlobalValueFlowAnalysis::backwardReachableAllSinks(const Value *V) {
    auto It = AllBackwardReachabilityMap.find(V);
    if (It == AllBackwardReachabilityMap.end()) {
        return false;
    }
    if (It->second.size() != Sinks.size()) {
        return false;
    }
    return true;
}

//===----------------------------------------------------------------------===//
// VFG navigation helpers - optimized with caching
//===----------------------------------------------------------------------===//

/**
 * Gets all successors of a value in the Value Flow Graph.
 *
 * @param V The value to get successors for
 * @return A vector of successor values
 */
std::vector<const Value *> DyckGlobalValueFlowAnalysis::getSuccessors(const Value *V) const {
    std::vector<const Value *> Successors;
    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(V))) {
        Successors.reserve(std::distance(Node->begin(), Node->end()));
        for (auto It = Node->begin(); It != Node->end(); ++It) {
            Successors.push_back(It->first->getValue());
        }
    }
    return Successors;
}

/**
 * Gets all predecessors of a value in the Value Flow Graph.
 *
 * @param V The value to get predecessors for
 * @return A vector of predecessor values
 */
std::vector<const Value *> DyckGlobalValueFlowAnalysis::getPredecessors(const Value *V) const {
    std::vector<const Value *> Predecessors;
    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(V))) {
        Predecessors.reserve(std::distance(Node->in_begin(), Node->in_end()));
        for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
            Predecessors.push_back(It->first->getValue());
        }
    }
    return Predecessors;
}

/**
 * Checks if there is a direct value flow edge between two values.
 *
 * @param From The source value
 * @param To The target value
 * @return true if there is a direct edge from From to To
 */
bool DyckGlobalValueFlowAnalysis::isValueFlowEdge(const Value *From, const Value *To) const {
    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(From))) {
        for (auto It = Node->begin(); It != Node->end(); ++It) {
            if (It->first->getValue() == To) {
                return true;
            }
        }
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Call site management
//===----------------------------------------------------------------------===//

/**
 * Gets or assigns a unique ID for a call site.
 *
 * @param CI The call instruction
 * @return The unique ID for this call site
 */
int DyckGlobalValueFlowAnalysis::getCallSiteID(const CallInst *CI) {
    auto It = CallSiteIndexMap.find(CI);
    if (It == CallSiteIndexMap.end()) {
        int ID = CallSiteIndexMap.size() + 1;
        CallSiteIndexMap[CI] = ID;
        return ID;
    } else {
        return It->second;
    }
}

/**
 * Gets or assigns a unique ID for a call site and callee pair.
 *
 * @param CI The call instruction
 * @param Callee The called function
 * @return The unique ID for this call site-callee pair
 */
int DyckGlobalValueFlowAnalysis::getCallSiteID(const CallInst *CI, const Function *Callee) {
    std::pair<const CallInst *, const Function *> key = std::make_pair(CI, Callee);
    auto It = CallSiteCalleePairIndexMap.find(key);
    if (It == CallSiteCalleePairIndexMap.end()) {
        int ID = CallSiteIndexMap.size() + 1;
        CallSiteCalleePairIndexMap[key] = ID;
        return ID;
    } else {
        return It->second;
    }
}

//===----------------------------------------------------------------------===//
// Source extension using alias analysis - optimized with iterative approach
//===----------------------------------------------------------------------===//

/**
 * Extends the source set using alias analysis information.
 *
 * This function uses alias analysis to find additional sources that may
 * alias with the current sources. It processes function arguments, return
 * values, and follows VFG predecessors to build a comprehensive source set.
 *
 * @param Sources The vector of source-value pairs to extend
 */
void DyckGlobalValueFlowAnalysis::extendSources(std::vector<std::pair<const Value *, int>> &Sources) {
    std::unordered_map<const Value *, int> NewSrcMap;
    std::queue<std::pair<const Value *, int>> WorkQueue;
    
    // Initialize work queue with current sources
    for (const auto &Source : Sources) {
        WorkQueue.push(Source);
        NewSrcMap[Source.first] = Source.second;
    }
    
    Sources.clear();
    
    auto count_lambda = [&NewSrcMap, this](const Value *V, int Mask) -> int {
        int Uncovered = 0;
        
        auto It = NewSrcMap.find(V);
        if (It != NewSrcMap.end()) {
            Uncovered = Mask & ~(Mask & It->second);
        } else {
            NewSrcMap[V] = 0;
            Uncovered = Mask;
        }
        return this->count(V, Uncovered);
    };
    
    // Process work queue iteratively
    while (!WorkQueue.empty()) {
        auto front = WorkQueue.front();
        const Value *CurrentValue = front.first;
        int CurrentMask = front.second;
        WorkQueue.pop();
        
        if (count_lambda(CurrentValue, CurrentMask) == 0) {
            continue;
        }
        
        NewSrcMap[CurrentValue] |= CurrentMask;
        
        // Process function arguments and returns
        if (auto *Arg = dyn_cast<Argument>(CurrentValue)) {
            const Function *F = Arg->getParent();
            
            // Find call sites that call this function
            for (auto *User : F->users()) {
                if (auto *CI = dyn_cast<CallInst>(User)) {
                    if (CI->getCalledFunction() == F) {
                        unsigned ArgIdx = Arg->getArgNo();
                        if (ArgIdx < CI->arg_size()) {
                            auto *ActualArg = CI->getArgOperand(ArgIdx);
                            if (int UncoveredMask = count_lambda(ActualArg, CurrentMask)) {
                                WorkQueue.emplace(ActualArg, UncoveredMask);
                            }
                        }
                    }
                }
            }
        } else if (auto *CI = dyn_cast<CallInst>(CurrentValue)) {
            // Handle return values
            if (auto *F = CI->getCalledFunction()) {
                for (auto &BB : *F) {
                    for (auto &I : BB) {
                        if (auto *RI = dyn_cast<ReturnInst>(&I)) {
                            if (RI->getReturnValue()) {
                                if (int UncoveredMask = count_lambda(RI->getReturnValue(), CurrentMask)) {
                                    WorkQueue.emplace(RI->getReturnValue(), UncoveredMask);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // Follow VFG predecessors
            if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
                for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                    auto *Pred = It->first->getValue();
                    if (int UncoveredMask = count_lambda(Pred, CurrentMask)) {
                        WorkQueue.emplace(Pred, UncoveredMask);
                    }
                }
            }
        }
    }
    
    // Re-init sources using NewSrcMap
    for (auto &It : NewSrcMap) {
        if (It.second) {
            Sources.emplace_back(It.first, It.second);
        }
    }
}

//===----------------------------------------------------------------------===//
// Main analysis entry point
//===----------------------------------------------------------------------===//

/**
 * Runs the global value flow analysis.
 *
 * This is the main entry point that orchestrates the entire analysis process.
 * It retrieves sources and sinks from the vulnerability checker, then runs
 * either the optimized or detailed analysis based on configuration.
 */
void DyckGlobalValueFlowAnalysis::run() {
    if (!VulnChecker) {
        errs() << "Warning: No vulnerability checker set\n";
        return;
    }
    
    // Get sources and sinks from vulnerability checker
    VulnChecker->getSources(M, Sources);
    VulnChecker->getSinks(M, Sinks);
    
    if (Sources.empty() || Sinks.empty()) {
        return;
    }
    
    // Convert sources to vector format
    for (auto &It : Sources) {
        auto *SrcValue = It.first.first;
        int Mask = It.second;
        SourcesVec.emplace_back(SrcValue, Mask);
    }
    
    outs() << "#Sources: " << SourcesVec.size() << "\n";
    outs() << "#Sinks: " << Sinks.size() << "\n";
    
    if (EnableOptVFA.getValue()) {
        optimizedRun();
    } else {
        detailedRun();
    }
}

/**
 * Runs the optimized version of the analysis.
 *
 * This version uses bit-masked reachability tracking for better performance
 * at the cost of less detailed information. It processes sources iteratively
 * and maintains separate forward and backward reachability maps.
 */
void DyckGlobalValueFlowAnalysis::optimizedRun() {
    RecursiveTimer Timer("DyckGVFA-Optimized");
    
    ReachabilityMap.clear();
    while (!SourcesVec.empty()) {
        LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzBeforeExtend: " << SourcesVec.size() << "\n");
        extendSources(SourcesVec);
        LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzAfterExtend: " << SourcesVec.size() << "\n");
        optimizedForwardRun(SourcesVec);
    }
    
    LLVM_DEBUG({
        unsigned I = 0; 
        for (auto &It : ReachabilityMap) {
            if (It.second) ++I;
        } 
        dbgs() << "[DEBUG] ReachableNodesSz: " << I << "\n";
    });
    
    outs() << "[Opt-Indexing FW] Map Size: " << ReachabilityMap.size() << "\n";
    
    BackwardReachabilityMap.clear();
    optimizedBackwardRun();
    
    outs() << "[Opt-Indexing BW] Map Size: " << BackwardReachabilityMap.size() << "\n";
}

/**
 * Runs the detailed version of the analysis.
 *
 * This version maintains detailed all-pairs reachability information,
 * providing complete source-to-sink mappings at the cost of higher
 * memory usage and computation time.
 */
void DyckGlobalValueFlowAnalysis::detailedRun() {
    RecursiveTimer Timer("DyckGVFA-Detailed");
    
    AllReachabilityMap.clear();
    while (!SourcesVec.empty()) {
        LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzBeforeExtend: " << SourcesVec.size() << "\n");
        extendSources(SourcesVec);
        LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzAfterExtend: " << SourcesVec.size() << "\n");
        detailedForwardRun(SourcesVec);
    }
    
    outs() << "[Indexing FW] Map Size: " << AllReachabilityMap.size() << "\n";
    
    AllBackwardReachabilityMap.clear();
        detailedBackwardRun();
    
    outs() << "[Indexing BW] Map Size: " << AllBackwardReachabilityMap.size() << "\n";
}

//===----------------------------------------------------------------------===//
// Forward analysis
//===----------------------------------------------------------------------===//

/**
 * Runs the optimized forward analysis.
 *
 * @param Sources The vector of source-value pairs to analyze
 */
void DyckGlobalValueFlowAnalysis::optimizedForwardRun(std::vector<std::pair<const Value *, int>> &Sources) {
    auto SourceList = std::move(Sources);
    for (const auto &Src : SourceList) {
        forwardReachability(Src.first, Src.second);
    }
}

/**
 * Runs the detailed forward analysis.
 *
 * @param Sources The vector of source-value pairs to analyze
 */
void DyckGlobalValueFlowAnalysis::detailedForwardRun(std::vector<std::pair<const Value *, int>> &Sources) {
    auto SourceList = std::move(Sources);
    for (const auto &Src : SourceList) {
        detailedForwardReachability(Src.first, Src.first);
    }
}

//===----------------------------------------------------------------------===//
// Backward analysis  
//===----------------------------------------------------------------------===//

/**
 * Runs the optimized backward analysis.
 *
 * This function performs backward reachability analysis from all sinks
 * to identify values that can reach the sinks.
 */
void DyckGlobalValueFlowAnalysis::optimizedBackwardRun() {
    for (const auto &Sink : Sinks) {
        backwardReachability(Sink.first);
    }
}

/**
 * Runs the detailed backward analysis.
 *
 * This function performs detailed backward reachability analysis maintaining
 * complete sink-to-source mappings.
 */
void DyckGlobalValueFlowAnalysis::detailedBackwardRun() {
    for (const auto &Sink : Sinks) {
        detailedBackwardReachability(Sink.first, Sink.first);
    }
}
