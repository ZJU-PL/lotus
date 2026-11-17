#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFGNode.h"
#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/UniquePtrIterator.h"

#include <memory>
#include <vector>

#include <llvm/ADT/DenseMap.h>

namespace llvm {
class Function;
} // namespace llvm

namespace tpa {

// Control flow graph for pointer analysis
//
// Represents a function's control flow as a graph of CFGNodes.
// Each node represents a statement that affects pointer state.
//
// Node Types:
// - Entry: Function entry point
// - Alloc: Memory allocation
// - Copy: Pointer assignment
// - Offset: Address-of operation
// - Load: Pointer dereference
// - Store: Store through pointer
// - Call: Function call
// - Return: Function return
//
// Edges:
// - CFG edges (pred/succ): Control flow connections
// - Def-use edges (def/use): Data flow connections
class CFG {
private:
  // The LLVM function this CFG represents
  const llvm::Function &func;

  // All nodes in this CFG (owned)
  using NodeList = std::vector<std::unique_ptr<CFGNode>>;
  NodeList nodes;

  // Fast lookup from LLVM Value to CFGNode
  using ValueMap = llvm::DenseMap<const llvm::Value *, const CFGNode *>;
  ValueMap valueMap;

  // Special nodes
  EntryCFGNode *entryNode;
  const ReturnCFGNode *exitNode;
  std::vector<const ReturnCFGNode *> unwindExitNodes;

public:
  // Iterators over nodes
  using iterator = util::UniquePtrIterator<NodeList::iterator>;
  using const_iterator = util::UniquePtrConstIterator<NodeList::const_iterator>;

  // Constructor
  CFG(const llvm::Function &);

  // Access function
  const llvm::Function &getFunction() const { return func; }
  // Access entry node
  EntryCFGNode *getEntryNode() { return entryNode; }
  const EntryCFGNode *getEntryNode() const { return entryNode; }

  // Check if function has return
  bool doesNotReturn() const { return (exitNode == nullptr); }
  // Access exit node
  const ReturnCFGNode *getExitNode() const {
    assert(!doesNotReturn());
    return exitNode;
  }
  void setExitNode(const ReturnCFGNode *n) {
    assert(exitNode == nullptr && n != nullptr);
    exitNode = n;
  }

  // Exceptional exits carry memory back to callers, without a return value.
  void addUnwindExitNode(const ReturnCFGNode *node) {
    unwindExitNodes.push_back(node);
  }
  const auto &getUnwindExitNodes() const { return unwindExitNodes; }

  // CFG manipulation
  void removeNodes(const util::VectorSet<CFGNode *> &);
  // Build value-to-node mapping after construction
  void buildValueMap();

  // Get CFG node for an LLVM value
  const CFGNode *getCFGNodeForValue(const llvm::Value *val) const {
    auto itr = valueMap.find(val);
    if (itr != valueMap.end())
      return itr->second;
    else
      return nullptr;
  }

  // Node factory method
  template <typename Node, typename... Args> Node *create(Args &&...args) {
    // I can't use make_unique() here because the constructor for Node should be
    // private
    auto node = new Node(std::forward<Args>(args)...);
    node->setCFG(*this);
    nodes.emplace_back(node);
    return node;
  }

  // Iterate over all nodes
  iterator begin() { return iterator(nodes.begin()); }
  iterator end() { return iterator(nodes.end()); }
  const_iterator begin() const { return const_iterator(nodes.begin()); }
  const_iterator end() const { return const_iterator(nodes.end()); }
  size_t getNumNodes() const { return nodes.size(); }
};

} // namespace tpa