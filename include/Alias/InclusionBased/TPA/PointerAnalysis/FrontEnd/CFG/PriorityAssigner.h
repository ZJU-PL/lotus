#pragma once

#include <cstddef>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>

namespace tpa {

class CFG;
class CFGNode;

// PriorityAssigner assigns post-order labels (1 = exit/leaf, N = entry) via
// DFS rooted at the CFG entry node. Because ForwardWorkList is a max-heap
// (`lhs < rhs`), popping highest-priority first visits nodes in forward
// Reverse Post-Order (entry -> exit), and BackwardWorkList (min-heap) visits
// nodes in reverse-CFG RPO (exit -> entry).
class PriorityAssigner {
private:
  CFG &cfg;
  size_t currLabel; // kept for ABI compatibility; unused by new implementation

  using NodeSet = llvm::SmallPtrSet<const CFGNode *, 32>;
  NodeSet visitedNodes;

  // DFS helper: visits node and all successors, appending to postOrder on
  // the way back up (post-order traversal).
  void visitNode(CFGNode *, std::vector<CFGNode *> &postOrder);

public:
  PriorityAssigner(CFG &c) : cfg(c), currLabel(1) {}

  void traverse();
};

} // namespace tpa
