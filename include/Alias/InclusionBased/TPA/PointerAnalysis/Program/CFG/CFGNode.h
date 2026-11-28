#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/NodeMixin.h"
#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/IteratorRange.h"

#include <cstddef>
#include <cstdint>

namespace llvm {
class Function;
} // namespace llvm

namespace tpa {

class CFG;
class CFGNode;

struct CFGNodeIdLess {
  bool operator()(const CFGNode *lhs, const CFGNode *rhs) const noexcept;
};

// Base class for all CFG nodes
//
// Each CFG node represents a statement that may affect pointer analysis.
// Nodes are typed by their effect on pointer state.
//
// Node Types (CFGNodeTag):
// - Entry: Function entry point, initializes parameters
// - Alloc: Memory allocation (malloc, alloca, global)
// - Copy: Pointer assignment (p = q)
// - Offset: Address-of operation (p = &obj.field)
// - Load: Pointer dereference (p = *q)
// - Store: Store through pointer (*p = q)
// - Call: Function call (may transfer to other functions)
// - Ret: Function return
//
// Edges:
// - pred/succ: Control flow predecessors and successors
// - def/use: Top-level def-use chains for SSA-like analysis
class CFGNode {
private:
  // Node type tag
  CFGNodeTag tag;

  // The CFG this node belongs to
  const CFG *cfg;

  // Reverse postorder number (for scheduling/priority)
  size_t rpo;

  // Deterministic creation ID within owning CFG
  uint32_t id;

  // Node sets for edges (ordered deterministically by CFGNode creation ID)
  using NodeSet = util::SortedVector<CFGNode *, CFGNodeIdLess>;
  // Control flow edges
  NodeSet pred, succ;
  // Data flow edges (def-use at top level)
  NodeSet def, use;

protected:
  // Constructor (called by mixins)
  CFGNode(CFGNodeTag t) : tag(t), cfg(nullptr), rpo(0), id(0) {}

  // Set the CFG and deterministic node ID (called by CFG when creating nodes)
  void setCFG(const CFG &c) { cfg = &c; }
  void setId(uint32_t nodeId) { id = nodeId; }

public:
  using iterator = NodeSet::iterator;
  using const_iterator = NodeSet::const_iterator;

  uint32_t getId() const noexcept { return id; }

  // Non-copyable
  CFGNode(const CFGNode &) = delete;
  CFGNode(CFGNode &&) noexcept = default;
  CFGNode &operator=(const CFGNode &) = delete;
  CFGNode &operator=(CFGNode &&) = delete;

  // Node type queries
  CFGNodeTag getNodeTag() const { return tag; }
  bool isEntryNode() const { return tag == CFGNodeTag::Entry; }
  bool isAllocNode() const { return tag == CFGNodeTag::Alloc; }
  bool isCopyNode() const { return tag == CFGNodeTag::Copy; }
  bool isOffsetNode() const { return tag == CFGNodeTag::Offset; }
  bool isLoadNode() const { return tag == CFGNodeTag::Load; }
  bool isStoreNode() const { return tag == CFGNodeTag::Store; }
  bool isCallNode() const { return tag == CFGNodeTag::Call; }
  bool isReturnNode() const { return tag == CFGNodeTag::Ret; }

  // Access containing CFG
  const CFG &getCFG() const {
    assert(cfg != nullptr);
    return *cfg;
  }
  // Access containing function
  const llvm::Function &getFunction() const;

  // Priority (RPO number) for worklist scheduling
  size_t getPriority() const { return rpo; }
  void setPriority(size_t p) {
    assert(rpo == 0);
    rpo = p;
  }

  // Control flow predecessors
  const_iterator pred_begin() const { return pred.begin(); }
  const_iterator pred_end() const { return pred.end(); }
  auto preds() const { return util::iteratorRange(pred.begin(), pred.end()); }
  unsigned pred_size() const { return pred.size(); }

  // Control flow successors
  iterator succ_begin() { return succ.begin(); }
  iterator succ_end() { return succ.end(); }
  const_iterator succ_begin() const { return succ.begin(); }
  const_iterator succ_end() const { return succ.end(); }
  auto succs() const { return util::iteratorRange(succ.begin(), succ.end()); }
  unsigned succ_size() const { return succ.size(); }

  // Def-use edges (nodes that define values used here)
  const_iterator def_begin() const { return def.begin(); }
  const_iterator def_end() const { return def.end(); }
  auto defs() const { return util::iteratorRange(def.begin(), def.end()); }
  unsigned def_size() const { return def.size(); }

  // Def-use edges (nodes that use values defined here)
  const_iterator use_begin() const { return use.begin(); }
  const_iterator use_end() const { return use.end(); }
  auto uses() const { return util::iteratorRange(use.begin(), use.end()); }
  unsigned use_size() const { return use.size(); }

  // Edge existence checks
  bool hasSuccessor(const CFGNode *node) const {
    return succ.count(const_cast<CFGNode *>(node));
  }
  bool hasUse(const CFGNode *node) const {
    return use.count(const_cast<CFGNode *>(node));
  }

  // Edge manipulation
  void insertEdge(CFGNode *n);
  void removeEdge(CFGNode *n);
  void insertDefUseEdge(CFGNode *n);
  void removeDefUseEdge(CFGNode *n);
  void detachFromCFG();

  friend class CFG;
};

inline bool CFGNodeIdLess::operator()(const CFGNode *lhs,
                                      const CFGNode *rhs) const noexcept {
  const uint32_t lid = lhs->getId();
  const uint32_t rid = rhs->getId();
  if (lid != rid)
    return lid < rid;
  return lhs < rhs;
}

// Type aliases for specific node types
using EntryCFGNode = EntryNodeMixin<CFGNode>;
using AllocCFGNode = AllocNodeMixin<CFGNode>;
using CopyCFGNode = CopyNodeMixin<CFGNode>;
using OffsetCFGNode = OffsetNodeMixin<CFGNode>;
using LoadCFGNode = LoadNodeMixin<CFGNode>;
using StoreCFGNode = StoreNodeMixin<CFGNode>;
using CallCFGNode = CallNodeMixin<CFGNode>;
using ReturnCFGNode = ReturnNodeMixin<CFGNode>;

} // namespace tpa