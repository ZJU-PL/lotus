/*
 * LotusAA - Call Graph State Implementation
 */

#include "Alias/LotusAA/Support/CallGraphState.h"

using namespace llvm;

void CallGraphState::clear() {
  topDown_.clear();
  bottomUp_.clear();
  backEdges_.clear();
}

FunctionSet &CallGraphState::getCallees(Function *func) {
  return topDown_[func];
}

const FunctionSet &CallGraphState::getCallees(Function *func) const {
  static const FunctionSet emptySet;
  auto it = topDown_.find(func);
  return (it != topDown_.end()) ? it->second : emptySet;
}

FunctionSet &CallGraphState::getCallers(Function *func) {
  return bottomUp_[func];
}

const FunctionSet &CallGraphState::getCallers(Function *func) const {
  static const FunctionSet emptySet;
  auto it = bottomUp_.find(func);
  return (it != bottomUp_.end()) ? it->second : emptySet;
}

void CallGraphState::addEdge(Function *caller, Function *callee) {
  if (caller && callee && !isBackEdge(caller, callee)) {
    topDown_[caller].insert(callee);
    bottomUp_[callee].insert(caller);
  }
}

bool CallGraphState::isBackEdge(Function *caller, Function *callee) const {
  auto it = backEdges_.find(caller);
  return (it != backEdges_.end()) && it->second.count(callee);
}

void CallGraphState::markBackEdge(Function *caller, Function *callee) {
  backEdges_[caller].insert(callee);
}

void CallGraphState::initializeForFunctions(const std::vector<Function *> &functions) {
  for (Function *F : functions) {
    topDown_[F];   // Create empty entry
    bottomUp_[F];  // Create empty entry
  }
}

void CallGraphState::detectBackEdges(std::set<Function *> &changedFuncs) {
  std::set<Function *, llvm_cmp> notVisited, visiting;

  // Initialize with all functions
  for (auto &item : topDown_) {
    notVisited.insert(item.first);
  }

  // DFS from each unvisited function
  while (!notVisited.empty()) {
    Function *func = *notVisited.begin();
    visiting.clear();
    detectBackEdgesRecursive(notVisited, visiting, func, changedFuncs);
  }
}

void CallGraphState::detectBackEdgesRecursive(
    std::set<Function *, llvm_cmp> &notVisited,
    std::set<Function *, llvm_cmp> &visiting,
    Function *currentFunc,
    std::set<Function *> &changedFuncs) {
  
  notVisited.erase(currentFunc);
  visiting.insert(currentFunc);

  // Visit all callees
  if (topDown_.count(currentFunc)) {
    for (Function *child : topDown_[currentFunc]) {
      if (notVisited.count(child)) {
        // Forward edge: recurse
        detectBackEdgesRecursive(notVisited, visiting, child, changedFuncs);
      } else if (visiting.count(child)) {
        // Back edge found: child is already on the DFS path
        backEdges_[currentFunc].insert(child);
        changedFuncs.insert(currentFunc);
      }
    }
  }

  visiting.erase(currentFunc);
}

