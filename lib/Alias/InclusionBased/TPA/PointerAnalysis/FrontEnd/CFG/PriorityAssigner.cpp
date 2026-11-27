// Implementation of PriorityAssigner.
//
// Assigns topological priorities to CFG nodes.
//
// Purpose:
// The worklist algorithm (in the Engine) uses priorities to determine the order
// of node processing. Processing nodes in Reverse Post-Order (RPO)
// significantly speeds up convergence for forward data-flow analysis: in RPO, a
// node is always processed before its successors (in the acyclic case), so
// information flows forward in a single pass rather than requiring many
// iterations.
//
// Assigns post-order priorities (1 = exit/leaf, N = entry) via DFS rooted at
// the CFG entry node. Because ForwardWorkList is a max-heap (`lhs < rhs`),
// popping highest-priority first visits nodes in Reverse Post-Order
// (entry -> exit), and BackwardWorkList (min-heap, `lhs > rhs`) visits nodes
// in reverse-CFG RPO (exit -> entry).

#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/CFG/PriorityAssigner.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFG.h"

#include <vector>

namespace tpa {

void PriorityAssigner::traverse() {
  // Step 1: Collect nodes in post-order via DFS, rooted at the function's
  // entry node so reachable nodes follow true CFG topology.
  std::vector<tpa::CFGNode *> postOrder;
  postOrder.reserve(64);

  if (auto *entry = cfg.getEntryNode())
    visitNode(entry, postOrder);

  for (auto *node : cfg) {
    if (node->getPriority() == 0u)
      visitNode(node, postOrder);
  }

  // Step 2: Assign post-order labels (1 for deepest exit/leaf, highest for
  // entry). ForwardWorkList uses a std::priority_queue max-heap with
  // PriorityComparator (lhs < rhs), so popping highest-priority first visits
  // nodes in Reverse Post-Order (entry -> exit), and BackwardWorkList (min-heap
  // with lhs > rhs) visits nodes in reverse-CFG RPO (exit -> entry).
  unsigned label = 1;
  for (auto *node : postOrder) {
    node->setPriority(label);
    ++label;
  }
}

void PriorityAssigner::visitNode(tpa::CFGNode *node,
                                 std::vector<tpa::CFGNode *> &postOrder) {
  if (!visitedNodes.insert(node).second)
    return;

  // Visit all successors first (DFS).
  for (auto const &succ : node->succs())
    visitNode(succ, postOrder);

  // Record in post-order on the way back up.
  postOrder.push_back(node);
}

} // namespace tpa
