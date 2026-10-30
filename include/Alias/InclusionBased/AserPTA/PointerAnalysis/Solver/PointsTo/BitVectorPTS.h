//
// Created by peiming on 9/9/19.
//
#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/PTSTrait.h"

#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace aser {

using NodeID = uint64_t;

class BitVectorPTS {
private:
  using TargetID = NodeID;
  using PtsTy = llvm::SparseBitVector<5120>;
  using iterator = PtsTy::iterator;

  static std::vector<PtsTy> ptsVec;

  static inline void onNewNodeCreation(NodeID id) {
    // should be the same value
    assert(id == ptsVec.size());
    ptsVec.emplace_back();
    assert(ptsVec.size() == (id + 1));
  }

  static inline void clearAll() { ptsVec.clear(); }

  // get the pts of the corresponding node
  __attribute__((warn_unused_result)) static inline const PtsTy &getPointsTo(NodeID id) {
    assert(id < ptsVec.size());
    return ptsVec[id];
  }

  // union the pts of the nodes
  static inline bool unionWith(NodeID src, NodeID dst) {
    assert(src < ptsVec.size() && dst < ptsVec.size());
    bool r = ptsVec[src] |= ptsVec[dst];
    assert(ptsVec[src].find_last() < 0
               ? true
               : ptsVec[src].find_last() < ptsVec.size());
    return r;
  }

  // whether the two pts intersect
  __attribute__((warn_unused_result)) static inline bool intersectWith(NodeID src, NodeID dst) {
    assert(src < ptsVec.size() && dst < ptsVec.size());
    return ptsVec[src].intersects(ptsVec[dst]);
  }

  __attribute__((warn_unused_result)) static inline bool intersectWithNoSpecialNode(NodeID src,
                                                              NodeID dst) {
    assert(src < ptsVec.size() && dst < ptsVec.size());
    auto result = ptsVec[src] & ptsVec[dst];

    for (NodeID i = 0; i < NORMAL_NODE_START_ID; i++) {
      // remove special node
      result.reset(i);
    }

    return !result.empty();
  }

  // insert a node into the pts
  static inline bool insert(NodeID src, TargetID idx) {
    assert(src < ptsVec.size() && idx < ptsVec.size());
    return ptsVec[src].test_and_set(idx);
  }

  // Return true if this has idx as an element
  __attribute__((warn_unused_result)) static inline bool has(NodeID src, TargetID idx) {
    assert(src < ptsVec.size() && idx < ptsVec.size());
    return ptsVec[src].test(idx);
  }

  __attribute__((warn_unused_result)) static inline bool equal(NodeID src, NodeID dst) {
    assert(src < ptsVec.size() && dst < ptsVec.size());
    return ptsVec[src] == ptsVec[dst];
  }

  // Return true if *this is a superset of other
  __attribute__((warn_unused_result)) static inline bool contains(NodeID src, NodeID dst) {
    assert(src < ptsVec.size() && dst < ptsVec.size());
    return ptsVec[src].contains(ptsVec[dst]);
  }

  __attribute__((warn_unused_result)) static inline bool isEmpty(NodeID id) {
    assert(id < ptsVec.size());
    return ptsVec[id].empty();
  }

  __attribute__((warn_unused_result)) static inline iterator begin(NodeID id) {
    assert(id < ptsVec.size());
    assert(*ptsVec[id].begin() < ptsVec.size());
    return ptsVec[id].begin();
  }

  __attribute__((warn_unused_result)) static inline iterator end(NodeID id) {
    assert(id < ptsVec.size());
    return ptsVec[id].end();
  }

  static inline void clear(NodeID id) {
    assert(id < ptsVec.size());
    ptsVec[id].clear();
  }

  static inline size_t count(NodeID id) {
    assert(id < ptsVec.size());
    return ptsVec[id].count();
  }

  static inline const PtsTy &getPointedBy(NodeID id) {
    llvm_unreachable(
        "not supported by BitVectorPTS, use PointedByPts instead ");
  }

  // TODO: simply traverse the whole points-to information to gather the
  // pointed by information
  static inline constexpr bool supportPointedBy() { return false; }

  friend struct PTSTrait<BitVectorPTS>;
};

// DEFINE_PTS_TRAIT(BitVectorPTS)

} // namespace aser
DEFINE_PTS_TRAIT(BitVectorPTS)

