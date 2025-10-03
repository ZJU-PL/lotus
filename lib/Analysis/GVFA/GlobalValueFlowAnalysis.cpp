/**
 * @file GlobalValueFlowAnalysis.cpp
 * @brief Implementation of Global Value Flow Analysis using Dyck VFG
 *
 * This file implements the core global value flow analysis that tracks data flow
 * from vulnerability sources to sinks using the Dyck Value Flow Graph. It provides
 * both optimized and comprehensive analysis modes, with support for context-sensitive
 * reachability queries through CFL reachability.
 */

#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
//#include <algorithm>
#include <unordered_map>


#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
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

//===----------------------------------------------------------------------===//
// DyckGlobalValueFlowAnalysis Implementation
//===----------------------------------------------------------------------===//

/**
 * Constructs a new DyckGlobalValueFlowAnalysis instance.
 *
 * @param M The LLVM module to analyze
 * @param VFG The Dyck Value Flow Graph for value flow tracking
 * @param DyckAA The Dyck alias analysis for alias information
 * @param DyckMRA The Dyck mod-ref analysis for side-effect tracking
 */
DyckGlobalValueFlowAnalysis::DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG, 
                                                         DyckAliasAnalysis *DyckAA, 
                                                         DyckModRefAnalysis *DyckMRA)
    : VFG(VFG), DyckAA(DyckAA), DyckMRA(DyckMRA), M(M) {}

DyckGlobalValueFlowAnalysis::~DyckGlobalValueFlowAnalysis() = default;

/**
 * Sets the vulnerability checker for this analysis instance.
 *
 * @param checker A unique pointer to the vulnerability checker to use
 */
void DyckGlobalValueFlowAnalysis::setVulnerabilityChecker(std::unique_ptr<VulnerabilityChecker> checker) {
    VulnChecker = std::move(checker);
}

/**
 * Prints the accumulated online query time to the output stream.
 *
 * @param O The output stream to write to
 * @param Title The title prefix for the timing output
 */
void DyckGlobalValueFlowAnalysis::printOnlineQueryTime(llvm::raw_ostream &O, const char *Title) const {
    if (EnableOnlineQuery.getValue()) {
        long Ms = SnapshotedOnlineTime / 1000;
        O << Title << " Time: " << Ms / 1000 << "." << (Ms % 1000) / 100
          << (Ms % 1000) / 10 << "s\n";
    }
}

//===----------------------------------------------------------------------===//
// Counting helpers for reachability tracking
//===----------------------------------------------------------------------===//

/**
 * Counts and tracks reachability for a value with a given mask.
 *
 * This function implements an optimized reachability tracking mechanism where
 * each value can have multiple sources identified by bit masks. It returns
 * the uncovered portion of the mask that hasn't been processed yet.
 *
 * @param V The value to count reachability for
 * @param Mask The bit mask representing source identifiers
 * @return The uncovered portion of the mask that needs processing
 */
int DyckGlobalValueFlowAnalysis::count(const Value *V, int Mask) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return Mask & ~(Mask & It->second);
    } else {
        ReachabilityMap[V] = 0;
        return Mask;
    }
}

/**
 * Checks if a value has been counted before and marks it as counted.
 *
 * @param V The value to check and mark
 * @return true if the value was already counted, false otherwise
 */
bool DyckGlobalValueFlowAnalysis::count(const Value *V) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return true;
    } else {
        ReachabilityMap[V] = 1;
        return false;
    }
}

/**
 * Const version of count() that doesn't modify the reachability map.
 *
 * @param V The value to check reachability for
 * @param Mask The bit mask representing source identifiers
 * @return The uncovered portion of the mask that needs processing
 */
int DyckGlobalValueFlowAnalysis::countConst(const Value *V, int Mask) const {
    auto It = ReachabilityMap.find(V);
    return (It != ReachabilityMap.end()) ? (Mask & ~(Mask & It->second)) : Mask;
}

/**
 * Counts backward reachability for a value.
 *
 * @param V The value to count backward reachability for
 * @return The number of times this value has been reached backward
 */
int DyckGlobalValueFlowAnalysis::backwardCount(const Value *V) {
    auto It = BackwardReachabilityMap.find(V);
    if (It != BackwardReachabilityMap.end()) {
        return It->second;
    } else {
        BackwardReachabilityMap[V] = 1;
        return 0;
    }
}

/**
 * Const version of backwardCount() that doesn't modify the map.
 *
 * @param V The value to check backward reachability for
 * @return The number of times this value has been reached backward
 */
int DyckGlobalValueFlowAnalysis::backwardCountConst(const Value *V) const {
    auto It = BackwardReachabilityMap.find(V);
    return (It != BackwardReachabilityMap.end()) ? It->second : 0;
}

/**
 * Tracks all-pairs reachability from a source to a value.
 *
 * @param V The target value
 * @param Src The source value
 * @return true if this source was already tracked for this value
 */
bool DyckGlobalValueFlowAnalysis::allCount(const Value *V, const Value *Src) {
    auto &Set = AllReachabilityMap[V];
    if (Set.count(Src)) {
        return true;
    } else {
        Set.insert(Src);
        return false;
    }
}

/**
 * Const version of allCount() for checking all-pairs reachability.
 *
 * @param V The target value
 * @param Src The source value
 * @return true if the source can reach the value
 */
bool DyckGlobalValueFlowAnalysis::allCountConst(const Value *V, const Value *Src) const {
    auto It = AllReachabilityMap.find(V);
    return (It != AllReachabilityMap.end()) && It->second.count(Src);
}

/**
 * Tracks all-pairs backward reachability from a value to a sink.
 *
 * @param V The source value
 * @param Sink The sink value
 * @return true if this sink was already tracked for this value
 */
bool DyckGlobalValueFlowAnalysis::allBackwardCount(const Value *V, const Value *Sink) {
    auto &Set = AllBackwardReachabilityMap[V];
    if (Set.count(Sink)) {
        return true;
    } else {
        Set.insert(Sink);
        return false;
    }
}

/**
 * Const version of allBackwardCount() for checking all-pairs backward reachability.
 *
 * @param V The source value
 * @param Sink The sink value
 * @return true if the value can reach the sink
 */
bool DyckGlobalValueFlowAnalysis::allBackwardCountConst(const Value *V, const Value *Sink) const {
    auto It = AllBackwardReachabilityMap.find(V);
    return (It != AllBackwardReachabilityMap.end()) && It->second.count(Sink);
}

//===----------------------------------------------------------------------===//
// Query interfaces
//===----------------------------------------------------------------------===//

/**
 * Checks if a value is reachable from sources with the given mask.
 *
 * This function supports both online and offline query modes. In offline mode,
 * it uses pre-computed reachability information. In online mode, it performs
 * real-time slicing to determine reachability.
 *
 * @param V The value to check reachability for
 * @param Mask The bit mask representing source identifiers
 * @return The portion of the mask that corresponds to reachable sources
 */
int DyckGlobalValueFlowAnalysis::reachable(const Value *V, int Mask) {
    AllQueryCounter++;
    if (EnableOnlineQuery.getValue()) {
        auto start_time = std::chrono::high_resolution_clock::now();
        auto res = onlineSlicing(V);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                end_time - start_time).count();
        std::lock_guard<std::mutex> L(ClearMutex);
        SnapshotedOnlineTime += elapsed_time;
        if (res) {
            SuccsQueryCounter++;
            return 1;
        } else {
            return 0;
        }
    } else {
        int UncoveredMask = countConst(V, Mask);
        if (!UncoveredMask) {
            SuccsQueryCounter++;
        }
        return Mask & ~UncoveredMask;
    }
}

/**
 * Checks if a specific source can reach a target value.
 *
 * @param V The target value
 * @param Src The source value
 * @return true if the source can reach the target
 */
bool DyckGlobalValueFlowAnalysis::srcReachable(const Value *V, const Value *Src) const {
    return allCountConst(V, Src);
}

/**
 * Checks if a value can reach any sink through backward analysis.
 *
 * @param V The value to check backward reachability for
 * @return true if the value can reach any sink
 */
bool DyckGlobalValueFlowAnalysis::backwardReachable(const Value *V) {
    if (Sinks.empty()) {
        return true;
    }
    AllQueryCounter++;
    if (EnableOnlineQuery.getValue()) {
        auto start_time = std::chrono::high_resolution_clock::now();
        auto res = onlineSlicing(V);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                end_time - start_time).count();
        std::lock_guard<std::mutex> L(ClearMutex);
        SnapshotedOnlineTime += elapsed_time;
        if (res) {
            SuccsQueryCounter++;
            return true;
        } else {
            return false;
        }
    } else {
        if (!backwardCountConst(V)) {
            SuccsQueryCounter++;
            return false;
        } else {
            return true;
        }
    }
}

/**
 * Checks if a value can reach any specific sink.
 *
 * @param V The value to check reachability for
 * @return true if the value can reach any sink
 */
bool DyckGlobalValueFlowAnalysis::backwardReachableSink(const Value *V) {
    AllQueryCounter++;
    if (EnableOnlineQuery.getValue()) {
        auto start_time = std::chrono::high_resolution_clock::now();
        auto res = onlineSlicing(V);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                end_time - start_time).count();
        std::lock_guard<std::mutex> L(ClearMutex);
        SnapshotedOnlineTime += elapsed_time;
        if (res) {
            SuccsQueryCounter++;
            return true;
        } else {
            return false;
        }
    } else {
        auto It = AllBackwardReachabilityMap.find(V);
        if (It != AllBackwardReachabilityMap.end()) {
            if (It->second.empty()) {
                SuccsQueryCounter++;
                return false;
            } else {
                return true;
            }
        } else {
            SuccsQueryCounter++;
            return false;
        }
    }
}

/**
 * Checks if a value can reach all known sinks.
 *
 * @param V The value to check reachability for
 * @return true if the value can reach all sinks
 */
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
 * either the optimized or comprehensive analysis based on configuration.
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
        comprehensiveRun();
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
 * Runs the comprehensive version of the analysis.
 *
 * This version maintains detailed all-pairs reachability information,
 * providing complete source-to-sink mappings at the cost of higher
 * memory usage and computation time.
 */
void DyckGlobalValueFlowAnalysis::comprehensiveRun() {
    RecursiveTimer Timer("DyckGVFA-Comprehensive");
    
    AllReachabilityMap.clear();
    while (!SourcesVec.empty()) {
        LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzBeforeExtend: " << SourcesVec.size() << "\n");
        extendSources(SourcesVec);
        LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzAfterExtend: " << SourcesVec.size() << "\n");
        comprehensiveForwardRun(SourcesVec);
    }
    
    outs() << "[Indexing FW] Map Size: " << AllReachabilityMap.size() << "\n";
    
    AllBackwardReachabilityMap.clear();
    comprehensiveBackwardRun();
    
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
        forwardSlicing(Src.first, Src.second);
    }
}

/**
 * Runs the comprehensive forward analysis.
 *
 * @param Sources The vector of source-value pairs to analyze
 */
void DyckGlobalValueFlowAnalysis::comprehensiveForwardRun(std::vector<std::pair<const Value *, int>> &Sources) {
    auto SourceList = std::move(Sources);
    for (const auto &Src : SourceList) {
        comprehensiveForwardSlicing(Src.first, Src.first);
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
        backwardSlicing(Sink.first);
    }
}

/**
 * Runs the comprehensive backward analysis.
 *
 * This function performs detailed backward reachability analysis maintaining
 * complete sink-to-source mappings.
 */
void DyckGlobalValueFlowAnalysis::comprehensiveBackwardRun() {
    for (const auto &Sink : Sinks) {
        comprehensiveBackwardSlicing(Sink.first, Sink.first);
    }
}
