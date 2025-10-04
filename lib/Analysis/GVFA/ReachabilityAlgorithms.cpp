/**
 * @file ReachabilityAlgorithms.cpp
 * @brief Reachability algorithms for Global Value Flow Analysis
 *
 * Iterative work queue implementations for forward/backward reachability,
 * detailed analysis, and online queries.
 */

#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <unordered_map>

#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/GVFA/ReachabilityAlgorithms.h"

using namespace llvm;

#define DEBUG_TYPE "dyck-gvfa"

//===----------------------------------------------------------------------===//
// Reachability algorithms
//===----------------------------------------------------------------------===//

// Forward reachability with bit mask tracking

void DyckGlobalValueFlowAnalysis::forwardReachability(const Value *V, int Mask) {
    std::queue<std::pair<const Value *, int>> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.emplace(V, Mask);
    
    while (!WorkQueue.empty()) {
        auto front = WorkQueue.front();
        const Value *CurrentValue = front.first;
        int CurrentMask = front.second;
        WorkQueue.pop();
        
        if (!Visited.insert(CurrentValue).second) continue;
        
        ReachabilityMap[CurrentValue] |= CurrentMask;
        
        if (auto *CI = dyn_cast<CallInst>(CurrentValue)) {
            processCallSite(CI, CurrentValue, CurrentMask, WorkQueue);
        } else if (auto *RI = dyn_cast<ReturnInst>(CurrentValue)) {
            processReturnSite(RI, CurrentValue, CurrentMask, WorkQueue);
        }
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (int UncoveredMask = count(Succ, CurrentMask)) {
                    if (Visited.find(Succ) == Visited.end()) {
                        WorkQueue.emplace(Succ, UncoveredMask);
                    }
                }
            }
        }
    }
}

// Backward reachability from sink

void DyckGlobalValueFlowAnalysis::backwardReachability(const Value *V) {
    std::queue<const Value *> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!Visited.insert(CurrentValue).second) continue;
        
        BackwardReachabilityMap[CurrentValue] += 1;
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (!backwardCount(Pred) && Visited.find(Pred) == Visited.end()) {
                    WorkQueue.push(Pred);
                }
            }
        }
    }
}

// Detailed forward reachability with all-pairs tracking

void DyckGlobalValueFlowAnalysis::detailedForwardReachability(const Value *V, const Value *Src) {
    std::queue<const Value *> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!Visited.insert(CurrentValue).second) continue;
        
        AllReachabilityMap[CurrentValue].insert(Src);
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (!allCount(Succ, Src) && Visited.find(Succ) == Visited.end()) {
                    WorkQueue.push(Succ);
                }
            }
        }
    }
}

// Detailed backward reachability with all-pairs tracking

void DyckGlobalValueFlowAnalysis::detailedBackwardReachability(const Value *V, const Value *Sink) {
    std::queue<const Value *> WorkQueue;
    std::unordered_set<const Value *> Visited;
    
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!Visited.insert(CurrentValue).second) continue;
        
        AllBackwardReachabilityMap[CurrentValue].insert(Sink);
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (!allBackwardCount(Pred, Sink) && Visited.find(Pred) == Visited.end()) {
                    WorkQueue.push(Pred);
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Call/return processing
//===----------------------------------------------------------------------===//

// Process call sites for value propagation

void DyckGlobalValueFlowAnalysis::processCallSite(const CallInst *CI, const Value *V, int Mask, 
                                                  std::queue<std::pair<const Value *, int>> &WorkQueue) {
    if (auto *F = CI->getCalledFunction()) {
        for (unsigned i = 0; i < CI->arg_size() && i < F->arg_size(); ++i) {
            if (CI->getArgOperand(i) == V) {
                if (int UncoveredMask = count(F->getArg(i), Mask)) {
                    WorkQueue.emplace(F->getArg(i), UncoveredMask);
                }
            }
        }
    }
}

// Process return sites for value propagation

void DyckGlobalValueFlowAnalysis::processReturnSite(const ReturnInst *RI, const Value *V, int Mask,
                                                    std::queue<std::pair<const Value *, int>> &WorkQueue) {
    const Function *F = RI->getFunction();
    
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
// Online queries
//===----------------------------------------------------------------------===//

// Online reachability check

bool DyckGlobalValueFlowAnalysis::onlineReachability(const Value *Target) {
    for (const auto &Sink : Sinks) {
        std::unordered_set<const Value *> visited;
        if (onlineBackwardReachability(Sink.first, Target, visited)) return true;
    }
    return false;
}

// Online forward reachability

bool DyckGlobalValueFlowAnalysis::onlineForwardReachability(const Value *V, 
                                                      std::unordered_set<const Value *> &visited) {
    std::queue<const Value *> WorkQueue;
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!visited.insert(CurrentValue).second) continue;
        
        if (Sinks.count(CurrentValue)) return true;
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (visited.find(Succ) == visited.end()) {
                    WorkQueue.push(Succ);
                }
            }
        }
    }
    
    return false;
}

// Online backward reachability

bool DyckGlobalValueFlowAnalysis::onlineBackwardReachability(const Value *V, const Value *Target,
                                                       std::unordered_set<const Value *> &visited) {
    std::queue<const Value *> WorkQueue;
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!visited.insert(CurrentValue).second) continue;
        
        if (CurrentValue == Target) return true;
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (visited.find(Pred) == visited.end()) {
                    WorkQueue.push(Pred);
                }
            }
        }
    }
    
    return false;
}

//===----------------------------------------------------------------------===//
// CFL Reachability
//===----------------------------------------------------------------------===//

// Initialize CFL analyzer

void DyckGlobalValueFlowAnalysis::initializeCFLAnalyzer() {
    // Lightweight CFL analyzer works directly with VFG's existing label structure
}

// CFL forward reachability

bool DyckGlobalValueFlowAnalysis::cflReachable(const Value *From, const Value *To) const {
    return cflReachabilityQuery(From, To, true);
}

// CFL backward reachability

bool DyckGlobalValueFlowAnalysis::cflBackwardReachable(const Value *From, const Value *To) const {
    return cflReachabilityQuery(To, From, false);
}

// CFL reachability query

bool DyckGlobalValueFlowAnalysis::performCFLReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    return cflReachabilityQuery(From, To, Forward);
}

// Core CFL reachability with call stack tracking

bool DyckGlobalValueFlowAnalysis::cflReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    std::unordered_set<const Value *> visited;
    std::queue<std::pair<const Value *, std::vector<int>>> workQueue;
    
    workQueue.push(std::make_pair(From, std::vector<int>()));
    
    while (!workQueue.empty()) {
        auto front = workQueue.front();
        const Value *current = front.first;
        std::vector<int> callStack = front.second;
        workQueue.pop();
        
        if (current == To) return true;
        if (!visited.insert(current).second) continue;
        
        auto *currentNode = VFG->getVFGNode(const_cast<Value *>(current));
        if (!currentNode) continue;
        
        if (Forward) {
            for (auto edgeIt = currentNode->begin(); edgeIt != currentNode->end(); ++edgeIt) {
                auto *nextValue = edgeIt->first->getValue();
                int label = edgeIt->second;
                
                std::vector<int> newCallStack = callStack;
                
                if (label > 0) {
                    newCallStack.push_back(label);
                } else if (label < 0) {
                    if (newCallStack.empty() || newCallStack.back() != -label) continue;
                    newCallStack.pop_back();
                }
                
                if (visited.find(nextValue) == visited.end()) {
                    workQueue.push(std::make_pair(nextValue, std::move(newCallStack)));
                }
            }
        } else {
            for (auto edgeIt = currentNode->in_begin(); edgeIt != currentNode->in_end(); ++edgeIt) {
                auto *prevValue = edgeIt->first->getValue();
                int label = edgeIt->second;
                
                std::vector<int> newCallStack = callStack;
                
                if (label < 0) {
                    newCallStack.push_back(-label);
                } else if (label > 0) {
                    if (newCallStack.empty() || newCallStack.back() != label) continue;
                    newCallStack.pop_back();
                }
                
                if (visited.find(prevValue) == visited.end()) {
                    workQueue.push(std::make_pair(prevValue, std::move(newCallStack)));
                }
            }
        }
    }
    
    return false;
}

// Get unique value node ID

int DyckGlobalValueFlowAnalysis::getValueNodeID(const Value *V) const {
    return reinterpret_cast<intptr_t>(V);
}

//===----------------------------------------------------------------------===//
// Context-sensitive queries
//===----------------------------------------------------------------------===//

// Context-sensitive reachability

bool DyckGlobalValueFlowAnalysis::contextSensitiveReachable(const Value *From, const Value *To) const {
    return cflReachable(From, To);
}

// Context-sensitive backward reachability

bool DyckGlobalValueFlowAnalysis::contextSensitiveBackwardReachable(const Value *From, const Value *To) const {
    return cflBackwardReachable(From, To);
}
