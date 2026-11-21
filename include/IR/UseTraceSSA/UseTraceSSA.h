#pragma once


#include <cstdint>
#include <iosfwd>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
namespace usetracessa {

using ID = std::uint32_t;
using ValueID = ID;
using BlockID = ID;
using EdgeID = ID;
using SiteID = ID;
using RegionID = ID;
using VersionID = ID;
constexpr ID InvalidID = std::numeric_limits<ID>::max();

/// An atomic use site. All uses read the pre-site state; repeated operands of
/// the same value share one psi. Definitions happen after uses. Separate
/// original SSA values must have separate ValueIDs.
struct Operation {
  SiteID id = InvalidID;
  std::string label;
  std::vector<ValueID> uses;
  std::vector<ValueID> definitions;
};

struct Block {
  std::string name;
  std::vector<SiteID> operations;
};

/// Edge IDs, rather than (from, to) pairs, distinguish parallel CFG edges.
/// Operations on an edge occur after its source block and before its target.
struct Edge {
  BlockID from = InvalidID;
  BlockID to = InvalidID;
  std::string label;
  std::vector<SiteID> operations;
};

/// A small, LLVM-independent description of an SSA function. This is also a
/// test seam for the construction algorithm. LLVMHistoryBuilder imports LLVM IR.
class Program {
public:
  ValueID addValue(std::string name);
  BlockID addBlock(std::string name);
  EdgeID addEdge(BlockID from, BlockID to, std::string label = {});
  SiteID addOperation(BlockID block, std::string label,
                      std::vector<ValueID> uses = {},
                      std::vector<ValueID> definitions = {});
  SiteID addEdgeOperation(EdgeID edge, std::string label,
                          std::vector<ValueID> uses = {},
                          std::vector<ValueID> definitions = {});
  void setEntry(BlockID block);

  BlockID entry() const { return Entry; }
  const std::vector<std::string> &values() const { return Values; }
  const std::vector<Block> &blocks() const { return Blocks; }
  const std::vector<Edge> &edges() const { return Edges; }
  const std::vector<Operation> &operations() const { return Operations; }

private:
  SiteID makeOperation(std::string label, std::vector<ValueID> uses,
                       std::vector<ValueID> definitions);
  BlockID Entry = InvalidID;
  std::vector<std::string> Values;
  std::vector<Block> Blocks;
  std::vector<Edge> Edges;
  std::vector<Operation> Operations;
};

enum class NodeKind { Definition, Psi, Phi };

struct Incoming {
  VersionID version = InvalidID;
  /// For phi, the predecessor in the edge-expanded CFG. For psi, InvalidID.
  RegionID predecessor = InvalidID;
};

struct Node {
  VersionID id = InvalidID;
  NodeKind kind = NodeKind::Definition;
  ValueID value = InvalidID;
  RegionID region = InvalidID;
  /// Original operation, or InvalidID for an inserted history phi.
  SiteID site = InvalidID;
  std::vector<Incoming> incoming;
};

struct UseVersion {
  SiteID site = InvalidID;
  ValueID value = InvalidID;
  VersionID before = InvalidID;
  VersionID after = InvalidID;
};

/// A real block or a virtual edge block. The LLVM CFG is never modified.
struct Region {
  BlockID block = InvalidID;
  EdgeID edge = InvalidID;
  bool reachable = false;
  std::vector<RegionID> predecessors;
  std::vector<RegionID> successors;
  std::vector<SiteID> operations;
};

/// A finite representation of (possibly infinite) sets of use traces.
/// Definition denotes {epsilon}, psi appends a use site, phi unions histories.
/// Cycles represent loop histories; they are not unrolled.
class Graph {
public:
  /// Throws invalid_argument for non-SSA input (including missing or duplicate
  /// reachable definitions). Unreachable regions are retained but not analyzed.
  static Graph build(Program program);

  const Program &program() const { return Input; }
  const std::vector<Node> &nodes() const { return Nodes; }
  const std::vector<Region> &regions() const { return Regions; }
  const Node &node(VersionID id) const { return Nodes.at(id); }
  const std::vector<VersionID> &successors(VersionID id) const {
    return Users.at(id);
  }
  VersionID definition(ValueID value) const { return Definitions.at(value); }
  const UseVersion *use(SiteID site, ValueID value) const;
  const std::vector<UseVersion> &usesAt(SiteID site) const {
    return Uses.at(site);
  }
  /// Block/edge region IDs are stable, even for unreachable input regions.
  RegionID blockRegion(BlockID block) const;
  RegionID edgeRegion(EdgeID edge) const;

  /// Existential, intraprocedural HISTORY reachability, not a path-feasibility
  /// proof or an alias/taint analysis. Traps include endpoints. The returned
  /// witness includes both endpoints. Empty means no unblocked path.
  std::vector<VersionID>
  findPath(VersionID source, VersionID sink,
           const std::vector<VersionID> &traps = {}) const;
  bool isReachable(VersionID source, VersionID sink,
                   const std::vector<VersionID> &traps = {}) const {
    return !findPath(source, sink, traps).empty();
  }

  /// Checks structural, definition, use-binding, and dominance invariants.
  /// Returns true when valid, with an optional first-error diagnostic.
  bool verify(std::string *error = nullptr) const;
  void print(std::ostream &out) const;
  void printDOT(std::ostream &out) const;

private:
  Program Input;
  std::vector<Region> Regions;
  std::vector<Node> Nodes;
  std::vector<std::vector<UseVersion>> Uses;
  std::unordered_map<SiteID, std::vector<std::pair<ValueID, ID>>> WideUses;
  std::vector<VersionID> Definitions;
  std::vector<std::vector<VersionID>> Users;
  std::vector<RegionID> IDom;
  std::vector<ID> DomIn, DomOut;
  std::vector<RegionID> SiteRegion;
  std::vector<ID> SitePosition;

  VersionID addNode(NodeKind kind, ValueID value, RegionID region, SiteID site);
  bool dominates(RegionID a, RegionID b) const;
  friend class Constructor;
};

} // namespace usetracessa
} // namespace lotus
