#ifndef ANALYSIS_GVFA_REACHABILITYALGORITHMS_H
#define ANALYSIS_GVFA_REACHABILITYALGORITHMS_H

#include <llvm/IR/Value.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

// Forward declaration
class DyckGlobalValueFlowAnalysis;

/**
 * Reachability Algorithms for Global Value Flow Analysis
 * 
 * Iterative work queue implementations for reachability analysis.
 * These are private methods of DyckGlobalValueFlowAnalysis.
 */

// Private methods of DyckGlobalValueFlowAnalysis:
// - forwardReachability, backwardReachability
// - detailedForwardReachability, detailedBackwardReachability  
// - onlineReachability, onlineForwardReachability, onlineBackwardReachability
// - processCallSite, processReturnSite
// - getSuccessors, getPredecessors, isValueFlowEdge

#endif // ANALYSIS_GVFA_REACHABILITYALGORITHMS_H
