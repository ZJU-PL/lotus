#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <stack>
#include <queue>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "Dataflow/GVFA/GlobalValueFlowAnalysis.h"
#include "Checker/Taint/TaintUtils.h"
#include "Support/RecursiveTimer.h"

using namespace llvm;

#define DEBUG_TYPE "dyck-gvfa"

static cl::opt<bool> EnableOnlineQuery("enable-online-query",
                                       cl::desc("enable online query"),
                                       cl::init(false));

static cl::opt<bool> EnableOptVFA("enable-opt-vfa",
                                 cl::desc("enable optimized value flow analysis"),
                                 cl::init(true), cl::ReallyHidden);



static std::mutex ClearMutex;

//===----------------------------------------------------------------------===//
// DyckGlobalValueFlowAnalysis Implementation
//===----------------------------------------------------------------------===//

DyckGlobalValueFlowAnalysis::DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG, 
                                                         DyckAliasAnalysis *DyckAA, 
                                                         DyckModRefAnalysis *DyckMRA)
    : VFG(VFG), DyckAA(DyckAA), DyckMRA(DyckMRA), M(M) {}

DyckGlobalValueFlowAnalysis::~DyckGlobalValueFlowAnalysis() = default;

void DyckGlobalValueFlowAnalysis::setVulnerabilityChecker(std::unique_ptr<VulnerabilityChecker> checker) {
    VulnChecker = std::move(checker);
}

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

int DyckGlobalValueFlowAnalysis::count(const Value *V, int Mask) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return Mask & ~(Mask & It->second);
    } else {
        ReachabilityMap[V] = 0;
        return Mask;
    }
}

bool DyckGlobalValueFlowAnalysis::count(const Value *V) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return true;
    } else {
        ReachabilityMap[V] = 1;
        return false;
    }
}

int DyckGlobalValueFlowAnalysis::countConst(const Value *V, int Mask) const {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return Mask & ~(Mask & It->second);
    } else {
        return Mask;
    }
}

int DyckGlobalValueFlowAnalysis::backwardCount(const Value *V) {
    auto It = BackwardReachabilityMap.find(V);
    if (It != BackwardReachabilityMap.end()) {
        return It->second;
    } else {
        BackwardReachabilityMap[V] = 1;
        return 0;
    }
}

int DyckGlobalValueFlowAnalysis::backwardCountConst(const Value *V) const {
    auto It = BackwardReachabilityMap.find(V);
    if (It != BackwardReachabilityMap.end()) {
        return It->second;
    } else {
        return 0;
    }
}

bool DyckGlobalValueFlowAnalysis::allCount(const Value *V, const Value *Src) {
    auto It = AllReachabilityMap.find(V);
    if (It != AllReachabilityMap.end()) {
        if (It->second.count(Src)) {
            return true;
        } else {
            AllReachabilityMap[V].insert(Src);
            return false;
        }
    } else {
        AllReachabilityMap.insert(std::make_pair(V, std::unordered_set<const Value *>()));
        AllReachabilityMap[V].insert(Src);
        return false;
    }
}

bool DyckGlobalValueFlowAnalysis::allCountConst(const Value *V, const Value *Src) const {
    auto It = AllReachabilityMap.find(V);
    if (It != AllReachabilityMap.end()) {
        return It->second.count(Src);
    } else {
        return false;
    }
}

bool DyckGlobalValueFlowAnalysis::allBackwardCount(const Value *V, const Value *Sink) {
    auto It = AllBackwardReachabilityMap.find(V);
    if (It != AllBackwardReachabilityMap.end()) {
        if (It->second.count(Sink)) {
            return true;
        } else {
            AllBackwardReachabilityMap[V].insert(Sink);
            return false;
        }
    } else {
        AllBackwardReachabilityMap.insert(std::make_pair(V, std::unordered_set<const Value *>()));
        AllBackwardReachabilityMap[V].insert(Sink);
        return false;
    }
}

bool DyckGlobalValueFlowAnalysis::allBackwardCountConst(const Value *V, const Value *Sink) const {
    auto It = AllBackwardReachabilityMap.find(V);
    if (It != AllBackwardReachabilityMap.end()) {
        return It->second.count(Sink);
    } else {
        return false;
    }
}

//===----------------------------------------------------------------------===//
// Query interfaces
//===----------------------------------------------------------------------===//

int DyckGlobalValueFlowAnalysis::reachable(const Value *V, int Mask) {
    AllQueryCounter++;
    if (EnableOnlineQuery.getValue()) {
        auto start_time = std::chrono::high_resolution_clock::now();
        auto res = onlineSlicing(V);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                end_time - start_time)
                                .count();
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

bool DyckGlobalValueFlowAnalysis::srcReachable(const Value *V, const Value *Src) const {
    return allCountConst(V, Src);
}

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
                                end_time - start_time)
                                .count();
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

bool DyckGlobalValueFlowAnalysis::backwardReachableSink(const Value *V) {
    AllQueryCounter++;
    if (EnableOnlineQuery.getValue()) {
        auto start_time = std::chrono::high_resolution_clock::now();
        auto res = onlineSlicing(V);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                end_time - start_time)
                                .count();
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
        auto [CurrentValue, CurrentMask] = WorkQueue.front();
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

void DyckGlobalValueFlowAnalysis::optimizedForwardRun(std::vector<std::pair<const Value *, int>> &Sources) {
    std::vector<std::pair<const Value *, int>> SourceList = std::move(Sources);
    for (auto Src : SourceList) {
        forwardSlicing(Src.first, Src.second);
    }
}

void DyckGlobalValueFlowAnalysis::comprehensiveForwardRun(std::vector<std::pair<const Value *, int>> &Sources) {
    std::vector<std::pair<const Value *, int>> SourceList = std::move(Sources);
    for (auto Src : SourceList) {
        comprehensiveForwardSlicing(Src.first, Src.first);
    }
}

//===----------------------------------------------------------------------===//
// Backward analysis  
//===----------------------------------------------------------------------===//

void DyckGlobalValueFlowAnalysis::optimizedBackwardRun() {
    for (const auto &Sink : Sinks) {
        backwardSlicing(Sink.first);
    }
}

void DyckGlobalValueFlowAnalysis::comprehensiveBackwardRun() {
    for (const auto &Sink : Sinks) {
        comprehensiveBackwardSlicing(Sink.first, Sink.first);
    }
}

//===----------------------------------------------------------------------===//
// Slicing algorithms - converted to iterative worklist algorithms
//===----------------------------------------------------------------------===//

void DyckGlobalValueFlowAnalysis::forwardSlicing(const Value *V, int Mask) {
    std::queue<std::pair<const Value *, int>> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.emplace(V, Mask);
    
    while (!WorkQueue.empty()) {
        auto [CurrentValue, CurrentMask] = WorkQueue.front();
        WorkQueue.pop();
        
        // Skip if already processed with this mask
        if (Visited.count(CurrentValue)) {
            continue;
        }
        Visited.insert(CurrentValue);
        
        // Update reachability map
        ReachabilityMap[CurrentValue] |= CurrentMask;
        
        // Process call sites and returns
        if (auto *CI = dyn_cast<CallInst>(CurrentValue)) {
            processCallSite(CI, CurrentValue, CurrentMask, WorkQueue);
        } else if (auto *RI = dyn_cast<ReturnInst>(CurrentValue)) {
            processReturnSite(RI, CurrentValue, CurrentMask, WorkQueue);
        }
        
        // Follow VFG edges
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (int UncoveredMask = count(Succ, CurrentMask)) {
                    if (!Visited.count(Succ)) {
                        WorkQueue.emplace(Succ, UncoveredMask);
                    }
                }
            }
        }
    }
}

void DyckGlobalValueFlowAnalysis::backwardSlicing(const Value *V) {
    std::queue<const Value *> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (Visited.count(CurrentValue)) {
            continue;
        }
        Visited.insert(CurrentValue);
        
        // Update backward reachability map
        if (BackwardReachabilityMap.count(CurrentValue)) {
            BackwardReachabilityMap[CurrentValue] += 1;
        } else {
            BackwardReachabilityMap[CurrentValue] = 1;
        }
        
        // Follow VFG edges backwards
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (!backwardCount(Pred)) {
                    if (!Visited.count(Pred)) {
                        WorkQueue.push(Pred);
                    }
                }
            }
        }
    }
}

void DyckGlobalValueFlowAnalysis::comprehensiveForwardSlicing(const Value *V, const Value *Src) {
    std::queue<const Value *> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (Visited.count(CurrentValue)) {
            continue;
        }
        Visited.insert(CurrentValue);
        
        // Update comprehensive reachability map
        auto It = AllReachabilityMap.find(CurrentValue);
        if (It != AllReachabilityMap.end()) {
            It->second.insert(Src);
        } else {
            AllReachabilityMap.insert(std::make_pair(CurrentValue, std::unordered_set<const Value *>()));
            AllReachabilityMap[CurrentValue].insert(Src);
        }
        
        // Follow VFG edges
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (!allCount(Succ, Src)) {
                    if (!Visited.count(Succ)) {
                        WorkQueue.push(Succ);
                    }
                }
            }
        }
    }
}

void DyckGlobalValueFlowAnalysis::comprehensiveBackwardSlicing(const Value *V, const Value *Sink) {
    std::queue<const Value *> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (Visited.count(CurrentValue)) {
            continue;
        }
        Visited.insert(CurrentValue);
        
        // Update comprehensive backward reachability map
        auto It = AllBackwardReachabilityMap.find(CurrentValue);
        if (It != AllBackwardReachabilityMap.end()) {
            It->second.insert(Sink);
        } else {
            AllBackwardReachabilityMap.insert(std::make_pair(CurrentValue, std::unordered_set<const Value *>()));
            AllBackwardReachabilityMap[CurrentValue].insert(Sink);
        }
        
        // Follow VFG edges backwards
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (!allBackwardCount(Pred, Sink)) {
                    if (!Visited.count(Pred)) {
                        WorkQueue.push(Pred);
                    }
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Call site and return site processing - updated for iterative approach
//===----------------------------------------------------------------------===//

void DyckGlobalValueFlowAnalysis::processCallSite(const CallInst *CI, const Value *V, int Mask, 
                                                  std::queue<std::pair<const Value *, int>> &WorkQueue) {
    // Handle interprocedural flow through call sites
    if (auto *F = CI->getCalledFunction()) {
        // Map arguments to parameters
        for (unsigned i = 0; i < CI->arg_size() && i < F->arg_size(); ++i) {
            auto *Arg = CI->getArgOperand(i);
            auto *Param = F->getArg(i);
            if (Arg == V) {
                if (int UncoveredMask = count(Param, Mask)) {
                    WorkQueue.emplace(Param, UncoveredMask);
                }
            }
        }
    }
}

void DyckGlobalValueFlowAnalysis::processReturnSite(const ReturnInst *RI, const Value *V, int Mask,
                                                    std::queue<std::pair<const Value *, int>> &WorkQueue) {
    // Handle interprocedural flow through return sites
    const Function *F = RI->getFunction();
    
    // Find all call sites that call this function
    for (auto *User : F->users()) {
        if (auto *CI = dyn_cast<CallInst>(User)) {
            if (CI->getCalledFunction() == F && RI->getReturnValue() == V) {
                if (int UncoveredMask = count(CI, Mask)) {
                    WorkQueue.emplace(CI, UncoveredMask);
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Online slicing for queries - optimized with iterative approach
//===----------------------------------------------------------------------===//

bool DyckGlobalValueFlowAnalysis::onlineSlicing(const Value *Target) {
    for (const auto &Sink : Sinks) {
        std::unordered_set<const Value *> visited;
        if (onlineBackwardSlicing(Sink.first, Target, visited)) {
            return true;
        }
    }
    return false;
}

bool DyckGlobalValueFlowAnalysis::onlineForwardSlicing(const Value *V, 
                                                      std::unordered_set<const Value *> &visited) {
    std::queue<const Value *> WorkQueue;
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (visited.count(CurrentValue)) {
            continue;
        }
        visited.insert(CurrentValue);
        
        if (Sinks.count(CurrentValue)) {
            return true;
        }
        
        // Follow VFG edges
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (!visited.count(Succ)) {
                    WorkQueue.push(Succ);
                }
            }
        }
    }
    
    return false;
}

bool DyckGlobalValueFlowAnalysis::onlineBackwardSlicing(const Value *V, const Value *Target,
                                                       std::unordered_set<const Value *> &visited) {
    std::queue<const Value *> WorkQueue;
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (visited.count(CurrentValue)) {
            continue;
        }
        visited.insert(CurrentValue);
        
        if (CurrentValue == Target) {
            return true;
        }
        
        // Follow VFG edges backwards
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (!visited.count(Pred)) {
                    WorkQueue.push(Pred);
                }
            }
        }
    }
    
    return false;
}

//===----------------------------------------------------------------------===//
// Vulnerability Checker Implementations
//===----------------------------------------------------------------------===//

void NullPointerVulnerabilityChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find null pointer sources (e.g., null constants, failed allocations)
    for (auto &F : *M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                // Null constants
                if (auto *CI = dyn_cast<ConstantPointerNull>(&I)) {
                    Sources[{CI, 1}] = 1;
                }
                // Failed malloc/calloc
                if (auto *Call = dyn_cast<CallInst>(&I)) {
                    if (auto *CalledF = Call->getCalledFunction()) {
                        if (CalledF->getName() == "malloc" || CalledF->getName() == "calloc") {
                            Sources[{Call, 1}] = 1;
                        }
                    }
                }
            }
        }
    }
}

void NullPointerVulnerabilityChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    // Find null pointer sinks (e.g., dereferences, array accesses)
    for (auto &F : *M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                // Load instructions (dereferences)
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    auto *PtrOp = LI->getPointerOperand();
                    Sinks[PtrOp] = new std::set<const Value *>();
                    Sinks[PtrOp]->insert(LI);
                }
                // Store instructions
                if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    auto *PtrOp = SI->getPointerOperand();
                    Sinks[PtrOp] = new std::set<const Value *>();
                    Sinks[PtrOp]->insert(SI);
                }
                // GEP instructions (array accesses)
                if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
                    auto *PtrOp = GEP->getPointerOperand();
                    Sinks[PtrOp] = new std::set<const Value *>();
                    Sinks[PtrOp]->insert(GEP);
                }
            }
        }
    }
}

bool NullPointerVulnerabilityChecker::isValidTransfer(const Value *From, const Value *To) const {
    // Allow flow through most instructions except sanitizers
    if (auto *CI = dyn_cast<CallInst>(To)) {
        if (auto *F = CI->getCalledFunction()) {
            // Block flow through null check functions
            if (F->getName().contains("check") || F->getName().contains("assert")) {
                return false;
            }
        }
    }
    return true;
}

void TaintVulnerabilityChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find taint sources (e.g., user input functions)
    for (auto &F : *M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *Call = dyn_cast<CallInst>(&I)) {
                    if (auto *CalledF = Call->getCalledFunction()) {
                        std::string FuncName = CalledF->getName().str();
                        if (taint::TaintUtils::isKnownSourceFunction(FuncName)) {
                            Sources[{Call, 1}] = 1;
                        }
                    }
                }
            }
        }
    }
}

void TaintVulnerabilityChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    // Find taint sinks (e.g., dangerous functions)
    for (auto &F : *M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *Call = dyn_cast<CallInst>(&I)) {
                    if (auto *CalledF = Call->getCalledFunction()) {
                        std::string FuncName = CalledF->getName().str();
                        if (taint::TaintUtils::isKnownSinkFunction(FuncName)) {
                            for (unsigned i = 0; i < Call->arg_size(); ++i) {
                                auto *Arg = Call->getArgOperand(i);
                                Sinks[Arg] = new std::set<const Value *>();
                                Sinks[Arg]->insert(Call);
                            }
                        }
                    }
                }
            }
        }
    }
}

bool TaintVulnerabilityChecker::isValidTransfer(const Value *From, const Value *To) const {
    // Block flow through sanitizer functions
    if (auto *CI = dyn_cast<CallInst>(To)) {
        if (auto *F = CI->getCalledFunction()) {
            std::string FuncName = F->getName().str();
            if (taint::TaintUtils::isKnownSanitizerFunction(FuncName)) {
                return false;
            }
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// CFL Reachability Implementation (Lightweight) - optimized with iterative approach
//===----------------------------------------------------------------------===//

void DyckGlobalValueFlowAnalysis::initializeCFLAnalyzer() {
    // Lightweight CFL analyzer doesn't need separate initialization
    // It works directly with the VFG's existing label structure
}

bool DyckGlobalValueFlowAnalysis::cflReachable(const Value *From, const Value *To) const {
    // Use lightweight CFL reachability with matched call/return pairs
    return cflReachabilityQuery(From, To, true);
}

bool DyckGlobalValueFlowAnalysis::cflBackwardReachable(const Value *From, const Value *To) const {
    // Use lightweight CFL reachability with matched call/return pairs
    return cflReachabilityQuery(To, From, false);
}

bool DyckGlobalValueFlowAnalysis::performCFLReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    return cflReachabilityQuery(From, To, Forward);
}

bool DyckGlobalValueFlowAnalysis::cflReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    // Lightweight CFL reachability using a stack to match call/return pairs
    // This respects the context-free nature of function calls
    
    std::unordered_set<const Value *> visited;
    std::queue<std::pair<const Value *, std::vector<int>>> workQueue;
    
    // Start with empty call stack
    workQueue.push(std::make_pair(From, std::vector<int>()));
    
    while (!workQueue.empty()) {
        auto [current, callStack] = workQueue.front();
        workQueue.pop();
        
        if (current == To) {
            return true;
        }
        
        if (visited.count(current)) {
            continue;
        }
        visited.insert(current);
        
        // Get VFG node for current value
        auto *currentNode = VFG->getVFGNode(const_cast<Value *>(current));
        if (!currentNode) {
            continue;
        }
        
        // Explore edges based on direction
        if (Forward) {
            // Forward traversal: follow outgoing edges
            for (auto edgeIt = currentNode->begin(); edgeIt != currentNode->end(); ++edgeIt) {
                auto *nextNode = edgeIt->first;
                auto *nextValue = nextNode->getValue();
                int label = edgeIt->second;
                
                std::vector<int> newCallStack = callStack;
                
                if (label > 0) {
                    // Call edge: push call site ID onto stack
                    newCallStack.push_back(label);
                } else if (label < 0) {
                    // Return edge: check if it matches top of stack
                    if (newCallStack.empty() || newCallStack.back() != -label) {
                        // Unmatched return - skip this path
                        continue;
                    }
                    // Matched return: pop from stack
                    newCallStack.pop_back();
                }
                // label == 0: epsilon edge, no change to call stack
                
                if (!visited.count(nextValue)) {
                    workQueue.push(std::make_pair(nextValue, std::move(newCallStack)));
                }
            }
        } else {
            // Backward traversal: follow incoming edges
            for (auto edgeIt = currentNode->in_begin(); edgeIt != currentNode->in_end(); ++edgeIt) {
                auto *prevNode = edgeIt->first;
                auto *prevValue = prevNode->getValue();
                int label = edgeIt->second;
                
                std::vector<int> newCallStack = callStack;
                
                if (label < 0) {
                    // Return edge in backward direction: push call site ID onto stack
                    newCallStack.push_back(-label);
                } else if (label > 0) {
                    // Call edge in backward direction: check if it matches top of stack
                    if (newCallStack.empty() || newCallStack.back() != label) {
                        // Unmatched call - skip this path
                        continue;
                    }
                    // Matched call: pop from stack
                    newCallStack.pop_back();
                }
                // label == 0: epsilon edge, no change to call stack
                
                if (!visited.count(prevValue)) {
                    workQueue.push(std::make_pair(prevValue, std::move(newCallStack)));
                }
            }
        }
    }
    
    return false;
}

int DyckGlobalValueFlowAnalysis::getValueNodeID(const Value *V) const {
    // For lightweight implementation, we don't need separate node IDs
    // Just use the value pointer as identifier
    return reinterpret_cast<intptr_t>(V);
}

//===----------------------------------------------------------------------===//
// Enhanced reachability queries with CFL support
//===----------------------------------------------------------------------===//

bool DyckGlobalValueFlowAnalysis::contextSensitiveReachable(const Value *From, const Value *To) const {
    return cflReachable(From, To);
}

bool DyckGlobalValueFlowAnalysis::contextSensitiveBackwardReachable(const Value *From, const Value *To) const {
    return cflBackwardReachable(From, To);
}