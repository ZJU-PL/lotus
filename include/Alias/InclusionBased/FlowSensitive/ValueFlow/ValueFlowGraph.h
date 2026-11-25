#pragma once

// An LLVM-independent implementation of the value-flow formulation in
// Li, Cifuentes and Keynes, "Boosting the Performance of Flow-sensitive
// Points-to Analysis using Value Flow", ESEC/FSE 2011, sections 4 and 5.
//
// All IDs are local to a Program. Zero is invalid for NodeID, ObjectID,
// BlockID, AccessID, and CallSiteID. FunctionID 0 is the implicit root
// function used by flat single-function Programs; explicit functions start at
// FunctionID 1. Object addresses are source nodes, never destinations of
// value-flow edges. Null and unknown are distinct objects; an empty points-to
// set is bottom, NOT an unknown pointer.

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
using FunctionID = std::uint32_t;
using CallSiteID = std::uint32_t;
using PointsToSet = lotus::analysis::SVFGNodeBS;
using NodeSet = lotus::analysis::SVFGNodeBS;

class Program {
public:
  enum class AccessKind { Load, Store };
  enum class InitializerKind { Unknown, Empty, Explicit };

  struct Object {
    NodeID address = 0;
    NodeID initialStore = 0;
    bool singleton = false;
    bool memory = true;
    bool isGlobal = true;
    FunctionID ownerFunction = 0;
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
  struct BlockEvent {
    enum class Kind { Access, Call };
    Kind kind = Kind::Access;
    std::uint32_t id = 0;
    CallSiteID afterCallAnchor = 0;
  };
  struct Block {
    FunctionID function = 0;
    std::vector<AccessID> accesses;
    std::vector<BlockEvent> events;
    std::vector<BlockID> successors;
  };
  struct Function {
    BlockID entry = 0;
    BlockID exit = 0;
    NodeID returned = 0;
    std::vector<NodeID> formalParams;
    std::vector<BlockID> blocks;
    bool addressTaken = false;
    std::string name;
  };
  struct CallSite {
    BlockID block = 0;
    NodeID calleePointer = 0;
    NodeID result = 0;
    std::vector<NodeID> actualArgs;
    std::vector<FunctionID> directTargets;
    std::vector<FunctionID> externalCallbacks;
    bool mayReturn = true;
    bool isInvoke = false;
    bool hasExternalTarget = false;
  };

  Program();
  NodeID addNode();
  ObjectID addObject(bool singleton, bool memory = true, std::string name = {});

  // Multi-function CFG construction (Algorithm 2: per-function CFGs +
  // PreprocessGlobalFlow interprocedural wiring).
  FunctionID addFunction(std::string name = {});
  BlockID addBlock();
  BlockID addBlock(FunctionID function);
  void setFunctionEntry(FunctionID function, BlockID entry);
  void setFunctionExit(FunctionID function, BlockID exit);
  void setFunctionReturn(FunctionID function, NodeID returned);
  void setFunctionParams(FunctionID function, std::vector<NodeID> params);
  void setAddressTakenFunction(FunctionID function, bool taken = true);
  void setFunctionObject(ObjectID object, FunctionID function);
  void addEntryFunction(FunctionID function);

  // Legacy flat-CFG root registration (associates root with its block's
  // function, typically implicit FunctionID 0).
  void addRoot(BlockID block);
  void addControlEdge(BlockID from, BlockID to);
  void addCopy(NodeID from, NodeID to);
  AccessID addLoad(BlockID block, NodeID pointer, NodeID result);
  AccessID addStore(BlockID block, NodeID pointer, NodeID source,
                    bool allowStrong = true);
  AccessID addLoadBeforeCall(CallSiteID site, NodeID pointer, NodeID result);
  AccessID addStoreBeforeCall(CallSiteID site, NodeID pointer, NodeID source,
                              bool allowStrong = true);
  AccessID addLoadAfterCall(CallSiteID site, NodeID pointer, NodeID result);
  AccessID addStoreAfterCall(CallSiteID site, NodeID pointer, NodeID source,
                             bool allowStrong = true);

  CallSiteID addCallSite(BlockID block, NodeID calleePointer = 0,
                         NodeID result = 0,
                         std::vector<NodeID> actualArgs = {},
                         bool mayReturn = true, bool isInvoke = false);
  CallSiteID addCallSiteAfterCall(CallSiteID anchor, NodeID calleePointer = 0,
                                  NodeID result = 0,
                                  std::vector<NodeID> actualArgs = {},
                                  bool mayReturn = true, bool isInvoke = false);
  void addCallTarget(CallSiteID site, FunctionID callee);
  void markCallExternal(CallSiteID site, bool isExternal = true);
  void addExternalCallback(CallSiteID site, FunctionID callback);

  // Use once per object, before constructing a Solver. Unspecified initial
  // contents default to unknown; call setEmptyInitializer() for freshly
  // allocated objects that start with no pointer contents.
  void setInitializer(ObjectID object, NodeID value);
  void setEmptyInitializer(ObjectID object);
  void setSingleton(ObjectID object, bool singleton);
  void setObjectOwner(ObjectID object, FunctionID owner);
  void setObjectGlobal(ObjectID object, bool isGlobal);

  ObjectID nullObject() const { return 1; }
  ObjectID unknownObject() const { return 2; }
  NodeID nullValue() const { return objects_.at(nullObject()).address; }
  NodeID unknownValue() const { return objects_.at(unknownObject()).address; }
  NodeID address(ObjectID object) const { return objects_.at(object).address; }
  const Object &object(ObjectID id) const { return objects_.at(id); }
  const Access &access(AccessID id) const { return accesses_.at(id); }
  const Block &block(BlockID id) const { return blocks_.at(id); }
  const Function &function(FunctionID id) const { return functions_.at(id); }
  const CallSite &callSite(CallSiteID id) const { return callSites_.at(id); }
  const std::vector<NodeID> &directSuccessors(NodeID node) const;
  bool isSourceNode(NodeID node) const;
  std::size_t nodeCount() const { return edges_.size() - 1; }
  std::size_t objectCount() const { return objects_.size() - 1; }
  std::size_t blockCount() const { return blocks_.size() - 1; }
  std::size_t accessCount() const { return accesses_.size() - 1; }
  std::size_t functionCount() const { return functions_.size() - 1; }
  std::size_t callSiteCount() const { return callSites_.size() - 1; }

private:
  friend class Solver;
  void checkNode(NodeID id) const;
  void checkBlock(BlockID id) const;
  void checkFunction(FunctionID id, bool allowImplicit = false) const;
  void checkCallSite(CallSiteID id) const;
  std::vector<std::vector<NodeID>> edges_{1};
  std::vector<bool> sources_{false};
  std::vector<Object> objects_{1};
  std::vector<InitializerKind> initializerKinds_{InitializerKind::Unknown};
  std::vector<NodeID> initializers_{0};
  std::vector<FunctionID> functionOfObject_{0};
  std::vector<Access> accesses_{1};
  std::vector<Block> blocks_{1};
  std::vector<Function> functions_{1}; // index 0 = implicit flat-CFG function
  std::vector<CallSite> callSites_{1};
  std::set<BlockID> roots_;
  std::set<FunctionID> entryFunctions_;
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
    std::size_t functionFlowComputations = 0;
    std::size_t functionFlowReuseHits = 0;
    std::size_t interproceduralAuxEdges = 0;
    // Internal nodes factoring memory flow after SCC condensation and
    // interprocedural call-in/call-out auxiliary nodes. These are not Program
    // NodeIDs and are omitted from pointedToBy() queries.
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
    std::size_t dynamicIndirectTargets = 0;
    std::size_t strongResets = 0;
    // True means at least one scalar object was locally weakened due to a
    // scalar escape cycle, an unknown-pointer escape, or a late strong update.
    // Unrelated scalar objects retain their strong updates.
    bool usedWeakFallback = false;
    std::size_t weakFallbackObjects = 0;
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

  // VFG inspection API for downstream value-flow applications.
  std::size_t vfgNodeCount() const;
  bool isJoinNode(NodeID node) const;
  const NodeSet &vfgSuccessors(NodeID node) const;
  NodeSet vfgIndirectSuccessors(NodeID node) const;
  const std::set<FunctionID> &resolvedCallTargets(CallSiteID site) const;
  bool isFunctionReachable(FunctionID function) const;
  bool isBlockReachable(BlockID block) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::alias::vfg
