#pragma once

// An LLVM-independent implementation of the value-flow formulation in
// Li, Cifuentes and Keynes, "Boosting the Performance of Flow-sensitive
// Points-to Analysis using Value Flow", ESEC/FSE 2011, sections 4 and 5.
//
// All IDs are local to a Program. Zero is invalid. Object addresses are source
// nodes, never destinations of value-flow edges. Null and unknown are distinct
// objects; an empty points-to set is bottom, NOT an unknown pointer.

#include "IR/SVFG/SVFGNodeBS.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace lotus::alias::vfg {

using NodeID = std::uint32_t;
using ObjectID = std::uint32_t;
using BlockID = std::uint32_t;
using AccessID = std::uint32_t;
using PointsToSet = lotus::analysis::SVFGNodeBS;
using NodeSet = lotus::analysis::SVFGNodeBS;

class Program {
public:
  enum class AccessKind { Load, Store };
  struct Object {
    NodeID address = 0;
    NodeID initialStore = 0;
    bool singleton = false;
    bool memory = true;
    std::string name;
  };
  struct Access {
    AccessKind kind = AccessKind::Load;
    BlockID block = 0;
    NodeID pointer = 0;
    // A load's SSA result or a store's distinct value-flow node.
    NodeID value = 0;
    bool allowStrong = true;
  };
  struct Block {
    std::vector<AccessID> accesses;
    std::vector<BlockID> successors;
  };

  Program();
  NodeID addNode();
  ObjectID addObject(bool singleton, bool memory = true, std::string name = {});
  BlockID addBlock();
  void addRoot(BlockID block);
  void addControlEdge(BlockID from, BlockID to);
  void addCopy(NodeID from, NodeID to);
  AccessID addLoad(BlockID block, NodeID pointer, NodeID result);
  AccessID addStore(BlockID block, NodeID pointer, NodeID source,
                    bool allowStrong = true);
  // Use once per object, before constructing a Solver. Unspecified initial
  // contents are unknown. The initial definition is separate from the address.
  void setInitializer(ObjectID object, NodeID value);
  void setSingleton(ObjectID object, bool singleton);

  ObjectID nullObject() const { return 1; }
  ObjectID unknownObject() const { return 2; }
  NodeID nullValue() const { return objects_.at(nullObject()).address; }
  NodeID unknownValue() const { return objects_.at(unknownObject()).address; }
  NodeID address(ObjectID object) const { return objects_.at(object).address; }
  const Object &object(ObjectID id) const { return objects_.at(id); }
  const Access &access(AccessID id) const { return accesses_.at(id); }
  const Block &block(BlockID id) const { return blocks_.at(id); }
  std::size_t nodeCount() const { return edges_.size() - 1; }
  std::size_t objectCount() const { return objects_.size() - 1; }
  std::size_t blockCount() const { return blocks_.size() - 1; }
  std::size_t accessCount() const { return accesses_.size() - 1; }

private:
  friend class Solver;
  void checkNode(NodeID id) const;
  void checkBlock(BlockID id) const;
  std::vector<std::vector<NodeID>> edges_{1};
  std::vector<bool> sources_{false};
  std::vector<Object> objects_{1};
  std::vector<NodeID> initializers_{0};
  std::vector<Access> accesses_{1};
  std::vector<Block> blocks_{1};
  std::set<BlockID> roots_;
};

class Solver {
public:
  struct Config {
    bool enableStrongUpdates = true;
  };
  struct Statistics {
    std::size_t objectTraversals = 0;
    std::size_t indirectFlowComputations = 0;
    std::size_t indirectFlowCacheHits = 0;
    // Internal nodes factoring memory flow after SCC condensation. These
    // are not Program NodeIDs and are omitted from pointedToBy() queries.
    std::size_t valueFlowJoinNodes = 0;
    // Includes edges through internal joins, not the transitive store/load
    // relation (which may be quadratic in the size of the sparse graph).
    std::size_t indirectEdges = 0;
    std::size_t strongUpdateSites = 0;
    std::size_t projectedEvents = 0;
    std::size_t expandedControlFlowNodes = 0;
    std::size_t iteratedDominanceFrontierNodes = 0;
    std::size_t sparseGraphNodes = 0;
    std::size_t sparseGraphEdges = 0;
    // True means all stores were made weak and the graph was rebuilt from
    // direct edges. The result is conservative, not a precision-equivalent
    // implementation of the paper's scalar-acyclic fast path.
    bool usedWeakFallback = false;
    std::string fallbackReason;
  };

  explicit Solver(const Program &program);
  Solver(const Program &program, Config config);
  ~Solver();
  Solver(const Solver &) = delete;
  Solver &operator=(const Solver &) = delete;

  // The Program must remain unchanged while the solver exists. analyze() may
  // be repeated; it discards all old points-to sets and inferred edges.
  void analyze();
  const PointsToSet &pointsTo(NodeID node) const;
  const NodeSet &pointedToBy(ObjectID object) const;
  bool mayAlias(NodeID left, NodeID right) const;
  bool isStrongUpdate(AccessID access) const;
  const Statistics &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::alias::vfg
