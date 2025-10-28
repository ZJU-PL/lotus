/*
 * LotusAA - Call Graph State Management
 * 
 * Encapsulates call graph relationships and back-edge detection
 * for inter-procedural analysis.
 */

#pragma once

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/Function.h>
#include "Alias/LotusAA/MemoryModel/Types.h"

namespace llvm {

/// Set of functions (with LLVM-based comparison)
using FunctionSet = std::set<Function *, llvm_cmp>;

/// Map from function to set of related functions
using FunctionRelationMap = std::map<Function *, FunctionSet, llvm_cmp>;

/**
 * CallGraphState - Manages call graph relationships
 * 
 * Tracks caller-callee relationships, back edges (cycles), and
 * provides utilities for topological ordering and cycle detection.
 */
class CallGraphState {
public:
  CallGraphState() = default;
  ~CallGraphState() = default;

  /// Clear all call graph state
  void clear();

  /// Get callees of a function (top-down view)
  FunctionSet &getCallees(Function *func);
  const FunctionSet &getCallees(Function *func) const;

  /// Get callers of a function (bottom-up view)
  FunctionSet &getCallers(Function *func);
  const FunctionSet &getCallers(Function *func) const;

  /// Add a call edge from caller to callee
  void addEdge(Function *caller, Function *callee);

  /// Check if edge from caller to callee is a back edge (cycle)
  bool isBackEdge(Function *caller, Function *callee) const;

  /// Mark edge as a back edge and track which functions changed
  void markBackEdge(Function *caller, Function *callee);

  /// Detect all back edges in the call graph using DFS
  void detectBackEdges(std::set<Function *> &changed_funcs);

  /// Initialize call graph mappings for all functions in module
  void initializeForFunctions(const std::vector<Function *> &functions);

  /// Access to underlying maps (for compatibility)
  FunctionRelationMap &getTopDownMap() { return topDown_; }
  FunctionRelationMap &getBottomUpMap() { return bottomUp_; }
  const FunctionRelationMap &getTopDownMap() const { return topDown_; }
  const FunctionRelationMap &getBottomUpMap() const { return bottomUp_; }

private:
  /// DFS helper for back edge detection
  void detectBackEdgesRecursive(
      std::set<Function *, llvm_cmp> &notVisited,
      std::set<Function *, llvm_cmp> &visiting,
      Function *currentFunc,
      std::set<Function *> &changedFuncs);

private:
  /// Caller -> Callees mapping (top-down traversal)
  FunctionRelationMap topDown_;

  /// Callee -> Callers mapping (bottom-up traversal)
  FunctionRelationMap bottomUp_;

  /// Back edges: caller -> callees that form cycles
  std::map<Function *, FunctionSet, llvm_cmp> backEdges_;
};

} // namespace llvm

