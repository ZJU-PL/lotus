//
// Created by peiming on 10/23/19.
//

// the pts data structure that also stores pointed by information
#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/PTSTrait.h"
#include "Alias/InclusionBased/AserPTA/Util/Util.h"

namespace aser {

// this can be slow as the pointedBy relation can not be
// updated in constant time.
class PointedByPts {
  using TargetID = NodeID;
  using PtsTy = llvm::SparseBitVector<>;
  using iterator = PtsTy::iterator;

  // points to set
  static std::vector<PtsTy> pointsTo;
  // pointed by set
  static std::vector<PtsTy> pointedBy;

  static void clearAll() {
    pointsTo.clear();
    pointedBy.clear();
  }

  static inline void onNewNodeCreation(NodeID id) {
    assert(id == pointsTo.size());
    assert(pointsTo.size() == pointedBy.size());

    pointsTo.emplace_back();
    pointedBy.emplace_back();

    assert(pointsTo.size() == id + 1 && pointedBy.size() == id + 1);
  }

  // union the pts of the nodes
  static inline bool unionWith(NodeID src, NodeID dst) {
    assert(src < pointsTo.size() && dst < pointsTo.size());
    // update the pointed by relation first
    for (NodeID id : pointsTo[dst]) {
      // must be pointed by dst already
      assert(pointedBy[id].test(dst));
      // now can also be pointed by src
      pointedBy[id].set(src);
    }
    return pointsTo[src] |= pointsTo[dst];
  }

  // whether the two pts intersect
  __attribute__((warn_unused_result)) static inline bool intersectWith(NodeID src, NodeID dst) {
    assert(src < pointsTo.size() && dst < pointsTo.size());
    return pointsTo[src].intersects(pointsTo[dst]);
  }

  __attribute__((warn_unused_result)) static inline bool intersectWithNoSpecialNode(NodeID src,
                                                              NodeID dst) {
    assert(src < pointsTo.size() && dst < pointsTo.size());
    auto result = pointsTo[src] & pointsTo[dst];

    for (NodeID i = 0; i < NORMAL_NODE_START_ID; i++) {
      // remove special node
      result.reset(i);
    }

    return !result.empty();
  }

  // insert a node into the pts
  static inline bool insert(NodeID src, TargetID idx) {
    assert(src < pointsTo.size() && idx < pointsTo.size());
    // idx now can be pointed by src
    pointedBy[idx].set(src);
    return pointsTo[src].test_and_set(idx);
  }

  __attribute__((warn_unused_result)) static inline bool equal(NodeID src, NodeID dst) {
    assert(src < pointsTo.size() && dst < pointsTo.size());
    return pointsTo[src] == pointsTo[dst];
  }

  // Return true if this has idx as an element
  __attribute__((warn_unused_result)) static inline bool has(NodeID src, TargetID idx) {
    assert(src < pointsTo.size() && idx < pointsTo.size());
    return pointsTo[src].test(idx);
  }

  // Return true if *this is a superset of other
  __attribute__((warn_unused_result)) static inline bool contains(NodeID src, NodeID dst) {
    assert(src < pointsTo.size() && dst < pointsTo.size());
    return pointsTo[src].contains(pointsTo[dst]);
  }

  __attribute__((warn_unused_result)) static inline bool isEmpty(NodeID id) {
    assert(id < pointsTo.size());
    return pointsTo[id].empty();
  }

  __attribute__((warn_unused_result)) static inline iterator begin(NodeID id) {
    assert(id < pointsTo.size());
    return pointsTo[id].begin();
  }

  __attribute__((warn_unused_result)) static inline iterator end(NodeID id) {
    assert(id < pointsTo.size());
    return pointsTo[id].end();
  }

  static inline void clear(NodeID id) {
    assert(id < pointsTo.size());
    pointsTo[id].clear();
  }

  __attribute__((warn_unused_result)) static inline const PtsTy &getPointedBy(NodeID id) {
    assert(id < pointsTo.size());
    return pointedBy[id];
  }

  __attribute__((warn_unused_result)) static inline const PtsTy &getPointsTo(NodeID id) {
    assert(id < pointsTo.size());
    return pointsTo[id];
  }

  __attribute__((warn_unused_result)) static inline size_t count(NodeID id) {
    assert(id < pointsTo.size());
    return pointsTo[id].count();
  }

  static inline constexpr bool supportPointedBy() { return true; }

  friend struct PTSTrait<PointedByPts>;
};

} // namespace aser

DEFINE_PTS_TRAIT(aser::PointedByPts)
