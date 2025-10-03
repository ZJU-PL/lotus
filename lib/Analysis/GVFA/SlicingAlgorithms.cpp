/**
 * @file SlicingAlgorithms.cpp
 * @brief Implementation of slicing algorithms for Global Value Flow Analysis
 *
 * This file contains the core slicing algorithms used in the global value flow
 * analysis. All algorithms have been converted from recursive to iterative
 * implementations using work queues for better performance and stack safety.
 * Includes forward/backward slicing, comprehensive analysis, and online queries.
 */

#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <unordered_map>

#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"

using namespace llvm;

#define DEBUG_TYPE "dyck-gvfa"

//===----------------------------------------------------------------------===//
// Slicing algorithms - converted to iterative worklist algorithms
//===----------------------------------------------------------------------===//

/**
 * Performs forward slicing from a source value with a given mask.
 *
 * This function uses an iterative work queue approach to traverse the Value
 * Flow Graph forward from the source, tracking reachability with bit masks.
 * It handles call sites and return sites specially for context sensitivity.
 *
 * @param V The source value to start slicing from
 * @param Mask The bit mask representing source identifiers
 */
void DyckGlobalValueFlowAnalysis::forwardSlicing(const Value *V, int Mask) {
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

/**
 * Performs backward slicing from a sink value.
 *
 * This function traverses the Value Flow Graph backward from a sink to
 * identify all values that can reach the sink. It uses an iterative
 * work queue approach for better performance.
 *
 * @param V The sink value to start backward slicing from
 */
void DyckGlobalValueFlowAnalysis::backwardSlicing(const Value *V) {
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

/**
 * Performs comprehensive forward slicing maintaining detailed source mappings.
 *
 * This function is similar to forwardSlicing but maintains detailed
 * all-pairs reachability information, tracking which specific source
 * reaches each value.
 *
 * @param V The source value to start slicing from
 * @param Src The source identifier for detailed tracking
 */
void DyckGlobalValueFlowAnalysis::comprehensiveForwardSlicing(const Value *V, const Value *Src) {
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

/**
 * Performs comprehensive backward slicing maintaining detailed sink mappings.
 *
 * This function is similar to backwardSlicing but maintains detailed
 * all-pairs reachability information, tracking which specific sink
 * can be reached from each value.
 *
 * @param V The sink value to start backward slicing from
 * @param Sink The sink identifier for detailed tracking
 */
void DyckGlobalValueFlowAnalysis::comprehensiveBackwardSlicing(const Value *V, const Value *Sink) {
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
// Call site and return site processing - updated for iterative approach
//===----------------------------------------------------------------------===//

/**
 * Processes call sites to propagate values across function boundaries.
 *
 * This function handles the propagation of values from call instructions
 * to the corresponding formal parameters in the called function.
 *
 * @param CI The call instruction being processed
 * @param V The value being propagated
 * @param Mask The bit mask for the value
 * @param WorkQueue The work queue to add new work items to
 */
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

/**
 * Processes return sites to propagate values back to callers.
 *
 * This function handles the propagation of return values from return
 * instructions back to the corresponding call instructions.
 *
 * @param RI The return instruction being processed
 * @param V The return value being propagated
 * @param Mask The bit mask for the value
 * @param WorkQueue The work queue to add new work items to
 */
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
// Online slicing for queries - optimized with iterative approach
//===----------------------------------------------------------------------===//

/**
 * Performs online slicing to check if a target can reach any sink.
 *
 * This function is used for real-time queries when online query mode is enabled.
 * It performs backward slicing from all sinks to check if the target is reachable.
 *
 * @param Target The value to check reachability for
 * @return true if the target can reach any sink
 */
bool DyckGlobalValueFlowAnalysis::onlineSlicing(const Value *Target) {
    for (const auto &Sink : Sinks) {
        std::unordered_set<const Value *> visited;
        if (onlineBackwardSlicing(Sink.first, Target, visited)) return true;
    }
    return false;
}

/**
 * Performs online forward slicing to check reachability to sinks.
 *
 * This function performs forward traversal from a value to check if
 * any sink can be reached. It maintains a visited set to avoid cycles.
 *
 * @param V The value to start forward slicing from
 * @param visited The set of already visited values
 * @return true if any sink is reachable from the value
 */
bool DyckGlobalValueFlowAnalysis::onlineForwardSlicing(const Value *V, 
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

/**
 * Performs online backward slicing to check reachability from a target.
 *
 * This function performs backward traversal from a value to check if
 * a specific target can be reached. It maintains a visited set to avoid cycles.
 *
 * @param V The value to start backward slicing from
 * @param Target The target value to search for
 * @param visited The set of already visited values
 * @return true if the target is reachable from the value
 */
bool DyckGlobalValueFlowAnalysis::onlineBackwardSlicing(const Value *V, const Value *Target,
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
// CFL Reachability Implementation (Lightweight) - optimized with iterative approach
//===----------------------------------------------------------------------===//

/**
 * Initializes the CFL (Context-Free Language) analyzer.
 *
 * This is a lightweight implementation that works directly with the VFG's
 * existing label structure rather than building a separate CFL graph.
 */
void DyckGlobalValueFlowAnalysis::initializeCFLAnalyzer() {
    // Lightweight CFL analyzer works directly with VFG's existing label structure
}

/**
 * Checks CFL reachability between two values in the forward direction.
 *
 * CFL reachability provides context-sensitive analysis by tracking
 * call and return edges with proper stack discipline.
 *
 * @param From The source value
 * @param To The target value
 * @return true if To is CFL reachable from From
 */
bool DyckGlobalValueFlowAnalysis::cflReachable(const Value *From, const Value *To) const {
    return cflReachabilityQuery(From, To, true);
}

/**
 * Checks CFL reachability between two values in the backward direction.
 *
 * @param From The source value
 * @param To The target value
 * @return true if From is CFL reachable from To (backward)
 */
bool DyckGlobalValueFlowAnalysis::cflBackwardReachable(const Value *From, const Value *To) const {
    return cflReachabilityQuery(To, From, false);
}

/**
 * Performs a CFL reachability query in the specified direction.
 *
 * @param From The source value
 * @param To The target value
 * @param Forward true for forward direction, false for backward
 * @return true if reachable in the specified direction
 */
bool DyckGlobalValueFlowAnalysis::performCFLReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    return cflReachabilityQuery(From, To, Forward);
}

/**
 * Core CFL reachability query implementation.
 *
 * This function implements context-sensitive reachability by maintaining
 * a call stack during traversal. Positive labels represent call edges,
 * negative labels represent return edges. The stack ensures proper
 * matching of calls and returns.
 *
 * @param From The source value
 * @param To The target value
 * @param Forward true for forward direction, false for backward
 * @return true if reachable with proper context sensitivity
 */
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

/**
 * Gets a unique ID for a value node.
 *
 * @param V The value to get an ID for
 * @return A unique integer ID for the value
 */
int DyckGlobalValueFlowAnalysis::getValueNodeID(const Value *V) const {
    return reinterpret_cast<intptr_t>(V);
}

//===----------------------------------------------------------------------===//
// Enhanced reachability queries with CFL support
//===----------------------------------------------------------------------===//

/**
 * Checks context-sensitive reachability using CFL analysis.
 *
 * @param From The source value
 * @param To The target value
 * @return true if To is context-sensitively reachable from From
 */
bool DyckGlobalValueFlowAnalysis::contextSensitiveReachable(const Value *From, const Value *To) const {
    return cflReachable(From, To);
}

/**
 * Checks context-sensitive backward reachability using CFL analysis.
 *
 * @param From The source value
 * @param To The target value
 * @return true if From is context-sensitively reachable from To (backward)
 */
bool DyckGlobalValueFlowAnalysis::contextSensitiveBackwardReachable(const Value *From, const Value *To) const {
    return cflBackwardReachable(From, To);
}
