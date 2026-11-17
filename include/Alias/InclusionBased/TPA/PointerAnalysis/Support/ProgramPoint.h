#pragma once

#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cassert>
#include <cstddef>

namespace context {
class Context;
} // namespace context

namespace tpa {

class CFGNode;

// Program point representation
//
// A ProgramPoint uniquely identifies a location in the program during analysis.
// It combines:
// 1. Context: The calling context (call stack) at this point
// 2. CFG Node: The control flow graph node being analyzed
//
// This enables flow-sensitive and context-sensitive analysis by tracking
// exactly where in the program (with what context) we are analyzing.
//
// Use Cases:
// - Worklist entries: (context, node) pairs to analyze
// - Context identification: determining when to create new contexts
// - Memoization keys: caching analysis results per (context, node)
//
// Example:
//   main() -> foo() at instruction %x = load i32*
//   Context: [main->foo call site]
//   CFGNode: LoadNode for that instruction
class ProgramPoint {
private:
  const context::Context *ctx;
  const CFGNode *node;

public:
  // Constructor
  // Parameters: c - the calling context, n - the CFG node
  ProgramPoint(const context::Context *c, const CFGNode *n) : ctx(c), node(n) {
    assert(ctx != nullptr && node != nullptr);
  }

  // Get the calling context at this program point
  const context::Context *getContext() const { return ctx; }
  // Get the CFG node at this program point
  const CFGNode *getCFGNode() const { return node; }

  // Equality: same context and same node
  bool operator==(const ProgramPoint &rhs) const {
    return ctx == rhs.ctx && node == rhs.node;
  }
  bool operator!=(const ProgramPoint &rhs) const { return !(*this == rhs); }
  // Ordering for use in ordered containers
  bool operator<(const ProgramPoint &rhs) const {
    if (ctx < rhs.ctx)
      return true;
    else if (rhs.ctx < ctx)
      return false;
    else
      return node < rhs.node;
  }
  bool operator>(const ProgramPoint &rhs) const { return rhs < *this; }
  bool operator<=(const ProgramPoint &rhs) const { return !(rhs < *this); }
  bool operator>=(const ProgramPoint &rhs) const { return !(*this < rhs); }
};

} // namespace tpa

namespace std {
template <> struct hash<tpa::ProgramPoint> {
  size_t operator()(const tpa::ProgramPoint &p) const {
    return util::hashPair(p.getContext(), p.getCFGNode());
  }
};
} // namespace std
