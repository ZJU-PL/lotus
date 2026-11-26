#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowGraph.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lotus::alias::vfg {
namespace {

template <typename T> void appendUnique(std::vector<T> &values, T value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

// Single-pass iterative Tarjan SCC decomposition in source-to-sink topological
// order (zero reverse-graph allocation).
template <typename ID>
std::pair<std::vector<std::size_t>, std::size_t>
computeSCCs(const std::vector<std::vector<ID>> &successors) {
  const std::size_t count = successors.size();
  constexpr std::uint32_t kUnvis = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> disc(count, kUnvis);
  std::vector<std::uint32_t> low(count, 0);
  std::vector<std::uint8_t> onStack(count, 0);
  std::vector<std::size_t> tarjanStack;
  tarjanStack.reserve(count);
  struct Frame {
    std::size_t u;
    std::size_t edgeIdx;
  };
  std::vector<Frame> callStack;
  callStack.reserve(std::min<std::size_t>(count, 1024));
  std::vector<std::size_t> component(count, 0);
  std::uint32_t nextDisc = 0;
  std::size_t components = 0;

  for (std::size_t start = 0; start < count; ++start) {
    if (disc[start] != kUnvis)
      continue;
    disc[start] = low[start] = nextDisc++;
    tarjanStack.push_back(start);
    onStack[start] = 1;
    callStack.push_back({start, 0});
    while (!callStack.empty()) {
      auto &top = callStack.back();
      const auto &edges = successors[top.u];
      if (top.edgeIdx < edges.size()) {
        const std::size_t v = static_cast<std::size_t>(edges[top.edgeIdx++]);
        if (disc[v] == kUnvis) {
          disc[v] = low[v] = nextDisc++;
          tarjanStack.push_back(v);
          onStack[v] = 1;
          callStack.push_back({v, 0});
        } else if (onStack[v] && disc[v] < low[top.u]) {
          low[top.u] = disc[v];
        }
      } else {
        const std::size_t u = top.u;
        callStack.pop_back();
        if (!callStack.empty()) {
          const std::size_t parent = callStack.back().u;
          if (low[u] < low[parent])
            low[parent] = low[u];
        }
        if (low[u] == disc[u]) {
          while (true) {
            const std::size_t w = tarjanStack.back();
            tarjanStack.pop_back();
            onStack[w] = 0;
            component[w] = components;
            if (w == u)
              break;
          }
          ++components;
        }
      }
    }
  }
  // Flip Tarjan's sink-to-source IDs into source-to-sink topological order.
  for (std::size_t &c : component)
    c = components - 1 - c;
  return {std::move(component), components};
}

template <typename ID>
std::pair<std::vector<std::size_t>, std::size_t>
computeSCCs(const std::vector<std::vector<ID>> &successors,
            const std::vector<std::vector<ID>> & /*predecessors*/) {
  return computeSCCs(successors);
}

std::uint32_t nextID(std::size_t size) {
  if (size >= std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("ValueFlowPTA: ID space exhausted");
  return static_cast<std::uint32_t>(size);
}

std::uint64_t hashMix(std::uint64_t hash, std::uint64_t value) {
  return (hash ^ value) * 1099511628211ULL;
}

std::uint64_t hashNodeSet(std::uint64_t hash, const NodeSet &set) {
  return hashMix(hash, set.hash());
}

} // namespace

//===----------------------------------------------------------------------===//
// Program
//===----------------------------------------------------------------------===//

Program::Program() {
  functions_[0].name = "<root>";
  addObject(false, false, "<null>");
  addObject(false, true, "<unknown>");
}

void Program::checkNode(NodeID id) const {
  if (!id || id >= edges_.size())
    throw std::out_of_range("ValueFlowPTA: invalid value node");
}

void Program::checkBlock(BlockID id) const {
  if (!id || id >= blocks_.size())
    throw std::out_of_range("ValueFlowPTA: invalid control block");
}

void Program::checkFunction(FunctionID id, bool allowImplicit) const {
  if ((!allowImplicit && !id) || id >= functions_.size())
    throw std::out_of_range("ValueFlowPTA: invalid function");
}

void Program::checkCallSite(CallSiteID id) const {
  if (!id || id >= callSites_.size())
    throw std::out_of_range("ValueFlowPTA: invalid call site");
}

NodeID Program::addNode() {
  const NodeID id = nextID(edges_.size());
  edges_.emplace_back();
  sources_.push_back(false);
  return id;
}

ObjectID Program::addObject(bool singleton, bool memory, std::string name) {
  const ObjectID id = nextID(objects_.size());
  const NodeID source = addNode();
  sources_[source] = true;
  const NodeID initial = memory ? addNode() : 0;
  objects_.push_back(
      {source, initial, singleton, memory, true, false, 0, std::move(name)});
  initializerKinds_.push_back(InitializerKind::Unknown);
  initializers_.push_back(0);
  functionOfObject_.push_back(0);
  return id;
}

FunctionID Program::addFunction(std::string name) {
  const FunctionID id = nextID(functions_.size());
  Function fn;
  fn.name = std::move(name);
  functions_.push_back(std::move(fn));
  return id;
}

BlockID Program::addBlock() { return addBlock(0); }

BlockID Program::addBlock(FunctionID function) {
  checkFunction(function, /*allowImplicit=*/true);
  const BlockID id = nextID(blocks_.size());
  Block block;
  block.function = function;
  blocks_.push_back(std::move(block));
  functions_[function].blocks.push_back(id);
  return id;
}

void Program::setFunctionEntry(FunctionID function, BlockID entry) {
  checkFunction(function, /*allowImplicit=*/true);
  checkBlock(entry);
  if (blocks_[entry].function != function)
    throw std::invalid_argument(
        "ValueFlowPTA: function entry block belongs to another function");
  functions_[function].entry = entry;
}

void Program::setFunctionExit(FunctionID function, BlockID exit) {
  checkFunction(function, /*allowImplicit=*/true);
  checkBlock(exit);
  if (blocks_[exit].function != function)
    throw std::invalid_argument(
        "ValueFlowPTA: function exit block belongs to another function");
  functions_[function].exit = exit;
}

void Program::setFunctionReturn(FunctionID function, NodeID returned) {
  checkFunction(function);
  checkNode(returned);
  functions_[function].returned = returned;
}

void Program::setFunctionParams(FunctionID function,
                                std::vector<NodeID> params) {
  checkFunction(function);
  for (NodeID param : params)
    if (param)
      checkNode(param);
  functions_[function].formalParams = std::move(params);
}

void Program::setAddressTakenFunction(FunctionID function, bool taken) {
  checkFunction(function);
  functions_[function].addressTaken = taken;
}

void Program::setFunctionObject(ObjectID object, FunctionID function) {
  if (!object || object >= objects_.size())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  checkFunction(function);
  functionOfObject_[object] = function;
}

void Program::addEntryFunction(FunctionID function) {
  checkFunction(function, /*allowImplicit=*/true);
  entryFunctions_.insert(function);
}

void Program::addRoot(BlockID block) {
  checkBlock(block);
  roots_.insert(block);
  entryFunctions_.insert(blocks_[block].function);
}

void Program::addControlEdge(BlockID from, BlockID to) {
  checkBlock(from);
  checkBlock(to);
  if (blocks_[from].function != blocks_[to].function)
    throw std::invalid_argument(
        "ValueFlowPTA: control edges must be intra-procedural; use call sites "
        "for inter-procedural flow");
  appendUnique(blocks_[from].successors, to);
}

void Program::addCopy(NodeID from, NodeID to) {
  checkNode(from);
  checkNode(to);
  if (sources_[to])
    throw std::invalid_argument(
        "ValueFlowPTA: object addresses are source nodes");
  auto &list = edges_[from];
  if (list.size() < 16) {
    for (NodeID existing : list)
      if (existing == to)
        return;
    list.push_back(to);
    return;
  }
  auto it = largeEdges_.find(from);
  if (it == largeEdges_.end()) {
    NodeSet set;
    for (NodeID existing : list)
      set.insertBit(existing);
    it = largeEdges_.emplace(from, std::move(set)).first;
  }
  if (it->second.insertBit(to))
    list.push_back(to);
}

AccessID Program::addLoad(BlockID block, NodeID pointer, NodeID result) {
  checkBlock(block);
  checkNode(pointer);
  checkNode(result);
  if (sources_[result])
    throw std::invalid_argument(
        "ValueFlowPTA: load result is an object source");
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Load, block, pointer, result, false, false});
  blocks_[block].accesses.push_back(id);
  blocks_[block].events.push_back({BlockEvent::Kind::Access, id, 0});
  return id;
}

AccessID Program::addStore(BlockID block, NodeID pointer, NodeID source,
                           bool allowStrong) {
  checkBlock(block);
  checkNode(pointer);
  checkNode(source);
  const NodeID stored = addNode();
  addCopy(source, stored);
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong,
                       source == unknownValue()});
  blocks_[block].accesses.push_back(id);
  blocks_[block].events.push_back({BlockEvent::Kind::Access, id, 0});
  return id;
}

AccessID Program::addLoadBeforeCall(CallSiteID site, NodeID pointer,
                                    NodeID result) {
  checkCallSite(site);
  const BlockID block = callSites_[site].block;
  checkNode(pointer);
  checkNode(result);
  if (sources_[result])
    throw std::invalid_argument(
        "ValueFlowPTA: load result is an object source");
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Load, block, pointer, result, false, false});
  blocks_[block].accesses.push_back(id);
  auto &events = blocks_[block].events;
  auto pos = std::find_if(events.begin(), events.end(), [&](const BlockEvent &e) {
    return e.kind == BlockEvent::Kind::Call && e.id == site;
  });
  const CallSiteID inheritedAnchor =
      pos != events.end() ? pos->afterCallAnchor : 0;
  events.insert(pos, {BlockEvent::Kind::Access, id, inheritedAnchor});
  return id;
}

AccessID Program::addStoreBeforeCall(CallSiteID site, NodeID pointer,
                                     NodeID source, bool allowStrong) {
  checkCallSite(site);
  const BlockID block = callSites_[site].block;
  checkNode(pointer);
  checkNode(source);
  const NodeID stored = addNode();
  addCopy(source, stored);
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong,
                       source == unknownValue()});
  blocks_[block].accesses.push_back(id);
  auto &events = blocks_[block].events;
  auto pos = std::find_if(events.begin(), events.end(), [&](const BlockEvent &e) {
    return e.kind == BlockEvent::Kind::Call && e.id == site;
  });
  const CallSiteID inheritedAnchor =
      pos != events.end() ? pos->afterCallAnchor : 0;
  events.insert(pos, {BlockEvent::Kind::Access, id, inheritedAnchor});
  return id;
}

AccessID Program::addLoadAfterCall(CallSiteID site, NodeID pointer,
                                   NodeID result) {
  checkCallSite(site);
  const BlockID block = callSites_[site].block;
  checkNode(pointer);
  checkNode(result);
  if (sources_[result])
    throw std::invalid_argument(
        "ValueFlowPTA: load result is an object source");
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Load, block, pointer, result, false, false});
  blocks_[block].accesses.push_back(id);
  auto &events = blocks_[block].events;
  auto pos = std::find_if(events.begin(), events.end(), [&](const BlockEvent &e) {
    return e.kind == BlockEvent::Kind::Call && e.id == site;
  });
  if (pos != events.end()) {
    ++pos;
    while (pos != events.end() && pos->afterCallAnchor == site)
      ++pos;
  }
  events.insert(pos, {BlockEvent::Kind::Access, id, site});
  return id;
}

AccessID Program::addStoreAfterCall(CallSiteID site, NodeID pointer,
                                    NodeID source, bool allowStrong) {
  checkCallSite(site);
  const BlockID block = callSites_[site].block;
  checkNode(pointer);
  checkNode(source);
  const NodeID stored = addNode();
  addCopy(source, stored);
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong,
                       source == unknownValue()});
  blocks_[block].accesses.push_back(id);
  auto &events = blocks_[block].events;
  auto pos = std::find_if(events.begin(), events.end(), [&](const BlockEvent &e) {
    return e.kind == BlockEvent::Kind::Call && e.id == site;
  });
  if (pos != events.end()) {
    ++pos;
    while (pos != events.end() && pos->afterCallAnchor == site)
      ++pos;
  }
  events.insert(pos, {BlockEvent::Kind::Access, id, site});
  return id;
}

CallSiteID Program::addCallSite(BlockID block, NodeID calleePointer,
                                NodeID result, std::vector<NodeID> actualArgs,
                                bool mayReturn, bool isInvoke) {
  checkBlock(block);
  if (calleePointer)
    checkNode(calleePointer);
  if (result) {
    checkNode(result);
    if (sources_[result])
      throw std::invalid_argument(
          "ValueFlowPTA: call result is an object source");
  }
  for (NodeID arg : actualArgs)
    if (arg)
      checkNode(arg);
  const CallSiteID id = nextID(callSites_.size());
  CallSite site;
  site.block = block;
  site.calleePointer = calleePointer;
  site.result = result;
  site.actualArgs = std::move(actualArgs);
  site.mayReturn = mayReturn;
  site.isInvoke = isInvoke;
  callSites_.push_back(std::move(site));
  blocks_[block].events.push_back({BlockEvent::Kind::Call, id, 0});
  return id;
}

CallSiteID Program::addCallSiteAfterCall(CallSiteID anchor,
                                         NodeID calleePointer, NodeID result,
                                         std::vector<NodeID> actualArgs,
                                         bool mayReturn, bool isInvoke) {
  checkCallSite(anchor);
  const BlockID block = callSites_[anchor].block;
  if (calleePointer)
    checkNode(calleePointer);
  if (result) {
    checkNode(result);
    if (sources_[result])
      throw std::invalid_argument(
          "ValueFlowPTA: call result is an object source");
  }
  for (NodeID arg : actualArgs)
    if (arg)
      checkNode(arg);
  const CallSiteID id = nextID(callSites_.size());
  CallSite site;
  site.block = block;
  site.calleePointer = calleePointer;
  site.result = result;
  site.actualArgs = std::move(actualArgs);
  site.mayReturn = mayReturn;
  site.isInvoke = isInvoke;
  callSites_.push_back(std::move(site));
  auto &events = blocks_[block].events;
  auto pos = std::find_if(events.begin(), events.end(), [&](const BlockEvent &e) {
    return e.kind == BlockEvent::Kind::Call && e.id == anchor;
  });
  if (pos != events.end()) {
    ++pos;
    while (pos != events.end() && pos->afterCallAnchor == anchor)
      ++pos;
  }
  events.insert(pos, {BlockEvent::Kind::Call, id, anchor});
  return id;
}

void Program::addCallTarget(CallSiteID site, FunctionID callee) {
  checkCallSite(site);
  checkFunction(callee);
  appendUnique(callSites_[site].directTargets, callee);
}

void Program::markCallExternal(CallSiteID site, bool isExternal) {
  checkCallSite(site);
  callSites_[site].hasExternalTarget = isExternal;
}

void Program::addExternalCallback(CallSiteID site, FunctionID callback) {
  checkCallSite(site);
  checkFunction(callback);
  auto &list = callSites_[site].externalCallbacks;
  if (list.size() < 16) {
    for (FunctionID existing : list) {
      if (existing == callback) {
        callSites_[site].hasExternalTarget = true;
        return;
      }
    }
    list.push_back(callback);
  } else {
    auto it = largeExternalCallbacks_.find(site);
    if (it == largeExternalCallbacks_.end()) {
      NodeSet set;
      for (FunctionID existing : list)
        set.insertBit(existing);
      it = largeExternalCallbacks_.emplace(site, std::move(set)).first;
    }
    if (it->second.insertBit(callback))
      list.push_back(callback);
  }
  callSites_[site].hasExternalTarget = true;
}

void Program::setUnknownExternalTarget(CallSiteID site, FunctionID target,
                                       bool escapeArgs) {
  checkCallSite(site);
  if (target)
    checkFunction(target);
  callSites_[site].unknownExternalTarget = target;
  callSites_[site].unknownExternalEscapesArgs = escapeArgs;
}

void Program::addExternalEscape(BlockID block, NodeID value) {
  checkBlock(block);
  checkNode(value);
  appendUnique(blocks_[block].externalEscapes, value);
}

void Program::addExternalEscapeAfterCall(CallSiteID site, NodeID value) {
  checkCallSite(site);
  addExternalEscape(callSites_[site].block, value);
}

void Program::setInitializer(ObjectID object, NodeID value) {
  checkNode(value);
  if (!object || object >= objects_.size() || !objects_[object].memory)
    throw std::invalid_argument(
        "ValueFlowPTA: initializer needs a memory object");
  if (initializerKinds_[object] == InitializerKind::Explicit)
    throw std::invalid_argument("ValueFlowPTA: initializer already set");
  initializerKinds_[object] = InitializerKind::Explicit;
  initializers_[object] = value;
}

void Program::setEmptyInitializer(ObjectID object) {
  if (!object || object >= objects_.size() || !objects_[object].memory)
    throw std::invalid_argument(
        "ValueFlowPTA: initializer needs a memory object");
  if (initializerKinds_[object] == InitializerKind::Explicit)
    throw std::invalid_argument("ValueFlowPTA: initializer already set");
  initializerKinds_[object] = InitializerKind::Empty;
  initializers_[object] = 0;
}

void Program::setSingleton(ObjectID object, bool singleton) {
  if (!object || object >= objects_.size() || object <= unknownObject())
    throw std::invalid_argument("ValueFlowPTA: invalid singleton object");
  objects_[object].singleton = singleton;
}

void Program::setObjectOwner(ObjectID object, FunctionID owner) {
  if (!object || object >= objects_.size())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  if (owner)
    checkFunction(owner);
  objects_[object].ownerFunction = owner;
  if (owner)
    objects_[object].isGlobal = false;
}

void Program::setObjectGlobal(ObjectID object, bool isGlobal) {
  if (!object || object >= objects_.size())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  objects_[object].isGlobal = isGlobal;
}

void Program::setObjectReadOnly(ObjectID object, bool readOnly) {
  if (!object || object >= objects_.size())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  objects_[object].readOnly = readOnly;
}

const std::vector<NodeID> &Program::directSuccessors(NodeID node) const {
  checkNode(node);
  return edges_[node];
}

bool Program::isSourceNode(NodeID node) const {
  checkNode(node);
  return sources_[node];
}

//===----------------------------------------------------------------------===//
// Solver::Impl
//===----------------------------------------------------------------------===//

class Solver::Impl {
public:
  Impl(const Program &program, Config config) : p(program), config(config) {}

  const Program &p;
  Config config;
  Statistics stats;
  bool weak = false;
  bool analyzed = false;

  // VFG state (Program nodes 1..p.nodeCount() + internal join nodes).
  // `adj` stores compact outgoing edge lists for fast traversal; `largeAdjSets`
  // deduplicates high-out-degree (>16) nodes; `edgesCache` is materialized
  // lazily only if public VFG inspection APIs are queried.
  std::vector<std::vector<NodeID>> adj;
  std::unordered_map<NodeID, NodeSet> largeAdjSets;
  // Condensed copy-SCC graph driving `computePointedToBy()`. `vfgRep` maps
  // each node to its SCC representative; `pts` is stored only on active
  // representatives (collapsed members free their `SVFGNodeBS` storage).
  mutable std::vector<NodeID> vfgRep;
  std::vector<std::vector<NodeID>> propAdj;
  std::unordered_map<NodeID, NodeSet> largePropAdjSets;
  std::vector<std::vector<NodeID>> sccHookNodes;
  std::vector<std::uint8_t> repHasUnknownStoreHook;
  std::vector<std::uint8_t> repHasExternalEscapeHook;
  std::vector<std::uint8_t> repHasCallBoundaryHook;
  struct PendingHookBatch {
    PointsToSet diff;
    std::vector<NodeID> nodes;
    bool unkEscHook = false;
    bool extEscHook = false;
    bool callVisHook = false;
  };
  std::vector<PendingHookBatch> pendingHookDiffs;
  std::size_t edgesSinceCondense = 0;
  std::size_t lastCondenseTrv = 0;
  std::size_t totalCollapsedNodes = 0;
  std::size_t uncompactedCollapsed = 0;
  bool forceCondense = false;
  bool topoRanksValid = false;
  std::vector<std::pair<NodeID, NodeID>> pendingBackEdges;
  std::uint64_t incSccGen = 0;
  std::vector<std::uint64_t> incVisGen;
  std::vector<std::uint32_t> incLocalIdx;
  mutable std::vector<NodeSet> edgesCache;
  mutable bool edgesCacheBuilt = false;
  std::unordered_map<NodeID, NodeSet> extraDirectEdges;
  // Pooled copy-on-write + hash-consed PointsToSet storage. Stored in a deque
  // so references into `ptsPool[id]` remain stable across `emplace_back`
  // growth (enabling zero-copy `propDeltaPoolId` propagation). Entry 0 is the
  // immutable empty set; active SCC representatives index `ptsPool` via
  // `ptsIdx[rep]`. `ptsDrained` caches monotonic global drain bits per slot
  // (bit0=unkEsc, bit1=extEsc, bit2=callVis).
  static constexpr NodeID kNoReadOnlyJoin = std::numeric_limits<NodeID>::max();
  std::deque<PointsToSet> ptsPool;
  std::vector<std::uint32_t> ptsRefs;
  std::vector<std::uint8_t> ptsDrained;
  std::vector<NodeID> ptsReadOnlyJoin;
  std::vector<std::uint32_t> freePtsIds;
  std::vector<std::uint32_t> ptsIdx;
  std::size_t ptsSlotsAtLastHashCons = 0;
  mutable std::vector<NodeSet> pted; // materialized lazily on pointedToBy()
  mutable bool ptedBuilt = false;
  std::vector<std::pair<ObjectID, NodeID>> pendingObjSeeds;
  std::vector<std::pair<NodeID, NodeID>> pendingEdgeSeeds;
  using PropWorkItem = std::pair<std::uint64_t, NodeID>;
  std::priority_queue<PropWorkItem> propWork;
  std::vector<std::uint64_t> nodeTopoRank;
  std::vector<std::uint32_t> propDeltaSlot;
  std::vector<std::uint32_t> propDeltaPoolId;
  std::vector<std::uint8_t> propNeedsHook;
  std::vector<PointsToSet> deltaPool;
  std::vector<std::uint32_t> freeDeltaSlots;
  std::vector<bool> nodeHasHook; // sized to p.edges_.size() only
  std::uint64_t reachEpoch = 0;
  std::uint64_t callGraphVersion = 1;

  // Per-object tracking.
  std::vector<std::uint8_t> dirtySigMark;
  std::vector<ObjectID> dirtySigList;
  std::vector<bool> callGraphDirty;
  std::vector<NodeSet> activeAccesses;
  std::vector<std::vector<AccessID>> pendingAccesses;
  std::vector<std::vector<NodeID>> escapePointers;
  std::vector<bool> escapePtrDirty;
  std::vector<std::uint32_t> escapePtrNextDedup;
  std::vector<NodeSet> initialEscapeObjects;
  std::vector<std::vector<CallSiteID>> escapedIndirectCalls;
  std::vector<ObjectID> blockedWitness;
  std::vector<std::uint64_t> blockedEmptyEpoch;
  std::vector<bool> effectiveSingleton;
  std::vector<FunctionID> effectiveOwner;
  std::vector<bool> forcedWeak;
  std::set<ObjectID> pending;
  std::vector<bool> processed;
  std::vector<bool> emittedFlow;
  std::set<AccessID> finalStrong;

  // Node -> access / callsite use maps (fixed at p.edges_.size(); join nodes
  // never appear here, avoiding ~160 bytes of dead metadata per join node).
  std::vector<std::vector<AccessID>> uses;
  std::vector<NodeID> storedThrough;
  std::vector<ObjectID> initializedObject;
  std::vector<std::vector<CallSiteID>> indirectCallUses;
  std::vector<std::vector<CallSiteID>> indirectArgUses;
  std::vector<bool> isIndirectCalleeLoad;
  std::vector<bool> isCallBoundaryNode;
  std::vector<std::vector<CallSiteID>> callBoundarySites;
  std::vector<bool> siteUnknownBoundaryMarked;
  std::vector<bool> siteUnknownArgsEscaped;
  std::vector<bool> isExternalEscapeNode;
  std::vector<bool> nodeHadMultiTarget;
  std::vector<bool> objCallExposed;
  std::vector<bool> objExternallyEscaped;
  PointsToSet externallyEscapedSet;
  std::vector<bool> objEscapedToUnknown;
  PointsToSet escapedToUnknownSet;
  std::vector<std::uint8_t> nodeHasLoadUse;
  // Read-only constant objects skipped by per-object dereference tracking.
  PointsToSet skipUseObjSet;
  std::vector<std::uint8_t> isEmptyReadOnly;
  std::vector<std::uint8_t> entryReadOnlyWired;
  NodeID allReadOnlyInitJoin = 0;
  std::uint64_t callVisGen = 0;
  std::vector<std::uint64_t> callVisMarkedGen;
  std::uint64_t escBlockGen = 0;
  std::vector<std::uint64_t> escObjVisGen;
  std::vector<std::uint64_t> escPtrVisGen;
  std::vector<NodeSet> reverseEscape;
  std::vector<NodeSet> ptrEscapedBy;
  std::vector<bool> ptrHasExternalTarget;
  std::vector<bool> ptrHasUnknownTarget;

  // Cached wildcard (`unknown`) access summaries across the call graph so
  // objects only project over their own active functions rather than every
  // function containing an opaque external `store unknown, unknown`.
  std::uint64_t unknownAccessVersion = 0;
  std::uint64_t unknownSummaryVersion = 0;
  std::uint64_t unknownSummaryChangeVersion = 1;
  std::unordered_map<FunctionID, NodeSet> unknownFnAccesses;
  std::vector<bool> unknownDirectRef;
  std::vector<FunctionID> unknownDirectRefFuncs;
  std::vector<bool> unknownTransRef;
  std::vector<NodeID> unknownReadEntry;
  std::vector<NodeID> unknownModSummary;
  struct NodeSetHash {
    std::size_t operator()(const NodeSet &set) const {
      return static_cast<std::size_t>(
          hashNodeSet(14695981039346656037ULL, set));
    }
  };
  std::unordered_map<NodeSet, NodeID, NodeSetHash> unknownSummaryNodes;
  std::unordered_map<NodeSet, NodeID, NodeSetHash> readOnlySourceJoinCache;
  PointsToSet lastBigROSubset;
  NodeID lastBigROJoin = 0;
  std::vector<bool> objLocalOnly;

  // Generation-stamped per-function scratch arrays for O(|ActiveFuncs|)
  // computeIndirect without O(|Funcs|) allocations or full-module scans.
  std::uint64_t indirectGen = 0;
  std::vector<std::uint64_t> fnDirectModGen;
  std::vector<std::uint64_t> fnDirectRefGen;
  std::vector<std::uint32_t> fnDirectIdx;
  std::vector<std::uint64_t> fnTransModGen;
  std::vector<std::uint64_t> fnTransRefGen;
  std::vector<std::uint64_t> fnTransStrongGen;
  std::vector<std::uint64_t> fnActiveGen;
  std::vector<std::uint32_t> fnActiveIdx;
  std::vector<std::uint64_t> fnSubgraphGen;
  std::vector<std::uint64_t> siteCarryGen;
  std::vector<std::uint8_t> siteCarryVal;
  std::vector<NodeID> fnLocalExitUnkStoreNode;
  std::vector<NodeID> fnLocalUnkLoadNode;
  std::vector<std::uint8_t> fnHasNonExitUnkStore;
  std::vector<std::uint8_t> fnLocalUnkAccessWired;

  // Incremental inverted index of non-alwaysCarry callsites carrying each
  // concrete non-global object (exact; populated at call-boundary hooks).
  std::uint64_t alwaysCarryVersion = 1;
  std::vector<NodeSet> objCarrySites;

  // Exact universal-carry returning call-graph SCCs (Theorem X):
  // Any returning `siteAlwaysCarries` SCC (`|members| >= 2`) satisfying the
  // intra-SCC Cycle-Return Condition (`exit_u ->* entry_u`) collapses to a
  // single super-vertex during exact weak-object projections.
  struct UnivExtModGroup {
    NodeID summaryNode = 0;
    std::vector<FunctionID> canonTargets; // deduplicated canon(v)s
  };
  struct UnivExtSegTree {
    struct Entry {
      FunctionID canonTarget = 0;
      NodeID readNode = 0;
    };
    std::vector<Entry> entries;
    std::vector<std::pair<FunctionID, std::pair<std::uint32_t, std::uint32_t>>>
        canonRanges;
    std::vector<NodeID> tree; // 1-indexed binary segment tree
    std::size_t leafBase = 0;
    void clear() {
      entries.clear();
      canonRanges.clear();
      tree.clear();
      leafBase = 0;
    }
  };
  struct UnivExtSummary {
    bool built = false;
    std::uint64_t builtSumVer = 0;
    std::vector<UnivExtModGroup> modGroups;
    UnivExtSegTree readSeg;
    void invalidate() {
      built = false;
      builtSumVer = 0;
      modGroups.clear();
      readSeg.clear();
    }
  };
  struct UnivWeakSCC {
    FunctionID repFunc = 0;
    std::uint64_t structVersion = 0;
    std::uint64_t stateVer = 1;
    bool containsHub = false;
    bool containsEntry = false;
    std::vector<FunctionID> members;
    std::vector<FunctionID> nonExitUnkFuncs;
    std::vector<FunctionID> nonExitExtRetFuncs;
    NodeID unkStoreJoin = 0;
    NodeID unkLoadJoin = 0;
    bool hasLocalExitUnkStore = false;
    std::uint64_t unkVersion = 0;
    std::vector<FunctionID> alwaysRetExtCallees;
    std::vector<FunctionID> alwaysNoRetExtCallees;
    std::vector<FunctionID> globalRetExtCallees;
    std::vector<FunctionID> globalNoRetExtCallees;
    NodeSet alwaysRetExtCanonSet;
    NodeSet alwaysNoRetExtCanonSet;
    NodeSet globalRetExtCanonSet;
    NodeSet globalNoRetExtCanonSet;
    NodeSet alwaysRetExtRawSet;
    NodeSet alwaysNoRetExtRawSet;
    bool alwaysRetCallsHub = false;
    bool alwaysNoRetCallsHub = false;
    bool globalRetCallsHub = false;
    bool globalNoRetCallsHub = false;
    UnivExtSummary alwaysExtSummary;
    UnivExtSummary globalExtSummary;
    std::vector<CallSiteID> extCallerSites;
    NodeSet accessedByObjects;
    std::vector<std::uint32_t> weakAncCtxIds;
    std::vector<std::uint32_t> weakAncCoreWatchIds;
  };
  std::vector<UnivWeakSCC> univSccs; // 1-indexed
  std::vector<std::uint32_t> fnUnivScc; // sized numFuncs + 1 (includes hubIdx)
  std::uint64_t univSccCGVer = 0;
  std::uint64_t univSccAlwaysVer = 0;
  std::uint64_t univSccUnkVer = 0;
  std::uint64_t univSccBuildId = 0;
  std::uint64_t nextUnivStructVersion = 1;
  std::uint64_t nextUnivStateVer = 1;
  std::uint64_t univDirtyGen = 0;
  std::vector<std::uint64_t> univDirtyStamp;
  std::vector<std::uint64_t> fnStateVer; // per-function local/call/summary ver
  std::uint64_t entryUnkRefVersion = 1;
  std::uint64_t hubGlobalVersion = 1;

  // Local temporary graph capture used to build exact Theorem Y
  // WeakAncestorContext summaries without polluting the main VFG.
  static constexpr NodeID kLocalNodeBase = 0x80000000U;
  struct LocalGraphCapture {
    bool active = false;
    std::vector<std::vector<std::uint32_t>> adj;
    std::unordered_map<std::uint32_t, NodeSet> largeSets;
    std::vector<NodeID> srcRealNode;
    std::vector<NodeID> snkRealNode;
    std::unordered_map<NodeID, std::uint32_t> realToSrcIdx;
    std::unordered_map<NodeID, std::uint32_t> realToSnkIdx;

    void reset() {
      active = false;
      adj.clear();
      largeSets.clear();
      srcRealNode.clear();
      snkRealNode.clear();
      realToSrcIdx.clear();
      realToSnkIdx.clear();
    }
    NodeID allocNode() {
      const std::uint32_t idx = static_cast<std::uint32_t>(adj.size());
      adj.emplace_back();
      srcRealNode.push_back(0);
      snkRealNode.push_back(0);
      return kLocalNodeBase | idx;
    }
    std::uint32_t getOrAddSrc(NodeID real) {
      auto [it, ins] = realToSrcIdx.emplace(real, 0);
      if (ins) {
        const std::uint32_t idx = static_cast<std::uint32_t>(adj.size());
        adj.emplace_back();
        srcRealNode.push_back(real);
        snkRealNode.push_back(0);
        it->second = idx;
      }
      return it->second;
    }
    std::uint32_t getOrAddSnk(NodeID real) {
      auto [it, ins] = realToSnkIdx.emplace(real, 0);
      if (ins) {
        const std::uint32_t idx = static_cast<std::uint32_t>(adj.size());
        adj.emplace_back();
        srcRealNode.push_back(0);
        snkRealNode.push_back(real);
        it->second = idx;
      }
      return it->second;
    }
    bool addEdge(NodeID from, NodeID to) {
      if (from == to)
        return false;
      const std::uint32_t u = (from >= kLocalNodeBase)
                                  ? (from & ~kLocalNodeBase)
                                  : getOrAddSrc(from);
      const std::uint32_t v = (to >= kLocalNodeBase)
                                  ? (to & ~kLocalNodeBase)
                                  : getOrAddSnk(to);
      if (u == v)
        return false;
      auto &list = adj[u];
      if (list.size() < 16) {
        for (std::uint32_t existing : list)
          if (existing == v)
            return false;
        list.push_back(v);
        return true;
      }
      auto it = largeSets.find(u);
      if (it == largeSets.end()) {
        NodeSet set;
        for (std::uint32_t existing : list)
          set.insertBit(existing);
        it = largeSets.emplace(u, std::move(set)).first;
      }
      if (!it->second.insertBit(v))
        return false;
      list.push_back(v);
      return true;
    }
  } localCap;

  // Exact Theorem Y: Parametric Zero-Own-Access Ancestor-Context Summary.
  // Summarizes the entire strict-ancestor subgraph `ANC` above `CORE` into
  // exact reachability ports (init -> coreEntry, unkSrc -> coreEntry,
  // coreExit -> coreEntry, coreExit -> unkSnk, init -> unkSnk), emitting
  // `U_src -> U_snk` once and reusing the summary across all weak objects.
  struct NodeVecHashImpl {
    std::size_t operator()(const std::vector<NodeID> &v) const {
      std::uint64_t h = 14695981039346656037ULL;
      for (NodeID x : v)
        h = hashMix(h, x);
      return static_cast<std::size_t>(h);
    }
  };
  struct WeakAncSummaryKey {
    std::vector<FunctionID> coreFuncs;
    std::vector<std::uint8_t> initReachesCoreEntry;
    std::vector<std::uint8_t> coreNeedExitFromAnc;
    std::vector<NodeID> unkSrcToCoreEntry;
    std::vector<NodeID> coreExitToUnkSnk;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> coreExitToEntry;
    NodeID initToUnkSnk = 0;
    bool operator==(const WeakAncSummaryKey &o) const {
      return coreFuncs == o.coreFuncs &&
             initReachesCoreEntry == o.initReachesCoreEntry &&
             coreNeedExitFromAnc == o.coreNeedExitFromAnc &&
             unkSrcToCoreEntry == o.unkSrcToCoreEntry &&
             coreExitToUnkSnk == o.coreExitToUnkSnk &&
             coreExitToEntry == o.coreExitToEntry &&
             initToUnkSnk == o.initToUnkSnk;
    }
  };
  struct WeakAncSummaryHash {
    std::size_t operator()(const WeakAncSummaryKey &k) const {
      std::uint64_t h = 14695981039346656037ULL;
      for (FunctionID f : k.coreFuncs)
        h = hashMix(h, f);
      for (std::uint8_t b : k.initReachesCoreEntry)
        h = hashMix(h, b);
      for (std::uint8_t b : k.coreNeedExitFromAnc)
        h = hashMix(h, b);
      for (NodeID n : k.unkSrcToCoreEntry)
        h = hashMix(h, n);
      for (NodeID n : k.coreExitToUnkSnk)
        h = hashMix(h, n);
      for (const auto &pr : k.coreExitToEntry)
        h = hashMix(h, (static_cast<std::uint64_t>(pr.first) << 32) | pr.second);
      h = hashMix(h, k.initToUnkSnk);
      return static_cast<std::size_t>(h);
    }
  };
  struct WeakAncestorContext {
    std::uint32_t id = 0;
    std::uint32_t summaryId = 0;
    bool stale = false;
    std::uint64_t builtUnivBuild = 0;
    std::uint64_t builtEntryUnkRefVer = 0;
    std::uint64_t builtHubGlobalVer = 0;
    std::vector<FunctionID> coreFuncs;
    std::vector<std::uint8_t> coreIsMod;
    std::vector<std::uint8_t> coreIsRef;
    std::vector<std::uint8_t> initReachesCoreEntry;
    std::vector<std::uint8_t> coreNeedExitFromAnc;
    std::vector<NodeID> unkSrcToCoreEntry;
    std::vector<NodeID> coreExitToUnkSnk;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> coreExitToEntry;
    NodeID initToUnkSnk = 0;
    NodeSet accessedByObjects;
  };
  struct WeakAncPreKey {
    std::uint64_t univBuild = 0;
    bool isGlobal = false;
    FunctionID scopeRoot = 0;
    std::vector<std::uint64_t> directPorts;
    bool operator==(const WeakAncPreKey &o) const {
      return univBuild == o.univBuild && isGlobal == o.isGlobal &&
             scopeRoot == o.scopeRoot && directPorts == o.directPorts;
    }
  };
  struct WeakAncPreHash {
    std::size_t operator()(const WeakAncPreKey &k) const {
      std::uint64_t h = 14695981039346656037ULL;
      h = hashMix(h, k.univBuild);
      h = hashMix(h, k.isGlobal ? 1ULL : 0ULL);
      h = hashMix(h, k.scopeRoot);
      for (std::uint64_t x : k.directPorts)
        h = hashMix(h, x);
      return static_cast<std::size_t>(h);
    }
  };
  struct WeakAncPostKey {
    std::uint64_t univBuild = 0;
    std::uint64_t entryUnkRefVer = 0;
    std::uint64_t hubStateVer = 0;
    bool isGlobal = false;
    FunctionID scopeRoot = 0;
    std::uint8_t ancHubState = 0;
    std::vector<std::uint64_t> coreStates;
    std::vector<std::uint64_t> ancStates;
    std::vector<std::uint64_t> ancStateVers;
    std::vector<CallSiteID> ancCarrierSites;
    bool operator==(const WeakAncPostKey &o) const {
      return univBuild == o.univBuild && entryUnkRefVer == o.entryUnkRefVer &&
             hubStateVer == o.hubStateVer && isGlobal == o.isGlobal &&
             scopeRoot == o.scopeRoot && ancHubState == o.ancHubState &&
             coreStates == o.coreStates && ancStates == o.ancStates &&
             ancStateVers == o.ancStateVers &&
             ancCarrierSites == o.ancCarrierSites;
    }
  };
  struct WeakAncPostHash {
    std::size_t operator()(const WeakAncPostKey &k) const {
      std::uint64_t h = 14695981039346656037ULL;
      h = hashMix(h, k.univBuild);
      h = hashMix(h, k.entryUnkRefVer);
      h = hashMix(h, k.hubStateVer);
      h = hashMix(h, (static_cast<std::uint64_t>(k.isGlobal ? 1U : 0U) << 8) |
                         k.ancHubState);
      h = hashMix(h, k.scopeRoot);
      for (std::uint64_t x : k.coreStates)
        h = hashMix(h, x);
      for (std::uint64_t x : k.ancStates)
        h = hashMix(h, x);
      for (std::uint64_t x : k.ancStateVers)
        h = hashMix(h, x);
      for (CallSiteID s : k.ancCarrierSites)
        h = hashMix(h, s);
      return static_cast<std::size_t>(h);
    }
  };
  std::vector<WeakAncestorContext> weakAncCtxs; // 1-indexed
  std::vector<std::uint64_t> weakAncCtxDirtyStamp;
  std::vector<std::vector<std::uint32_t>> funcWeakAncCtxs; // size numFuncs + 1
  std::vector<std::vector<std::uint32_t>> funcWeakAncCoreWatch;
  std::vector<std::uint32_t> objWeakAncCtx; // size numObjects
  std::unordered_map<std::vector<NodeID>, NodeID, NodeVecHashImpl>
      weakAncSrcJoinCache;
  std::unordered_map<std::vector<NodeID>, NodeID, NodeVecHashImpl>
      weakAncSnkJoinCache;
  std::unordered_map<WeakAncSummaryKey, std::uint32_t, WeakAncSummaryHash>
      weakAncSummaryIntern;
  std::uint32_t nextWeakAncSummaryId = 1;
  struct WeakAncPreEntry {
    std::uint32_t ctxId = 0;
    std::vector<FunctionID> coreFuncs;
    std::vector<std::uint8_t> coreIsMod;
    std::vector<std::uint8_t> coreIsRef;
    bool coreHasHub = false;
    bool coreHubMod = false;
    bool coreHubRef = false;
  };
  std::unordered_map<WeakAncPreKey, WeakAncPreEntry, WeakAncPreHash>
      weakAncPreCache;
  std::unordered_map<WeakAncPostKey, std::uint32_t, WeakAncPostHash>
      weakAncPostCache;
  UnivExtSummary hubExtSummary;
  std::uint64_t hubExtBuiltGlobalVer = 0;
  std::uint64_t hubExtBuiltUnivBuild = 0;
  bool anyUnresolvedIndirectCall = false;

  // Per-function CFG and dominance info (built lazily once per reachable fn).
  using LocalBlock = std::uint32_t;
  static constexpr LocalBlock InvalidLocal =
      std::numeric_limits<LocalBlock>::max();

  struct FuncCFG {
    bool built = false;
    bool sccBuilt = false;
    LocalBlock exitLocal = InvalidLocal;
    std::vector<BlockID> localToGlobal;
    std::vector<std::vector<LocalBlock>> succs;
    std::vector<std::vector<LocalBlock>> preds;
    std::vector<LocalBlock> idom;
    std::vector<std::vector<LocalBlock>> df;
    std::vector<bool> selfLoop;
    std::vector<bool> canReachExit;

    // Block-level SCC condensation for weak-only fast path.
    std::vector<std::size_t> comp;
    std::vector<std::vector<LocalBlock>> compBlocks;
    std::vector<bool> compCyclic;
    std::vector<NodeSet> compPreds;

    // Generation-stamped scratch arrays (avoids O(|CFG_f|) allocation per
    // object).
    std::uint64_t gen = 0;
    std::vector<std::uint64_t> defBlockGen;
    std::vector<std::uint64_t> idfGen;
    std::vector<std::uint64_t> memoGen;
    std::vector<std::uint32_t> phiSparse;
    std::vector<std::uint32_t> lastStoreSparse;
    std::vector<std::uint32_t> memoExitDef;

    std::uint64_t weakGen = 0;
    std::vector<std::uint64_t> weakNeededGen;
    std::vector<std::uint32_t> weakActiveIdx;
    std::vector<NodeID> weakFlowNode;
  };

  std::vector<FuncCFG> funcCFGs;
  std::vector<bool> funcReachable;
  std::vector<bool> blockReachable;
  std::vector<LocalBlock> blockToLocal;
  std::vector<std::uint32_t> accessEventPos;
  std::vector<std::uint32_t> callEventPos;
  std::vector<std::vector<CallSiteID>> funcCallSites;
  std::vector<bool> funcHasUnresolvedIndirectCall;

  // Dynamic call graph state (preserved across strong-mode restarts).
  std::vector<std::set<FunctionID>> liveCallTargets;
  std::vector<bool> liveCallExternal;
  std::vector<bool> siteCallsUnknownHub;
  std::vector<CallSiteID> unknownHubCallerSites;
  bool unknownHubActive = false;
  bool unknownHubHasVoidReturn = false;
  std::vector<NodeID> unknownHubArgNodes;
  NodeID unknownHubRetNode = 0;
  std::vector<std::set<FunctionID>> callers;
  std::vector<std::set<CallSiteID>> callerSites;
  std::vector<bool> funcRecursive;
  std::vector<FunctionID> addressTakenFuncs;
  std::vector<bool> isAddressTakenFunc;
  PointsToSet funcObjectSet;
  PointsToSet nonAddrTakenFuncObjSet;
  PointsToSet readOnlyWithInitSet;
  std::vector<std::uint8_t> siteIndirectSaturated;
  mutable std::unordered_map<CallSiteID, std::set<FunctionID>>
      resolvedTargetsCache;
  std::set<CallSiteID> pendingIndirectSites;
  std::vector<std::set<FunctionID>> persistentCallTargets;
  std::vector<bool> persistentCallExternal;
  std::vector<bool> persistentCallUnknownHub;
  std::uint64_t cgGen = 0;
  std::vector<std::uint64_t> cgVisitGen;
  std::vector<std::uint64_t> cgAncGen;

  struct Signature {
    NodeSet accesses;
    NodeSet strong;
    bool operator==(const Signature &other) const {
      return accesses == other.accesses && strong == other.strong;
    }
  };
  std::vector<Signature> previous;

  // Cross-object template cache for leaf/callsite-free per-function flows.
  struct LocalTemplateKey {
    FunctionID function = 0;
    NodeSet accesses;
    NodeSet strong;
    bool operator==(const LocalTemplateKey &other) const {
      return function == other.function && accesses == other.accesses &&
             strong == other.strong;
    }
  };
  struct LocalTemplateHash {
    std::size_t operator()(const LocalTemplateKey &key) const {
      std::uint64_t h = 14695981039346656037ULL;
      h = hashMix(h, key.function);
      h = hashNodeSet(h, key.accesses);
      h = hashNodeSet(h, key.strong);
      return static_cast<std::size_t>(h);
    }
  };
  std::unordered_map<LocalTemplateKey, NodeID, LocalTemplateHash> flowTemplates;

  // Whole-object immutable weak interprocedural flow cache: weak objects with
  // object-independent callsite carry semantics (`globalVisible`, or
  // `!globalVisible && carrierMarkedSites.empty() && scopeRoot == 0`) sharing
  // identical `(cgVer, alwaysVer, unkVer, univBuild, isGlobal, sig.accesses)`
  // share a single immutable interprocedural VFG subgraph.
  struct GlobalWeakFlowKey {
    std::uint64_t cgVer = 0;
    std::uint64_t alwaysVer = 0;
    std::uint64_t unkVer = 0;
    std::uint64_t univBuild = 0;
    bool isGlobal = false;
    NodeSet accesses;
    bool operator==(const GlobalWeakFlowKey &other) const {
      return cgVer == other.cgVer && alwaysVer == other.alwaysVer &&
             unkVer == other.unkVer && univBuild == other.univBuild &&
             isGlobal == other.isGlobal && accesses == other.accesses;
    }
  };
  struct GlobalWeakFlowHash {
    std::size_t operator()(const GlobalWeakFlowKey &k) const {
      std::uint64_t h = 14695981039346656037ULL;
      h = hashMix(h, k.cgVer);
      h = hashMix(h, k.alwaysVer);
      h = hashMix(h, k.unkVer);
      h = hashMix(h, k.univBuild);
      h = hashMix(h, k.isGlobal ? 1ULL : 0ULL);
      h = hashNodeSet(h, k.accesses);
      return static_cast<std::size_t>(h);
    }
  };
  struct GlobalWeakFlowEntry {
    NodeID rootInitNode = 0;
    std::vector<FunctionID> activeFuncs;
    std::uint32_t weakAncCtxId = 0;
  };
  std::unordered_map<GlobalWeakFlowKey, GlobalWeakFlowEntry, GlobalWeakFlowHash>
      globalWeakFlowCache;

  // Per-object, per-function incremental reuse cache (paper Section 6, p. 9):
  // when an object is recomputed, functions whose local accesses and active
  // callsite summary did not change reuse their previous entry/exit nodes.
  struct CallFlowSummary {
    CallSiteID site = 0;
    bool hasIn = false;
    bool hasOut = false;
    bool outStrong = false;
    bool hasHub = false;
    bool operator==(const CallFlowSummary &other) const {
      return site == other.site && hasIn == other.hasIn &&
             hasOut == other.hasOut && outStrong == other.outStrong &&
             hasHub == other.hasHub;
    }
  };
  struct UnknownCallMod {
    CallSiteID site = 0;
    NodeID summaryNode = 0;
    bool operator==(const UnknownCallMod &other) const {
      return site == other.site && summaryNode == other.summaryNode;
    }
  };
  struct ObjectFuncCacheEntry {
    NodeSet accesses;
    NodeSet strong;
    std::vector<CallFlowSummary> calls;
    std::vector<UnknownCallMod> unknownMods;
    std::vector<UnknownCallMod> unknownReads;
    bool needExit = false;
    bool elidedWrappers = false;
    NodeID entryNode = 0;
    NodeID exitNode = 0;
    std::unordered_map<CallSiteID, NodeID> callInNodes;
    std::unordered_map<CallSiteID, NodeID> callOutNodes;
  };
  std::vector<std::unordered_map<FunctionID, ObjectFuncCacheEntry>>
      objectFuncCache;
  struct LastEmitFuncState {
    FunctionID fn = 0;
    std::uint64_t univStructVer = 0;
    NodeID univNode = 0;
    NodeSet accesses;
    NodeSet strong;
    std::size_t unkAccessCount = 0;
    std::vector<CallFlowSummary> calls;
    std::vector<std::size_t> callTargetCounts;
    std::vector<UnknownCallMod> unknownMods;
    std::vector<UnknownCallMod> unknownReads;
    std::vector<std::uint64_t> univExtKeys;
    bool needExit = false;
    bool operator==(const LastEmitFuncState &o) const {
      return fn == o.fn && univStructVer == o.univStructVer &&
             accesses == o.accesses && strong == o.strong &&
             unkAccessCount == o.unkAccessCount && calls == o.calls &&
             callTargetCounts == o.callTargetCounts &&
             unknownMods == o.unknownMods && unknownReads == o.unknownReads &&
             univExtKeys == o.univExtKeys && needExit == o.needExit;
    }
  };
  struct LastEmitState {
    bool valid = false;
    bool globalVisible = false;
    bool objCanStrong = false;
    bool hubActive = false;
    FunctionID scopeRoot = 0;
    std::size_t reachableAddrTakenCount = 0;
    std::uint64_t hubUnknownVersion = 0;
    std::size_t entryUnknownRefCount = 0;
    std::uint32_t weakAncSummaryId = 0;
    std::vector<LastEmitFuncState> funcs;
  };
  std::vector<LastEmitState> lastEmitState;
  // Track which functions each processed object previously wired into, so
  // dynamic call-graph growth can mark affected processed objects dirty.
  std::vector<NodeSet> funcAccessedByObjects;

  bool canStrongUpdate(ObjectID object) const {
    return !weak && effectiveSingleton[object] && !forcedWeak[object];
  }

  NodeID findRep(NodeID u) const {
    NodeID r = u;
    while (vfgRep[r] != r)
      r = vfgRep[r];
    while (vfgRep[u] != r) {
      const NodeID parent = vfgRep[u];
      vfgRep[u] = r;
      u = parent;
    }
    return r;
  }

  const PointsToSet &ptsOfRep(NodeID rep) const {
    return ptsPool[ptsIdx[rep]];
  }

  const PointsToSet &ptsOf(NodeID u) const {
    return ptsPool[ptsIdx[findRep(u)]];
  }

  std::uint32_t allocPtsSlot() {
    if (!freePtsIds.empty()) {
      const std::uint32_t id = freePtsIds.back();
      freePtsIds.pop_back();
      ptsPool[id].clear();
      ptsRefs[id] = 1;
      ptsDrained[id] = 0;
      ptsReadOnlyJoin[id] = 0;
      return id;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(ptsPool.size());
    ptsPool.emplace_back();
    ptsRefs.push_back(1);
    ptsDrained.push_back(0);
    ptsReadOnlyJoin.push_back(0);
    return id;
  }

  void releasePtsSlot(std::uint32_t id) {
    if (id == 0)
      return;
    if (--ptsRefs[id] == 0) {
      PointsToSet().swap(ptsPool[id]);
      ptsDrained[id] = 0;
      ptsReadOnlyJoin[id] = 0;
      freePtsIds.push_back(id);
    }
  }

  void setPtsIdx(NodeID rep, std::uint32_t newId) {
    const std::uint32_t oldId = ptsIdx[rep];
    if (oldId == newId)
      return;
    if (newId != 0)
      ++ptsRefs[newId];
    ptsIdx[rep] = newId;
    releasePtsSlot(oldId);
  }

  PointsToSet &mutablePts(NodeID rep) {
    const std::uint32_t id = ptsIdx[rep];
    if (id == 0) {
      const std::uint32_t nid = allocPtsSlot();
      ptsIdx[rep] = nid;
      return ptsPool[nid];
    }
    if (ptsRefs[id] == 1) {
      ptsDrained[id] = 0;
      ptsReadOnlyJoin[id] = 0;
      return ptsPool[id];
    }
    const std::uint32_t nid = allocPtsSlot();
    ptsPool[nid] = ptsPool[id];
    --ptsRefs[id];
    ptsIdx[rep] = nid;
    return ptsPool[nid];
  }

  bool insertBitRep(NodeID rep, ObjectID obj) {
    const std::uint32_t id = ptsIdx[rep];
    if (id != 0 && ptsPool[id].count(obj))
      return false;
    return mutablePts(rep).insertBit(obj);
  }

  // Unite `src` (never a reference into `ptsPool`) into `rep`. When
  // `needDiff` is true, sets `diffPtr` to point to the newly added bits (either
  // `&src` with zero copy when `rep` was empty, or `&diff`). If
  // `fullSrcId != 0`, `src == ptsPool[fullSrcId]`, enabling O(1) sharing.
  bool uniteRepWithDiff(NodeID rep, const PointsToSet &src,
                        std::uint32_t fullSrcId, bool needDiff,
                        PointsToSet &diff, const PointsToSet *&diffPtr) {
    if (src.empty()) {
      diffPtr = nullptr;
      return false;
    }
    const std::uint32_t id = ptsIdx[rep];
    if (fullSrcId != 0 && id == fullSrcId) {
      diffPtr = nullptr;
      return false;
    }
    if (id == 0) {
      if (fullSrcId != 0) {
        setPtsIdx(rep, fullSrcId);
      } else {
        const std::uint32_t nid = allocPtsSlot();
        ptsPool[nid] = src;
        ptsIdx[rep] = nid;
      }
      diffPtr = &src;
      return true;
    }
    if (ptsRefs[id] > 1) {
      if (src.isSubsetOf(ptsPool[id])) {
        diffPtr = nullptr;
        return false;
      }
      if (fullSrcId != 0 && ptsPool[id].isSubsetOf(src)) {
        if (ptsPool[id].size() == src.size()) {
          setPtsIdx(rep, fullSrcId);
          diffPtr = nullptr;
          return false;
        }
        if (needDiff) {
          diff = src;
          diff.subtract(ptsPool[id]);
          diffPtr = &diff;
        }
        setPtsIdx(rep, fullSrcId);
        return true;
      }
    } else if (fullSrcId != 0 && src.size() >= 32 &&
               ptsPool[id].size() <= src.size() &&
               ptsPool[id].isSubsetOf(src)) {
      if (ptsPool[id].size() == src.size()) {
        setPtsIdx(rep, fullSrcId);
        diffPtr = nullptr;
        return false;
      }
      if (needDiff) {
        diff = src;
        diff.subtract(ptsPool[id]);
        diffPtr = &diff;
      }
      setPtsIdx(rep, fullSrcId);
      return true;
    }
    if (!needDiff) {
      diffPtr = nullptr;
      return mutablePts(rep).unite(src);
    }
    diffPtr = &diff;
    return mutablePts(rep).uniteWithDiff(src, diff);
  }

  bool uniteRepWithDiffById(NodeID rep, std::uint32_t srcPoolId, bool needDiff,
                            PointsToSet &diff) {
    if (srcPoolId == 0) {
      diff.clear();
      return false;
    }
    const std::uint32_t id = ptsIdx[rep];
    if (id == srcPoolId) {
      diff.clear();
      return false;
    }
    if (id == 0) {
      setPtsIdx(rep, srcPoolId);
      if (needDiff)
        diff = ptsPool[srcPoolId];
      return true;
    }
    if (ptsPool[srcPoolId].isSubsetOf(ptsPool[id])) {
      diff.clear();
      return false;
    }
    if (ptsPool[id].size() <= ptsPool[srcPoolId].size() &&
        ptsPool[id].isSubsetOf(ptsPool[srcPoolId])) {
      if (ptsPool[id].size() == ptsPool[srcPoolId].size()) {
        setPtsIdx(rep, srcPoolId);
        diff.clear();
        return false;
      }
      if (needDiff) {
        diff = ptsPool[srcPoolId];
        diff.subtract(ptsPool[id]);
      }
      setPtsIdx(rep, srcPoolId);
      return true;
    }
    mutablePts(rep);
    // Re-index `ptsPool` after `mutablePts` in case `ptsPool` reallocated.
    if (!needDiff)
      return ptsPool[ptsIdx[rep]].unite(ptsPool[srcPoolId]);
    return ptsPool[ptsIdx[rep]].uniteWithDiff(ptsPool[srcPoolId], diff);
  }

  // Hash-cons large PointsToSets by hashing each distinct live pool slot ONCE
  // (O(|live slots|) instead of O(|nodes| * |set|)), then remapping node IDs.
  void hashConsLargePts() {
    const std::size_t nSlots = ptsPool.size();
    std::vector<std::uint32_t> slotMap(nSlots, 0);
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> seen;
    seen.reserve(nSlots / 2 + 16);
    bool anyMerged = false;
    for (std::uint32_t id = 1; id < nSlots; ++id) {
      slotMap[id] = id;
      if (ptsRefs[id] == 0 || ptsPool[id].size() < 32)
        continue;
      const std::uint64_t h = ptsPool[id].hash();
      auto &bucket = seen[h];
      std::uint32_t canonical = 0;
      for (std::uint32_t cand : bucket) {
        if (ptsPool[cand] == ptsPool[id]) {
          canonical = cand;
          break;
        }
      }
      if (canonical == 0) {
        bucket.push_back(id);
      } else {
        slotMap[id] = canonical;
        anyMerged = true;
      }
    }
    if (!anyMerged)
      return;
    const NodeID numNodes = static_cast<NodeID>(adj.size());
    for (NodeID u = 1; u < numNodes; ++u) {
      if (vfgRep[u] != u)
        continue;
      const std::uint32_t id = ptsIdx[u];
      if (id != 0 && slotMap[id] != id)
        setPtsIdx(u, slotMap[id]);
    }
  }

  void markDirtySig(ObjectID obj) {
    if (!dirtySigMark[obj]) {
      dirtySigMark[obj] = 1;
      dirtySigList.push_back(obj);
    }
  }

  void clearDirtySigs() {
    for (ObjectID obj : dirtySigList)
      dirtySigMark[obj] = 0;
    dirtySigList.clear();
  }

  void enableNodeHook(NodeID v) {
    if (!nodeHasHook[v]) {
      nodeHasHook[v] = true;
      const NodeID r = findRep(v);
      if (r != v)
        sccHookNodes[r].push_back(v);
    }
  }

  NodeID addJoin() {
    if (localCap.active)
      return localCap.allocNode();
    const NodeID node = nextID(adj.size());
    adj.emplace_back();
    vfgRep.push_back(node);
    propAdj.emplace_back();
    sccHookNodes.emplace_back();
    repHasUnknownStoreHook.push_back(0);
    repHasExternalEscapeHook.push_back(0);
    repHasCallBoundaryHook.push_back(0);
    ptsIdx.push_back(0);
    nodeTopoRank.push_back(0);
    propDeltaSlot.push_back(0);
    propDeltaPoolId.push_back(0);
    propNeedsHook.push_back(0);
    ++stats.valueFlowJoinNodes;
    return node;
  }

  // Lazily build a single deduplicated join node uniting the `initialStore`s
  // of a subset of non-empty read-only objects so distinct `ptsPool` slots or
  // incremental diffs sharing the same read-only subset never duplicate the
  // `initialStore -> join` fan-in edges.
  NodeID getOrCreateReadOnlySourceJoin(const PointsToSet &roSubset) {
    if (roSubset.empty())
      return kNoReadOnlyJoin;
    if (roSubset.size() == 1) {
      const ObjectID onlyObj = *roSubset.begin();
      emittedFlow[onlyObj] = true;
      return p.objects_[onlyObj].initialStore;
    }
    auto it = readOnlySourceJoinCache.find(roSubset);
    if (it != readOnlySourceJoinCache.end())
      return it->second;
    const NodeID jn = addJoin();
    readOnlySourceJoinCache.emplace(roSubset, jn);
    if (!lastBigROSubset.empty() && lastBigROJoin &&
        roSubset.size() >= lastBigROSubset.size() &&
        lastBigROSubset.isSubsetOf(roSubset)) {
      addIndirect(lastBigROJoin, jn);
      roSubset.forEachNotIn(lastBigROSubset, [&](ObjectID obj) {
        addIndirect(p.objects_[obj].initialStore, jn);
        emittedFlow[obj] = true;
      });
    } else {
      for (ObjectID obj : roSubset) {
        addIndirect(p.objects_[obj].initialStore, jn);
        emittedFlow[obj] = true;
      }
    }
    if (roSubset.size() >= 64 && roSubset.size() >= lastBigROSubset.size()) {
      lastBigROSubset = roSubset;
      lastBigROJoin = jn;
    }
    return jn;
  }

  NodeID getOrCreatePoolReadOnlyJoin(std::uint32_t poolId) {
    if (poolId == 0)
      return kNoReadOnlyJoin;
    if (ptsReadOnlyJoin[poolId] != 0)
      return ptsReadOnlyJoin[poolId];
    if (readOnlyWithInitSet.empty()) {
      ptsReadOnlyJoin[poolId] = kNoReadOnlyJoin;
      return kNoReadOnlyJoin;
    }
    PointsToSet roSub;
    ptsPool[poolId].forEachIn(readOnlyWithInitSet,
                              [&](ObjectID obj) { roSub.insertBit(obj); });
    const NodeID jn = getOrCreateReadOnlySourceJoin(roSub);
    ptsReadOnlyJoin[poolId] = jn;
    return jn;
  }

  bool hookNeedsPerNode(NodeID h, bool hasUnk) const {
    if (!hasUnk && initializedObject[h] != 0)
      return true;
    const NodeID stPtr = storedThrough[h];
    if (!hasUnk && stPtr != 0 && !ptrHasUnknownTarget[stPtr])
      return true;
    if (!ptrEscapedBy[h].empty())
      return true;
    if (!uses[h].empty())
      return true;
    for (CallSiteID site : indirectCallUses[h]) {
      if (!siteIndirectSaturated[site])
        return true;
    }
    if (!hasUnk && !indirectArgUses[h].empty()) {
      for (CallSiteID site : indirectArgUses[h]) {
        if (liveCallTargets[site].empty() && !liveCallExternal[site] &&
            !siteCallsUnknownHub[site])
          return true;
      }
    }
    if (isCallBoundaryNode[h]) {
      for (CallSiteID site : callBoundarySites[h]) {
        if (!siteUnknownBoundaryMarked[site])
          return true;
      }
    }
    return false;
  }

  // Scan SCC-level global drain flags and compact `sccHookNodes[rep]`.
  void compactRepHooks(NodeID rep) {
    const NodeID numProg = static_cast<NodeID>(p.edges_.size());
    auto scanNodeFlags = [&](NodeID h) {
      if (isExternalEscapeNode[h])
        repHasExternalEscapeHook[rep] = 1;
      if (isCallBoundaryNode[h])
        repHasCallBoundaryHook[rep] = 1;
      if (const ObjectID initObj = initializedObject[h]) {
        if (initObj == p.unknownObject() || objEscapedToUnknown[initObj])
          repHasUnknownStoreHook[rep] = 1;
      }
      if (const NodeID stPtr = storedThrough[h]) {
        if (!ptrHasUnknownTarget[stPtr] &&
            ptsOf(stPtr).count(p.unknownObject())) {
          ptrHasUnknownTarget[stPtr] = true;
          ptrHasExternalTarget[stPtr] = true;
          for (ObjectID c : ptrEscapedBy[stPtr])
            markEscapedToUnknown(c);
          ptrEscapedBy[stPtr].clear();
        }
        if (ptrHasUnknownTarget[stPtr])
          repHasUnknownStoreHook[rep] = 1;
      }
    };
    if (rep < numProg && nodeHasHook[rep])
      scanNodeFlags(rep);
    for (NodeID h : sccHookNodes[rep])
      scanNodeFlags(h);

    const bool hasUnk = (repHasUnknownStoreHook[rep] != 0);
    if (!sccHookNodes[rep].empty()) {
      auto &hooks = sccHookNodes[rep];
      std::size_t keep = 0;
      for (NodeID h : hooks) {
        if (hookNeedsPerNode(h, hasUnk))
          hooks[keep++] = h;
      }
      hooks.resize(keep);
      if (hooks.empty())
        std::vector<NodeID>().swap(hooks);
    }
  }

  bool insertPropAdj(NodeID from, NodeID to) {
    from = findRep(from);
    to = findRep(to);
    if (from == to)
      return false;
    auto &list = propAdj[from];
    if (list.size() < 16) {
      for (NodeID existing : list)
        if (existing == to)
          return false;
      list.push_back(to);
    } else {
      auto it = largePropAdjSets.find(from);
      if (it == largePropAdjSets.end()) {
        NodeSet set;
        for (NodeID existing : list)
          set.insertBit(existing);
        it = largePropAdjSets.emplace(from, std::move(set)).first;
      }
      if (!it->second.insertBit(to))
        return false;
      list.push_back(to);
    }
    if (topoRanksValid) {
      if (nodeTopoRank[to] == 0 && propAdj[to].empty() &&
          nodeTopoRank[from] > 1) {
        nodeTopoRank[to] = nodeTopoRank[from] - 1;
      } else if (nodeTopoRank[from] <= nodeTopoRank[to]) {
        pendingBackEdges.emplace_back(from, to);
      }
    }
    ++edgesSinceCondense;
    return true;
  }

  bool insertAdj(NodeID from, NodeID to) {
    auto &list = adj[from];
    bool inserted = false;
    if (list.size() < 16) {
      bool found = false;
      for (NodeID existing : list) {
        if (existing == to) {
          found = true;
          break;
        }
      }
      if (!found) {
        list.push_back(to);
        inserted = true;
      }
    } else {
      auto it = largeAdjSets.find(from);
      if (it == largeAdjSets.end()) {
        NodeSet set;
        for (NodeID existing : list)
          set.insertBit(existing);
        it = largeAdjSets.emplace(from, std::move(set)).first;
      }
      if (it->second.insertBit(to)) {
        list.push_back(to);
        inserted = true;
      }
    }
    if (inserted)
      insertPropAdj(from, to);
    return inserted;
  }

  // Collapse one multi-node SCC `members` into a single representative `rep`,
  // merging points-to sets, hook lists, pending hook diffs, and outgoing
  // `propAdj`. When `incremental` is true, assigns `nodeTopoRank[rep]` to the
  // minimum member rank (preserving the exact `pendingBackEdges` invariant
  // without scanning incoming external edges) and records any newly inverted
  // outgoing edges of `rep`.
  NodeID mergeVFGSCC(const std::vector<NodeID> &members, bool incremental) {
    const NodeID numProg = static_cast<NodeID>(p.edges_.size());
    NodeID rep = members[0];
    std::size_t bestSize = ptsOfRep(rep).size();
    std::uint64_t minRank = nodeTopoRank[rep];
    for (NodeID m : members) {
      const std::size_t sz = ptsOfRep(m).size();
      if (sz > bestSize) {
        bestSize = sz;
        rep = m;
      }
      if (nodeTopoRank[m] < minRank)
        minRank = nodeTopoRank[m];
    }
    const std::uint32_t bestId = ptsIdx[rep];
    bool allSameOrSubset = (bestId != 0);
    PointsToSet unionPts;
    if (bestId != 0) {
      for (NodeID m : members) {
        const std::uint32_t mId = ptsIdx[m];
        if (mId != 0 && mId != bestId &&
            !ptsPool[mId].isSubsetOf(ptsPool[bestId])) {
          allSameOrSubset = false;
          break;
        }
      }
    }
    if (!allSameOrSubset) {
      unionPts = ptsOfRep(rep);
      for (NodeID m : members) {
        if (m != rep && ptsIdx[m] != 0)
          unionPts.unite(ptsOfRep(m));
      }
    }
    const PointsToSet &finalUnion =
        allSameOrSubset ? ptsPool[bestId] : unionPts;

    // Pre-compute post-merge SCC flags so `PendingHookBatch` omits hooks
    // already subsumed by SCC-level global drains.
    bool sccUnkHook = false;
    bool sccExtHook = false;
    bool sccCallVisHook = false;
    for (NodeID m : members) {
      sccUnkHook |= (repHasUnknownStoreHook[m] != 0);
      sccExtHook |= (repHasExternalEscapeHook[m] != 0);
      sccCallVisHook |= (repHasCallBoundaryHook[m] != 0);
      auto scanFlags = [&](NodeID h) {
        if (isExternalEscapeNode[h])
          sccExtHook = true;
        if (isCallBoundaryNode[h])
          sccCallVisHook = true;
        if (const ObjectID initObj = initializedObject[h]) {
          if (initObj == p.unknownObject() || objEscapedToUnknown[initObj])
            sccUnkHook = true;
        }
        if (const NodeID stPtr = storedThrough[h]) {
          if (!ptrHasUnknownTarget[stPtr] &&
              ptsOf(stPtr).count(p.unknownObject())) {
            ptrHasUnknownTarget[stPtr] = true;
            ptrHasExternalTarget[stPtr] = true;
            for (ObjectID child : ptrEscapedBy[stPtr])
              markEscapedToUnknown(child);
            ptrEscapedBy[stPtr].clear();
          }
          if (ptrHasUnknownTarget[stPtr])
            sccUnkHook = true;
        }
      };
      if (m < numProg && nodeHasHook[m])
        scanFlags(m);
      for (NodeID h : sccHookNodes[m])
        scanFlags(h);
    }

    if (!finalUnion.empty()) {
      std::unordered_map<std::uint32_t, std::size_t> idToBatch;
      for (NodeID m : members) {
        const std::uint32_t mId = ptsIdx[m];
        const auto &mPts = ptsPool[mId];
        if (mPts.size() >= finalUnion.size())
          continue;
        const bool mHasHook = (m < numProg && nodeHasHook[m]);
        const bool mHasRepFlags = repHasUnknownStoreHook[m] ||
                                  repHasExternalEscapeHook[m] ||
                                  repHasCallBoundaryHook[m];
        if (!mHasHook && sccHookNodes[m].empty() && !mHasRepFlags)
          continue;
        bool anyKeptNode = (mHasHook && hookNeedsPerNode(m, sccUnkHook));
        if (!anyKeptNode) {
          for (NodeID h : sccHookNodes[m]) {
            if (hookNeedsPerNode(h, sccUnkHook)) {
              anyKeptNode = true;
              break;
            }
          }
        }
        if (!anyKeptNode && !mHasRepFlags && !sccUnkHook && !sccExtHook &&
            !sccCallVisHook)
          continue;
        auto [it, inserted] = idToBatch.emplace(mId, pendingHookDiffs.size());
        if (inserted) {
          PendingHookBatch batch;
          batch.diff = finalUnion;
          if (mId != 0)
            batch.diff.subtract(mPts);
          if (batch.diff.empty()) {
            it->second = std::numeric_limits<std::size_t>::max();
            continue;
          }
          batch.unkEscHook = sccUnkHook;
          batch.extEscHook = sccExtHook;
          batch.callVisHook = sccCallVisHook;
          pendingHookDiffs.push_back(std::move(batch));
        }
        if (it->second == std::numeric_limits<std::size_t>::max())
          continue;
        auto &batch = pendingHookDiffs[it->second];
        if (mHasHook && hookNeedsPerNode(m, sccUnkHook))
          batch.nodes.push_back(m);
        for (NodeID h : sccHookNodes[m]) {
          if (hookNeedsPerNode(h, sccUnkHook))
            batch.nodes.push_back(h);
        }
      }
      if (sccUnkHook || sccExtHook || sccCallVisHook) {
        const std::uint8_t curMask =
            allSameOrSubset ? ptsDrained[bestId] : std::uint8_t(0);
        const bool needUnk = sccUnkHook && !(curMask & 1U);
        const bool needExt = sccExtHook && !(curMask & 2U);
        const bool needVis = sccCallVisHook && !(curMask & 4U);
        if (needUnk || needExt || needVis) {
          PendingHookBatch globalBatch;
          globalBatch.diff = finalUnion;
          globalBatch.unkEscHook = needUnk;
          globalBatch.extEscHook = needExt;
          globalBatch.callVisHook = needVis;
          pendingHookDiffs.push_back(std::move(globalBatch));
        }
      }
    }
    if (sccUnkHook)
      repHasUnknownStoreHook[rep] = 1;
    if (sccExtHook)
      repHasExternalEscapeHook[rep] = 1;
    if (sccCallVisHook)
      repHasCallBoundaryHook[rep] = 1;
    for (NodeID m : members) {
      if (m == rep)
        continue;
      vfgRep[m] = rep;
      ++totalCollapsedNodes;
      if (incremental)
        ++uncompactedCollapsed;
      repHasUnknownStoreHook[rep] |= repHasUnknownStoreHook[m];
      repHasExternalEscapeHook[rep] |= repHasExternalEscapeHook[m];
      repHasCallBoundaryHook[rep] |= repHasCallBoundaryHook[m];
      if (m < numProg && nodeHasHook[m])
        sccHookNodes[rep].push_back(m);
      if (!sccHookNodes[m].empty()) {
        sccHookNodes[rep].insert(sccHookNodes[rep].end(),
                                 sccHookNodes[m].begin(),
                                 sccHookNodes[m].end());
        std::vector<NodeID>().swap(sccHookNodes[m]);
      }
      setPtsIdx(m, 0);
    }
    if (!allSameOrSubset) {
      if (unionPts.empty()) {
        setPtsIdx(rep, 0);
      } else {
        const std::uint32_t nid = allocPtsSlot();
        ptsPool[nid] = std::move(unionPts);
        const std::uint32_t oldId = ptsIdx[rep];
        ptsIdx[rep] = nid;
        releasePtsSlot(oldId);
      }
    }
    if (incremental)
      nodeTopoRank[rep] = minRank;
    compactRepHooks(rep);
    return rep;
  }

  // Exact incremental + full online VFG copy-SCC condensation:
  // 1. If `topoRanksValid && pendingBackEdges.empty()`, provably zero cycles
  //    exist (O(1) skip).
  // 2. When `pendingBackEdges` is small, run rank-window-bounded Tarjan ONLY on
  //    nodes reachable forward from back-edge targets within
  //    `[minSrcRank, maxDstRank]`. Every cycle in the entire graph lies
  //    provably inside this window, collapsing hot cycles immediately in O(|W|)
  //    without scanning all `propAdj` lists.
  // 3. Falls back to full single-pass Tarjan + rank rebalance + `propAdj`
  //    compaction when the window exceeds 4096 nodes, many non-cycle inversions
  //    accumulate, or `uncompactedCollapsed` grows large.
  void condenseVFGSCCs() {
    if (!propWork.empty() || adj.size() < 256)
      return;

    // Fast path 1: provably zero cycles when all edges obey spaced topo ranks.
    if (topoRanksValid && pendingBackEdges.empty()) {
      if (uncompactedCollapsed < 8192) {
        edgesSinceCondense = 0;
        lastCondenseTrv = stats.objectTraversals;
        forceCondense = false;
        return;
      }
    }

    // Fast path 2: rank-bounded incremental SCC collapse on `pendingBackEdges`.
    if (topoRanksValid && !pendingBackEdges.empty() && !forceCondense &&
        uncompactedCollapsed < 8192) {
      // Filter resolved or no-longer-inverted back-edges.
      std::size_t keep = 0;
      std::uint64_t minSrcRank = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t maxDstRank = 0;
      for (const auto &e : pendingBackEdges) {
        const NodeID u = findRep(e.first);
        const NodeID v = findRep(e.second);
        if (u == v || nodeTopoRank[u] > nodeTopoRank[v])
          continue;
        pendingBackEdges[keep++] = {u, v};
        if (nodeTopoRank[u] < minSrcRank)
          minSrcRank = nodeTopoRank[u];
        if (nodeTopoRank[v] > maxDstRank)
          maxDstRank = nodeTopoRank[v];
      }
      pendingBackEdges.resize(keep);
      if (pendingBackEdges.empty()) {
        edgesSinceCondense = 0;
        lastCondenseTrv = stats.objectTraversals;
        return;
      }

      if (pendingBackEdges.size() <= 128 &&
          (edgesSinceCondense >= 128 || pendingBackEdges.size() >= 32)) {
        const std::size_t numNodes = adj.size();
        if (incVisGen.size() < numNodes) {
          incVisGen.resize(numNodes, 0);
          incLocalIdx.resize(numNodes, 0);
        }
        const std::uint64_t wg = ++incSccGen;
        constexpr std::size_t kMaxIncWindow = 4096;
        std::vector<NodeID> wNodes;
        wNodes.reserve(256);
        std::vector<std::uint32_t> disc;
        std::vector<std::uint32_t> low;
        std::vector<std::uint8_t> onStack;
        std::vector<std::uint32_t> tarjanStack;
        struct IncFrame {
          std::uint32_t u;
          std::size_t edgeIdx;
        };
        std::vector<IncFrame> callStack;
        std::vector<std::vector<NodeID>> multiComps;
        bool overflow = false;
        std::uint32_t nextDisc = 0;

        auto allocLocal = [&](NodeID node) -> std::uint32_t {
          const std::uint32_t idx = static_cast<std::uint32_t>(wNodes.size());
          wNodes.push_back(node);
          incVisGen[node] = wg;
          incLocalIdx[node] = idx;
          disc.push_back(nextDisc);
          low.push_back(nextDisc);
          ++nextDisc;
          onStack.push_back(1);
          tarjanStack.push_back(idx);
          return idx;
        };

        for (std::size_t bi = 0; bi < pendingBackEdges.size() && !overflow;
             ++bi) {
          const NodeID startNode = pendingBackEdges[bi].second;
          if (incVisGen[startNode] == wg)
            continue;
          const std::uint32_t sIdx = allocLocal(startNode);
          callStack.push_back({sIdx, 0});
          while (!callStack.empty()) {
            auto &top = callStack.back();
            const NodeID uNode = wNodes[top.u];
            const auto &edges = propAdj[uNode];
            if (top.edgeIdx < edges.size()) {
              const NodeID rv = findRep(edges[top.edgeIdx++]);
              if (rv == uNode)
                continue;
              const std::uint64_t rRank = nodeTopoRank[rv];
              if (rRank < minSrcRank || rRank > maxDstRank)
                continue;
              if (incVisGen[rv] != wg) {
                if (wNodes.size() >= kMaxIncWindow) {
                  overflow = true;
                  break;
                }
                const std::uint32_t j = allocLocal(rv);
                callStack.push_back({j, 0});
              } else {
                const std::uint32_t j = incLocalIdx[rv];
                if (onStack[j] && disc[j] < low[top.u])
                  low[top.u] = disc[j];
              }
            } else {
              const std::uint32_t u = top.u;
              callStack.pop_back();
              if (!callStack.empty()) {
                const std::uint32_t parent = callStack.back().u;
                if (low[u] < low[parent])
                  low[parent] = low[u];
              }
              if (low[u] == disc[u]) {
                std::vector<NodeID> members;
                while (true) {
                  const std::uint32_t w = tarjanStack.back();
                  tarjanStack.pop_back();
                  onStack[w] = 0;
                  members.push_back(wNodes[w]);
                  if (w == u)
                    break;
                }
                if (members.size() > 1)
                  multiComps.push_back(std::move(members));
              }
            }
          }
        }

        if (!overflow) {
          edgesSinceCondense = 0;
          lastCondenseTrv = stats.objectTraversals;
          if (!multiComps.empty()) {
            std::vector<NodeID> mergedReps;
            mergedReps.reserve(multiComps.size());
            for (const auto &members : multiComps)
              mergedReps.push_back(mergeVFGSCC(members, /*incremental=*/true));
            for (std::size_t ci = 0; ci < multiComps.size(); ++ci) {
              const auto &members = multiComps[ci];
              const NodeID rep = findRep(mergedReps[ci]);
              const std::uint64_t repRank = nodeTopoRank[rep];
              std::vector<NodeID> out;
              for (NodeID m : members) {
                for (NodeID v : propAdj[m]) {
                  const NodeID rv = findRep(v);
                  if (rv != rep)
                    out.push_back(rv);
                }
                largePropAdjSets.erase(m);
                if (m != rep)
                  std::vector<NodeID>().swap(propAdj[m]);
              }
              std::sort(out.begin(), out.end());
              out.erase(std::unique(out.begin(), out.end()), out.end());
              for (NodeID rv : out) {
                if (repRank <= nodeTopoRank[rv])
                  pendingBackEdges.emplace_back(rep, rv);
              }
              propAdj[rep] = std::move(out);
              if (ptsIdx[rep] != 0 && !propAdj[rep].empty())
                enqueueDeltaPoolId(rep, ptsIdx[rep], /*fireHook=*/false);
            }
            // Drop any back-edges whose endpoints merged.
            std::size_t kept = 0;
            for (const auto &e : pendingBackEdges) {
              const NodeID u = findRep(e.first);
              const NodeID v = findRep(e.second);
              if (u != v && nodeTopoRank[u] <= nodeTopoRank[v])
                pendingBackEdges[kept++] = {u, v};
            }
            pendingBackEdges.resize(kept);
          }
          return;
        }
      }
    }

    if (!forceCondense) {
      if (edgesSinceCondense < 512 && uncompactedCollapsed < 8192)
        return;
      const std::size_t edgeThresh = std::min<std::size_t>(
          16384, std::max<std::size_t>(4096, adj.size() / 16));
      const std::size_t trvThresh = std::min<std::size_t>(
          100000, std::max<std::size_t>(50000, adj.size() / 4));
      if (edgesSinceCondense < edgeThresh &&
          stats.objectTraversals < lastCondenseTrv + trvThresh &&
          uncompactedCollapsed < 8192)
        return;
    }
    forceCondense = false;

    const NodeID numNodes = static_cast<NodeID>(adj.size());
    std::vector<NodeID> reps;
    std::vector<std::uint32_t> repToIdx(
        numNodes, std::numeric_limits<std::uint32_t>::max());
    reps.reserve(numNodes - totalCollapsedNodes);
    for (NodeID u = 1; u < numNodes; ++u) {
      if (vfgRep[u] == u) {
        repToIdx[u] = static_cast<std::uint32_t>(reps.size());
        reps.push_back(u);
      }
    }
    const std::size_t nReps = reps.size();

    // Full iterative Tarjan's SCC directly over `propAdj[reps[i]]`.
    constexpr std::uint32_t kUnvisited = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> disc(nReps, kUnvisited);
    std::vector<std::uint32_t> low(nReps, 0);
    std::vector<std::uint32_t> comp(nReps, 0);
    std::vector<std::uint8_t> onStack(nReps, 0);
    std::vector<std::uint32_t> tarjanStack;
    tarjanStack.reserve(nReps);
    struct Frame {
      std::uint32_t u;
      std::size_t edgeIdx;
    };
    std::vector<Frame> callStack;
    callStack.reserve(1024);

    std::uint32_t nextDisc = 0;
    std::uint32_t numComps = 0;
    for (std::uint32_t start = 0; start < nReps; ++start) {
      if (disc[start] != kUnvisited)
        continue;
      disc[start] = low[start] = nextDisc++;
      tarjanStack.push_back(start);
      onStack[start] = 1;
      callStack.push_back({start, 0});
      while (!callStack.empty()) {
        auto &top = callStack.back();
        const NodeID uNode = reps[top.u];
        const auto &edges = propAdj[uNode];
        if (top.edgeIdx < edges.size()) {
          const NodeID rv = findRep(edges[top.edgeIdx++]);
          if (rv == uNode)
            continue;
          const std::uint32_t j = repToIdx[rv];
          if (disc[j] == kUnvisited) {
            disc[j] = low[j] = nextDisc++;
            tarjanStack.push_back(j);
            onStack[j] = 1;
            callStack.push_back({j, 0});
          } else if (onStack[j]) {
            if (disc[j] < low[top.u])
              low[top.u] = disc[j];
          }
        } else {
          const std::uint32_t u = top.u;
          callStack.pop_back();
          if (!callStack.empty()) {
            const std::uint32_t parent = callStack.back().u;
            if (low[u] < low[parent])
              low[parent] = low[u];
          }
          if (low[u] == disc[u]) {
            while (true) {
              const std::uint32_t w = tarjanStack.back();
              tarjanStack.pop_back();
              onStack[w] = 0;
              comp[w] = numComps;
              if (w == u)
                break;
            }
            ++numComps;
          }
        }
      }
    }

    std::vector<std::size_t> compSize(numComps, 0);
    for (std::size_t i = 0; i < nReps; ++i) {
      ++compSize[comp[i]];
      // Spaced sink-to-source topological ranks: every DAG edge u -> v has
      // `nodeTopoRank[u] > nodeTopoRank[v]` with 2^20 slack for fresh join
      // nodes added before the next rebalance.
      nodeTopoRank[reps[i]] = (static_cast<std::uint64_t>(comp[i]) + 1ULL)
                              << 20;
    }

    bool anyMerged = false;
    for (std::size_t c = 0; c < numComps; ++c) {
      if (compSize[c] > 1) {
        anyMerged = true;
        break;
      }
    }
    edgesSinceCondense = 0;
    lastCondenseTrv = stats.objectTraversals;
    pendingBackEdges.clear();
    topoRanksValid = true;
    const bool needCompact = (uncompactedCollapsed > 0);
    uncompactedCollapsed = 0;

    if (!anyMerged && !needCompact) {
      hashConsLargePts();
      return;
    }

    std::vector<std::vector<NodeID>> compMembers(numComps);
    if (anyMerged) {
      for (std::size_t i = 0; i < nReps; ++i) {
        if (compSize[comp[i]] > 1)
          compMembers[comp[i]].push_back(reps[i]);
      }
      for (std::size_t c = 0; c < numComps; ++c) {
        if (compMembers[c].size() > 1)
          mergeVFGSCC(compMembers[c], /*incremental=*/false);
      }
      for (std::size_t c = 0; c < numComps; ++c) {
        const auto &members = compMembers[c];
        if (members.size() <= 1)
          continue;
        const NodeID rep = findRep(members[0]);
        std::vector<NodeID> out;
        for (NodeID m : members) {
          for (NodeID v : propAdj[m]) {
            const NodeID rv = findRep(v);
            if (rv != rep)
              out.push_back(rv);
          }
          largePropAdjSets.erase(m);
          if (m != rep)
            std::vector<NodeID>().swap(propAdj[m]);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        propAdj[rep] = std::move(out);
        if (ptsIdx[rep] != 0 && !propAdj[rep].empty())
          enqueueDeltaPoolId(rep, ptsIdx[rep], /*fireHook=*/false);
      }
    }
    for (std::size_t i = 0; i < nReps; ++i) {
      if (compSize[comp[i]] > 1)
        continue;
      const NodeID u = reps[i];
      auto &list = propAdj[u];
      bool changed = false;
      for (NodeID &v : list) {
        const NodeID rv = findRep(v);
        if (rv != v) {
          v = rv;
          changed = true;
        }
      }
      if (changed) {
        list.erase(std::remove(list.begin(), list.end(), u), list.end());
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        largePropAdjSets.erase(u);
      }
    }
    hashConsLargePts();
  }

  std::uint32_t acquireDeltaSlot(NodeID node) {
    std::uint32_t &slot = propDeltaSlot[node];
    if (slot == 0) {
      if (!freeDeltaSlots.empty()) {
        slot = freeDeltaSlots.back();
        freeDeltaSlots.pop_back();
        deltaPool[slot - 1].clear();
      } else {
        deltaPool.emplace_back();
        slot = static_cast<std::uint32_t>(deltaPool.size());
      }
      if (const std::uint32_t pid = propDeltaPoolId[node]) {
        propDeltaPoolId[node] = 0;
        deltaPool[slot - 1] = ptsPool[pid];
        releasePtsSlot(pid);
      } else {
        propWork.emplace(nodeTopoRank[node], node);
      }
    }
    return slot;
  }

  void enqueueDeltaPoolId(NodeID node, std::uint32_t poolId,
                          bool fireHook = true) {
    if (poolId == 0)
      return;
    if (fireHook)
      propNeedsHook[node] = 1;
    // Pin already-shared `ptsPool` slots (`ptsRefs >= 2`) so propagation along
    // DAG chains is zero-copy without bumping unique (`ptsRefs == 1`) slots.
    if (propDeltaSlot[node] == 0 && ptsPool[poolId].size() >= 16 &&
        ptsRefs[poolId] >= 2) {
      if (propDeltaPoolId[node] == 0) {
        ++ptsRefs[poolId];
        propDeltaPoolId[node] = poolId;
        propWork.emplace(nodeTopoRank[node], node);
        return;
      }
      if (propDeltaPoolId[node] == poolId)
        return;
    }
    const std::uint32_t slot = acquireDeltaSlot(node);
    deltaPool[slot - 1].unite(ptsPool[poolId]);
  }

  void enqueueDeltaBit(NodeID node, ObjectID obj, bool fireHook = true) {
    const std::uint32_t slot = acquireDeltaSlot(node);
    if (fireHook)
      propNeedsHook[node] = 1;
    deltaPool[slot - 1].insertBit(obj);
  }

  void enqueueDeltaSet(NodeID node, const PointsToSet &diff,
                       bool fireHook = true) {
    const std::uint32_t slot = acquireDeltaSlot(node);
    if (fireHook)
      propNeedsHook[node] = 1;
    deltaPool[slot - 1].unite(diff);
  }

  bool hasPendingPropagation() const {
    return !pendingObjSeeds.empty() || !pendingEdgeSeeds.empty() ||
           !propWork.empty() || !pendingHookDiffs.empty();
  }

  void ensurePted() const {
    if (ptedBuilt)
      return;
    ptedBuilt = true;
    pted.assign(p.objects_.size(), {});
    for (NodeID node = 1; node < p.edges_.size(); ++node) {
      for (ObjectID object : ptsOf(node))
        pted[object].insertBit(node);
    }
  }

  void ensureEdgesCache() const {
    if (edgesCacheBuilt)
      return;
    edgesCacheBuilt = true;
    edgesCache.assign(adj.size(), {});
    for (NodeID u = 1; u < adj.size(); ++u) {
      for (NodeID v : adj[u])
        edgesCache[u].insertBit(v);
    }
  }

  bool siteAlwaysCarries(CallSiteID site) const {
    return liveCallExternal[site] || siteUnknownBoundaryMarked[site];
  }

  void bumpFuncStateVer(FunctionID fn, bool bumpUniv = true) {
    if (fn < fnStateVer.size())
      ++fnStateVer[fn];
    if (bumpUniv && fn < fnUnivScc.size()) {
      if (const std::uint32_t u = fnUnivScc[fn])
        ++univSccs[u].stateVer;
    }
  }

  void markCoreWatchesStale(std::vector<std::uint32_t> &watches) {
    for (std::uint32_t ctxId : watches)
      weakAncCtxs[ctxId].stale = true;
    watches.clear();
  }

  template <typename Fn>
  void forEachAccessedObjectInFunc(FunctionID fn, std::uint64_t dirtyGen,
                                   Fn &&cb) {
    if (fn < funcAccessedByObjects.size()) {
      for (ObjectID obj : funcAccessedByObjects[fn])
        cb(obj);
    }
    if (fn < funcWeakAncCoreWatch.size() && !funcWeakAncCoreWatch[fn].empty())
      markCoreWatchesStale(funcWeakAncCoreWatch[fn]);
    auto visitAncCtxList = [&](std::vector<std::uint32_t> &list) {
      std::size_t keep = 0;
      for (std::size_t i = 0; i < list.size(); ++i) {
        const std::uint32_t ctxId = list[i];
        auto &ctx = weakAncCtxs[ctxId];
        ctx.stale = true;
        if (ctx.accessedByObjects.empty())
          continue; // prune dead stale context
        list[keep++] = ctxId;
        if (weakAncCtxDirtyStamp[ctxId] != dirtyGen) {
          weakAncCtxDirtyStamp[ctxId] = dirtyGen;
          for (ObjectID obj : ctx.accessedByObjects)
            cb(obj);
        }
      }
      list.resize(keep);
    };
    if (fn < funcWeakAncCtxs.size() && !funcWeakAncCtxs[fn].empty())
      visitAncCtxList(funcWeakAncCtxs[fn]);
    if (fn < fnUnivScc.size()) {
      const std::uint32_t u = fnUnivScc[fn];
      if (u != 0 && u < univDirtyStamp.size() && univDirtyStamp[u] != dirtyGen) {
        univDirtyStamp[u] = dirtyGen;
        if (!univSccs[u].weakAncCoreWatchIds.empty())
          markCoreWatchesStale(univSccs[u].weakAncCoreWatchIds);
        for (ObjectID obj : univSccs[u].accessedByObjects)
          cb(obj);
        if (!univSccs[u].weakAncCtxIds.empty())
          visitAncCtxList(univSccs[u].weakAncCtxIds);
      }
    }
  }

  void recordUnknownAccess(AccessID id) {
    const auto &acc = p.accesses_[id];
    if (!blockReachable[acc.block])
      return;
    const FunctionID fn = p.blocks_[acc.block].function;
    if (unknownFnAccesses[fn].insertBit(id)) {
      const std::uint32_t u = (fn < fnUnivScc.size()) ? fnUnivScc[fn] : 0U;
      const auto &fc = funcCFGs[fn];
      const bool reachesExit =
          fc.exitLocal != InvalidLocal && fc.canReachExit[blockToLocal[acc.block]];

      // Exact zero-dirtying fast path for already-wired persistent UnivWeakSCC
      // unknown joins: if `fn` is inside `univSccs[u]` and its persistent
      // `fnLocalExitUnkStoreNode`/`fnLocalUnkLoadNode` is ALREADY wired into
      // `scc.unkStoreJoin`/`scc.unkLoadJoin`, wire 1 edge directly into that
      // persistent join and skip dirtying all weak objects in `u`!
      if (u != 0 && reachesExit && acc.kind == Program::AccessKind::Store &&
          univSccs[u].hasLocalExitUnkStore && univSccs[u].unkStoreJoin != 0 &&
          fnLocalExitUnkStoreNode[fn] != 0) {
        const NodeID val =
            acc.sourceIsUnknown ? p.unknownValue() : acc.value;
        addIndirect(val, fnLocalExitUnkStoreNode[fn]);
        fnLocalUnkAccessWired[id] = 1;
        bumpFuncStateVer(fn, /*bumpUniv=*/false);
        if (!(val == p.unknownValue() && unknownModSummary[fn] != 0))
          ++unknownAccessVersion;
        for (ObjectID obj : funcAccessedByObjects[fn]) {
          if (processed[obj]) {
            callGraphDirty[obj] = true;
            markDirtySig(obj);
          }
        }
        return;
      }
      if (u != 0 && reachesExit && acc.kind == Program::AccessKind::Load &&
          univSccs[u].unkLoadJoin != 0 && fnLocalUnkLoadNode[fn] != 0) {
        addIndirect(fnLocalUnkLoadNode[fn], acc.value);
        fnLocalUnkAccessWired[id] = 1;
        bumpFuncStateVer(fn, /*bumpUniv=*/false);
        for (ObjectID obj : funcAccessedByObjects[fn]) {
          if (processed[obj]) {
            callGraphDirty[obj] = true;
            markDirtySig(obj);
          }
        }
        return;
      }

      ++unknownAccessVersion;
      bumpFuncStateVer(fn, /*bumpUniv=*/true);
      if (acc.kind == Program::AccessKind::Load)
        unknownDirectRef[fn] = true;
      // Only objects already active in `fn` (or `fn`'s univSCC) need to see
      // new local unknown accesses in `fn`; non-active objects see persistent
      // `unknownReadEntry`/`unknownModSummary` channels via
      // `ensureUnknownSummary()`.
      const std::uint64_t dg = ++univDirtyGen;
      forEachAccessedObjectInFunc(fn, dg, [&](ObjectID obj) {
        if (processed[obj]) {
          callGraphDirty[obj] = true;
          markDirtySig(obj);
        }
      });
    }
  }

  void markCallSiteUnknownBoundary(CallSiteID site) {
    if (siteUnknownBoundaryMarked[site])
      return;
    siteUnknownBoundaryMarked[site] = true;
    if (liveCallExternal[site])
      return; // `siteAlwaysCarries(site)` was already true.
    const BlockID blk = p.callSites_[site].block;
    if (!blockReachable[blk]) {
      ++alwaysCarryVersion;
      return;
    }
    const FunctionID caller = p.blocks_[blk].function;
    // Exact skip for exit-reaching callsites inside a UnivWeakSCC whose targets
    // are ALL already inside `callerU` or already covered by `callerU`'s
    // `alwaysRetExtRawSet` / `alwaysNoRetExtRawSet`: marking `site` adds zero
    // new SCC edges or external targets, so only non-univ objects
    // (`funcAccessedByObjects`) need dirtying here.
    const std::uint32_t callerU =
        (caller < fnUnivScc.size()) ? fnUnivScc[caller] : 0U;
    bool coveredByUnivExit =
        callerU != 0 && funcCFGs[caller].canReachExit[blockToLocal[blk]];
    if (coveredByUnivExit) {
      const auto &scc = univSccs[callerU];
      const bool isRetExit = p.callSites_[site].mayReturn;
      auto targetCovered = [&](FunctionID v) -> bool {
        if (v < fnUnivScc.size() && fnUnivScc[v] == callerU)
          return true;
        if (scc.alwaysRetExtRawSet.count(v))
          return true;
        return !isRetExit && scc.alwaysNoRetExtRawSet.count(v);
      };
      for (FunctionID callee : liveCallTargets[site]) {
        if (!targetCovered(callee)) {
          coveredByUnivExit = false;
          break;
        }
      }
      if (coveredByUnivExit && siteCallsUnknownHub[site]) {
        const FunctionID hubIdx = static_cast<FunctionID>(p.functions_.size());
        if (!unknownHubActive || !targetCovered(hubIdx))
          coveredByUnivExit = false;
      }
    }
    if (coveredByUnivExit) {
      if (univSccCGVer != callGraphVersion ||
          univSccAlwaysVer != alwaysCarryVersion) {
        ++alwaysCarryVersion;
      }
      bumpFuncStateVer(caller, /*bumpUniv=*/false);
      auto markDirectOnly = [&](FunctionID fn) {
        for (ObjectID obj : funcAccessedByObjects[fn]) {
          if (processed[obj] && !objLocalOnly[obj] && !p.objects_[obj].isGlobal &&
              !objExternallyEscaped[obj]) {
            callGraphDirty[obj] = true;
            markDirtySig(obj);
          }
        }
      };
      markDirectOnly(caller);
      for (FunctionID callee : liveCallTargets[site])
        markDirectOnly(callee);
      if (siteCallsUnknownHub[site]) {
        for (FunctionID callee : addressTakenFuncs)
          markDirectOnly(callee);
      }
      return;
    }
    ++alwaysCarryVersion;
    bumpFuncStateVer(caller, /*bumpUniv=*/true);
    const std::uint64_t dg = ++univDirtyGen;
    auto markFnObjs = [&](FunctionID fn) {
      forEachAccessedObjectInFunc(fn, dg, [&](ObjectID obj) {
        if (processed[obj] && !objLocalOnly[obj] && !p.objects_[obj].isGlobal &&
            !objExternallyEscaped[obj]) {
          callGraphDirty[obj] = true;
          markDirtySig(obj);
        }
      });
    };
    markFnObjs(caller);
    for (FunctionID callee : liveCallTargets[site])
      markFnObjs(callee);
    if (siteCallsUnknownHub[site]) {
      for (FunctionID callee : addressTakenFuncs)
        markFnObjs(callee);
    }
  }

  void markCallVisibilityDirty(ObjectID root) {
    // Global and externally-escaped objects already have `globalVisible=true`
    // (as do any children escaping into them), so arriving at another call
    // boundary cannot change `callSiteCanCarryObject` for them.
    if (!p.objects_[root].memory || p.objects_[root].isGlobal ||
        objExternallyEscaped[root])
      return;
    if (callVisMarkedGen[root] == callVisGen)
      return;
    std::vector<ObjectID> work{root};
    callVisMarkedGen[root] = callVisGen;
    while (!work.empty()) {
      const ObjectID cur = work.back();
      work.pop_back();
      objCallExposed[cur] = true;
      objLocalOnly[cur] = false;
      if (processed[cur] && !p.objects_[cur].readOnly) {
        callGraphDirty[cur] = true;
        markDirtySig(cur);
      }
      for (ObjectID child : reverseEscape[cur]) {
        if (p.objects_[child].memory && !p.objects_[child].isGlobal &&
            !objExternallyEscaped[child] &&
            callVisMarkedGen[child] != callVisGen) {
          callVisMarkedGen[child] = callVisGen;
          work.push_back(child);
        }
      }
    }
  }

  void markExternalEscape(ObjectID root) {
    if (!p.objects_[root].memory || objExternallyEscaped[root])
      return;
    std::vector<ObjectID> work{root};
    objExternallyEscaped[root] = true;
    externallyEscapedSet.insertBit(root);
    while (!work.empty()) {
      const ObjectID cur = work.back();
      work.pop_back();
      objCallExposed[cur] = true;
      objLocalOnly[cur] = false;
      if (processed[cur] && !p.objects_[cur].readOnly) {
        callGraphDirty[cur] = true;
        markDirtySig(cur);
      }
      for (ObjectID child : reverseEscape[cur]) {
        if (p.objects_[child].memory && !objExternallyEscaped[child]) {
          objExternallyEscaped[child] = true;
          externallyEscapedSet.insertBit(child);
          work.push_back(child);
        }
      }
      reverseEscape[cur].clear();
    }
  }

  void markEscapedToUnknown(ObjectID root) {
    if (!p.objects_[root].memory || objEscapedToUnknown[root])
      return;
    std::vector<ObjectID> work{root};
    objEscapedToUnknown[root] = true;
    escapedToUnknownSet.insertBit(root);
    while (!work.empty()) {
      const ObjectID cur = work.back();
      work.pop_back();
      if (!objExternallyEscaped[cur]) {
        objExternallyEscaped[cur] = true;
        externallyEscapedSet.insertBit(cur);
      }
      objCallExposed[cur] = true;
      objLocalOnly[cur] = false;
      std::vector<NodeID>().swap(escapePointers[cur]);
      escapePtrDirty[cur] = false;
      NodeSet().swap(initialEscapeObjects[cur]);
      std::vector<CallSiteID>().swap(escapedIndirectCalls[cur]);
      if (processed[cur] && !p.objects_[cur].readOnly) {
        callGraphDirty[cur] = true;
        markDirtySig(cur);
      }
      for (ObjectID child : reverseEscape[cur]) {
        if (p.objects_[child].memory && !objEscapedToUnknown[child]) {
          objEscapedToUnknown[child] = true;
          escapedToUnknownSet.insertBit(child);
          work.push_back(child);
        }
      }
      reverseEscape[cur].clear();
    }
  }

  void flushPendingAccesses(ObjectID obj) {
    auto &pa = pendingAccesses[obj];
    if (pa.empty())
      return;
    std::sort(pa.begin(), pa.end());
    activeAccesses[obj].insertSorted(pa);
    if (pa.capacity() > 64)
      std::vector<AccessID>().swap(pa);
    else
      pa.clear();
  }

  void addEscapePointer(ObjectID obj, NodeID ptr) {
    if (objEscapedToUnknown[obj])
      return;
    auto &ep = escapePointers[obj];
    if (ep.empty() || ep.back() != ptr) {
      ep.push_back(ptr);
      escapePtrDirty[obj] = true;
      if (ep.size() >= escapePtrNextDedup[obj]) {
        std::sort(ep.begin(), ep.end());
        ep.erase(std::unique(ep.begin(), ep.end()), ep.end());
        escapePtrDirty[obj] = false;
        escapePtrNextDedup[obj] = static_cast<std::uint32_t>(
            std::max<std::size_t>(128, ep.size() * 2));
      }
    }
  }

  const std::vector<NodeID> &getEscapePointers(ObjectID obj) {
    auto &ep = escapePointers[obj];
    if (escapePtrDirty[obj]) {
      std::sort(ep.begin(), ep.end());
      ep.erase(std::unique(ep.begin(), ep.end()), ep.end());
      escapePtrDirty[obj] = false;
    }
    return ep;
  }

  void recordReverseEscape(ObjectID dep, ObjectID child) {
    if (dep == child || !p.objects_[dep].memory || !p.objects_[child].memory ||
        objEscapedToUnknown[child])
      return;
    if (dep == p.unknownObject() || objEscapedToUnknown[dep]) {
      markEscapedToUnknown(child);
      return;
    }
    if (objExternallyEscaped[child])
      return;
    if (objExternallyEscaped[dep]) {
      markExternalEscape(child);
      return;
    }
    if (reverseEscape[dep].insertBit(child)) {
      if (objCallExposed[dep])
        markCallVisibilityDirty(child);
    }
  }

  bool addVFGEdge(NodeID from, NodeID to, bool countIndirect) {
    if (!insertAdj(from, to))
      return false;
    edgesCacheBuilt = false;
    if (countIndirect)
      ++stats.indirectEdges;
    else if (from < p.edges_.size())
      extraDirectEdges[from].insertBit(to);
    const NodeID rFrom = findRep(from);
    const NodeID rTo = findRep(to);
    if (rFrom != rTo && ptsIdx[rFrom] != 0)
      pendingEdgeSeeds.emplace_back(rFrom, rTo);
    return true;
  }

  bool addIndirect(NodeID from, NodeID to) {
    if (localCap.active)
      return localCap.addEdge(from, to);
    return addVFGEdge(from, to, /*countIndirect=*/true);
  }

  //===--------------------------------------------------------------------===//
  // Per-function CFG & dominance construction (lazy, once per reachable fn)
  //===--------------------------------------------------------------------===//

  void buildFunctionCFG(FunctionID fn) {
    auto &fc = funcCFGs[fn];
    if (fc.built)
      return;
    fc.built = true;

    const auto &fnInfo = p.functions_[fn];
    std::vector<BlockID> rootBlocks;
    if (fnInfo.entry)
      rootBlocks.push_back(fnInfo.entry);
    for (BlockID r : p.roots_)
      if (p.blocks_[r].function == fn)
        appendUnique(rootBlocks, r);

    // Intra-procedural BFS for blocks reachable in this function.
    std::vector<BlockID> work(rootBlocks.begin(), rootBlocks.end());
    std::vector<BlockID> orderedBlocks;
    while (!work.empty()) {
      const BlockID b = work.back();
      work.pop_back();
      if (blockReachable[b])
        continue;
      blockReachable[b] = true;
      orderedBlocks.push_back(b);
      for (BlockID succ : p.blocks_[b].successors)
        if (!blockReachable[succ])
          work.push_back(succ);
    }

    // Assign dense local block IDs: 0 = virtual entry, 1..N-1 = reachable
    // blocks.
    fc.localToGlobal.assign(1, 0);
    for (BlockID b : orderedBlocks) {
      const LocalBlock local = nextID(fc.localToGlobal.size());
      fc.localToGlobal.push_back(b);
      blockToLocal[b] = local;
    }
    if (fnInfo.exit && blockReachable[fnInfo.exit])
      fc.exitLocal = blockToLocal[fnInfo.exit];

    const std::size_t n = fc.localToGlobal.size();
    fc.succs.assign(n, {});
    fc.preds.assign(n, {});
    fc.selfLoop.assign(n, false);
    auto addEdge = [&](LocalBlock from, LocalBlock to) {
      if (from == to)
        fc.selfLoop[from] = true;
      appendUnique(fc.succs[from], to);
      appendUnique(fc.preds[to], from);
    };
    for (BlockID r : rootBlocks)
      if (blockReachable[r])
        addEdge(0, blockToLocal[r]);
    for (LocalBlock u = 1; u < n; ++u) {
      const BlockID global = fc.localToGlobal[u];
      for (BlockID succ : p.blocks_[global].successors)
        if (blockReachable[succ])
          addEdge(u, blockToLocal[succ]);
    }

    // Cooper-Harvey-Kennedy iterative dominators on block-level local CFG.
    std::vector<bool> visited(n, false);
    std::vector<std::pair<LocalBlock, std::size_t>> dfs{{0, 0}};
    std::vector<LocalBlock> postorder;
    postorder.reserve(n);
    visited[0] = true;
    while (!dfs.empty()) {
      auto &[node, idx] = dfs.back();
      if (idx < fc.succs[node].size()) {
        const LocalBlock succ = fc.succs[node][idx++];
        if (!visited[succ]) {
          visited[succ] = true;
          dfs.emplace_back(succ, 0);
        }
        continue;
      }
      postorder.push_back(node);
      dfs.pop_back();
    }
    std::vector<LocalBlock> rpo(postorder.rbegin(), postorder.rend());
    std::vector<LocalBlock> rpoNum(n, InvalidLocal);
    for (LocalBlock i = 0; i < rpo.size(); ++i)
      rpoNum[rpo[i]] = i;

    fc.idom.assign(n, InvalidLocal);
    fc.idom[0] = 0;
    auto intersect = [&](LocalBlock left, LocalBlock right) {
      while (left != right) {
        while (rpoNum[left] > rpoNum[right])
          left = fc.idom[left];
        while (rpoNum[right] > rpoNum[left])
          right = fc.idom[right];
      }
      return left;
    };
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t i = 1; i < rpo.size(); ++i) {
        const LocalBlock node = rpo[i];
        LocalBlock newDom = InvalidLocal;
        for (LocalBlock pred : fc.preds[node]) {
          if (fc.idom[pred] == InvalidLocal)
            continue;
          newDom =
              newDom == InvalidLocal ? pred : intersect(pred, newDom);
        }
        if (newDom == InvalidLocal)
          throw std::logic_error(
              "ValueFlowPTA: reachable control node has no dominator");
        if (fc.idom[node] != newDom) {
          fc.idom[node] = newDom;
          changed = true;
        }
      }
    }

    fc.df.assign(n, {});
    for (LocalBlock node = 1; node < n; ++node) {
      if (fc.preds[node].size() < 2)
        continue;
      for (LocalBlock pred : fc.preds[node]) {
        LocalBlock runner = pred;
        while (runner != fc.idom[node]) {
          appendUnique(fc.df[runner], node);
          runner = fc.idom[runner];
        }
      }
    }

    fc.canReachExit.assign(n, false);
    if (fc.exitLocal != InvalidLocal) {
      std::vector<LocalBlock> revWork{fc.exitLocal};
      fc.canReachExit[fc.exitLocal] = true;
      while (!revWork.empty()) {
        const LocalBlock cur = revWork.back();
        revWork.pop_back();
        for (LocalBlock pred : fc.preds[cur]) {
          if (!fc.canReachExit[pred]) {
            fc.canReachExit[pred] = true;
            revWork.push_back(pred);
          }
        }
      }
    }

    fc.defBlockGen.assign(n, 0);
    fc.idfGen.assign(n, 0);
    fc.memoGen.assign(n, 0);
    fc.phiSparse.assign(n, 0);
    fc.lastStoreSparse.assign(n, 0);
    fc.memoExitDef.assign(n, 0);
    stats.expandedControlFlowNodes += n;

    // Register reachable accesses and callsites in this function.
    std::vector<CallSiteID> unknownBoundarySites;
    for (LocalBlock u = 1; u < n; ++u) {
      const BlockID b = fc.localToGlobal[u];
      std::uint32_t pos = 0;
      for (const auto &ev : p.blocks_[b].events) {
        if (ev.kind == Program::BlockEvent::Kind::Access) {
          const AccessID id = ev.id;
          accessEventPos[id] = pos++;
          const auto &access = p.accesses_[id];
          uses[access.pointer].push_back(id);
          if (access.kind == Program::AccessKind::Load)
            nodeHasLoadUse[access.pointer] = 1;
          enableNodeHook(access.pointer);
          if (access.kind == Program::AccessKind::Store) {
            storedThrough[access.value] = access.pointer;
            enableNodeHook(access.value);
            // Objects that already reached access.value now escape via pointer.
            const NodeID ptr = access.pointer;
            const auto &ptrPts = ptsOf(ptr);
            bool ptrUnk = ptrHasUnknownTarget[ptr];
            if (!ptrUnk && ptrPts.count(p.unknownObject())) {
              ptrHasUnknownTarget[ptr] = true;
              ptrHasExternalTarget[ptr] = true;
              ptrUnk = true;
              for (ObjectID c : ptrEscapedBy[ptr])
                markEscapedToUnknown(c);
              ptrEscapedBy[ptr].clear();
            }
            if (ptrUnk) {
              std::vector<ObjectID> freshUnk;
              ptsOf(access.value).forEachNotIn(
                  escapedToUnknownSet, [&](ObjectID obj) {
                    if (p.objects_[obj].memory)
                      freshUnk.push_back(obj);
                  });
              for (ObjectID obj : freshUnk)
                markEscapedToUnknown(obj);
            } else {
              bool ptrExt = ptrHasExternalTarget[ptr];
              if (!ptrExt) {
                for (ObjectID target : ptrPts) {
                  if (p.objects_[target].memory &&
                      objExternallyEscaped[target]) {
                    ptrHasExternalTarget[ptr] = true;
                    ptrExt = true;
                    for (ObjectID c : ptrEscapedBy[ptr])
                      markExternalEscape(c);
                    ptrEscapedBy[ptr].clear();
                    break;
                  }
                }
              }
              std::vector<ObjectID> freshEsc;
              ptsOf(access.value).forEachNotIn(
                  escapedToUnknownSet, [&](ObjectID obj) {
                    if (p.objects_[obj].memory)
                      freshEsc.push_back(obj);
                  });
              for (ObjectID obj : freshEsc) {
                addEscapePointer(obj, ptr);
                if (objExternallyEscaped[obj])
                  continue;
                if (ptrExt) {
                  markExternalEscape(obj);
                  continue;
                }
                if (ptrEscapedBy[ptr].insertBit(obj)) {
                  enableNodeHook(ptr);
                  for (ObjectID target : ptrPts)
                    recordReverseEscape(target, obj);
                }
              }
            }
          }
          // Objects that already reached access.pointer now see this access.
          const NodeID rPtr = findRep(access.pointer);
          const auto &accPtrPts = ptsOfRep(rPtr);
          if (accPtrPts.size() >= 2)
            nodeHadMultiTarget[access.pointer] = true;
          if (access.kind == Program::AccessKind::Load &&
              !readOnlyWithInitSet.empty()) {
            const std::uint32_t ptrPoolId = ptsIdx[rPtr];
            if (ptrPoolId != 0) {
              const NodeID roJoin = getOrCreatePoolReadOnlyJoin(ptrPoolId);
              if (roJoin != kNoReadOnlyJoin)
                addIndirect(roJoin, access.value);
            }
          }
          auto handleInitObj = [&](ObjectID obj) {
            if (!p.objects_[obj].memory || p.objects_[obj].readOnly)
              return;
            pendingAccesses[obj].push_back(id);
            markDirtySig(obj);
            if (obj == p.unknownObject())
              recordUnknownAccess(id);
          };
          accPtrPts.forEachNotIn(skipUseObjSet, handleInitObj);
        } else {
          const CallSiteID site = ev.id;
          callEventPos[site] = pos++;
          funcCallSites[fn].push_back(site);
          const auto &cs = p.callSites_[site];
          if (cs.hasExternalTarget && !liveCallExternal[site]) {
            liveCallExternal[site] = true;
            ++alwaysCarryVersion;
            bumpFuncStateVer(fn, true);
          }
          auto registerBoundaryNode = [&](NodeID node) {
            if (!node)
              return;
            isCallBoundaryNode[node] = true;
            appendUnique(callBoundarySites[node], site);
            enableNodeHook(node);
            const auto &nPts = ptsOf(node);
            if (nPts.count(p.unknownObject())) {
              objCallExposed[p.unknownObject()] = true;
              unknownBoundarySites.push_back(site);
            }
            const bool notAlways = !siteAlwaysCarries(site);
            std::vector<ObjectID> freshVis;
            nPts.forEachNotIn(externallyEscapedSet, [&](ObjectID obj) {
              freshVis.push_back(obj);
            });
            for (ObjectID obj : freshVis) {
              objCallExposed[obj] = true;
              if (notAlways && p.objects_[obj].memory)
                objCarrySites[obj].insertBit(site);
              markCallVisibilityDirty(obj);
            }
          };
          for (NodeID arg : cs.actualArgs)
            registerBoundaryNode(arg);
          registerBoundaryNode(cs.result);
          if (cs.calleePointer) {
            indirectCallUses[cs.calleePointer].push_back(site);
            enableNodeHook(cs.calleePointer);
            // Mark loads producing calleePointer to prioritize resolving it.
            isIndirectCalleeLoad[cs.calleePointer] = true;
            if (!ptsOf(cs.calleePointer).empty())
              pendingIndirectSites.insert(site);
            const bool siteResolved = !liveCallTargets[site].empty() ||
                                      liveCallExternal[site] ||
                                      siteCallsUnknownHub[site];
            for (NodeID arg : cs.actualArgs) {
              if (!arg)
                continue;
              indirectArgUses[arg].push_back(site);
              enableNodeHook(arg);
              if (!siteResolved) {
                ptsOf(arg).forEachNotIn(escapedToUnknownSet, [&](ObjectID obj) {
                  appendUnique(escapedIndirectCalls[obj], site);
                });
              }
            }
          }
        }
      }
      for (NodeID escNode : p.blocks_[b].externalEscapes) {
        isExternalEscapeNode[escNode] = true;
        enableNodeHook(escNode);
        for (ObjectID obj : ptsOf(escNode)) {
          if (obj != p.unknownObject())
            markExternalEscape(obj);
        }
      }
    }
    for (CallSiteID site : unknownBoundarySites)
      markCallSiteUnknownBoundary(site);
    ++unknownAccessVersion;
  }

  void ensureFunctionSCCs(FunctionID fn) {
    auto &fc = funcCFGs[fn];
    if (fc.sccBuilt)
      return;
    fc.sccBuilt = true;
    auto partition = computeSCCs(fc.succs, fc.preds);
    fc.comp = std::move(partition.first);
    const std::size_t numComps = partition.second;
    fc.compBlocks.assign(numComps, {});
    fc.compPreds.assign(numComps, {});
    for (LocalBlock b = 0; b < fc.succs.size(); ++b)
      fc.compBlocks[fc.comp[b]].push_back(b);
    fc.compCyclic.assign(numComps, false);
    for (std::size_t c = 0; c < numComps; ++c) {
      fc.compCyclic[c] =
          fc.compBlocks[c].size() > 1 ||
          (fc.compBlocks[c].size() == 1 && fc.selfLoop[fc.compBlocks[c][0]]);
    }
    for (LocalBlock u = 0; u < fc.succs.size(); ++u)
      for (LocalBlock v : fc.succs[u])
        if (fc.comp[u] != fc.comp[v])
          fc.compPreds[fc.comp[v]].insert(nextID(fc.comp[u]));
    fc.weakNeededGen.assign(numComps, 0);
    fc.weakActiveIdx.assign(numComps, 0);
    fc.weakFlowNode.assign(numComps, 0);
  }

  void activateUnknownHub() {
    if (unknownHubActive || addressTakenFuncs.empty())
      return;
    unknownHubActive = true;
    forceCondense = true;
    ++callGraphVersion;
    ++unknownAccessVersion;
    ++hubGlobalVersion;
    bumpFuncStateVer(static_cast<FunctionID>(p.functions_.size()), true);
    for (FunctionID callee : addressTakenFuncs) {
      const auto &calleeInfo = p.functions_[callee];
      for (std::size_t i = 0; i < calleeInfo.formalParams.size(); ++i) {
        const NodeID formal = calleeInfo.formalParams[i];
        if (!formal || p.sources_[formal])
          continue;
        if (i >= unknownHubArgNodes.size())
          unknownHubArgNodes.resize(i + 1, 0);
        if (!unknownHubArgNodes[i])
          unknownHubArgNodes[i] = addJoin();
        addVFGEdge(unknownHubArgNodes[i], formal, /*countIndirect=*/false);
      }
      if (calleeInfo.returned) {
        if (!unknownHubRetNode)
          unknownHubRetNode = addJoin();
        addVFGEdge(calleeInfo.returned, unknownHubRetNode,
                   /*countIndirect=*/false);
      } else {
        unknownHubHasVoidReturn = true;
      }
    }
    for (FunctionID callee : addressTakenFuncs)
      activateFunction(callee);
  }

  bool connectUnknownHubSite(CallSiteID site, bool dynamic,
                             bool &hubJustActivated) {
    hubJustActivated = false;
    if (siteCallsUnknownHub[site] || addressTakenFuncs.empty())
      return false;
    siteCallsUnknownHub[site] = true;
    persistentCallUnknownHub[site] = true;
    unknownHubCallerSites.push_back(site);
    ++callGraphVersion;
    ++unknownAccessVersion;
    bumpFuncStateVer(p.blocks_[p.callSites_[site].block].function, true);
    if (dynamic)
      ++stats.dynamicIndirectTargets;
    if (!unknownHubActive) {
      hubJustActivated = true;
      activateUnknownHub();
    }
    const auto &cs = p.callSites_[site];
    const std::size_t nArgs = unknownHubArgNodes.size();
    for (std::size_t i = 0; i < nArgs; ++i) {
      if (!unknownHubArgNodes[i])
        continue;
      const NodeID actual =
          (i < cs.actualArgs.size() && cs.actualArgs[i]) ? cs.actualArgs[i]
                                                         : p.unknownValue();
      addVFGEdge(actual, unknownHubArgNodes[i], /*countIndirect=*/false);
    }
    if (cs.result && !p.sources_[cs.result]) {
      if (unknownHubRetNode)
        addVFGEdge(unknownHubRetNode, cs.result, /*countIndirect=*/false);
      if (unknownHubHasVoidReturn)
        addVFGEdge(p.unknownValue(), cs.result, /*countIndirect=*/false);
    }
    return true;
  }

  void activateFunction(FunctionID fn) {
    if (funcReachable[fn])
      return;
    std::vector<FunctionID> work{fn};
    while (!work.empty()) {
      const FunctionID cur = work.back();
      work.pop_back();
      if (funcReachable[cur])
        continue;
      funcReachable[cur] = true;
      if (isAddressTakenFunc[cur]) {
        ++hubGlobalVersion;
        bumpFuncStateVer(static_cast<FunctionID>(p.functions_.size()), true);
      }
      buildFunctionCFG(cur);

      // Connect all direct targets, external callbacks, and previously
      // persisted indirect targets from reachable callsites in cur.
      for (CallSiteID site : funcCallSites[cur]) {
        const auto &cs = p.callSites_[site];
        for (FunctionID target : cs.directTargets) {
          if (connectCallEdgeInternal(site, target, /*dynamic=*/false,
                                      /*checkStrongKill=*/false))
            work.push_back(target);
        }
        for (FunctionID cb : cs.externalCallbacks) {
          if (connectCallEdgeInternal(site, cb, /*dynamic=*/false,
                                      /*checkStrongKill=*/false))
            work.push_back(cb);
        }
        if (persistentCallExternal[site] && !liveCallExternal[site]) {
          liveCallExternal[site] = true;
          ++alwaysCarryVersion;
          bumpFuncStateVer(cur, true);
        }
        if (persistentCallUnknownHub[site]) {
          bool ignored = false;
          connectUnknownHubSite(site, /*dynamic=*/false, ignored);
        }
        for (FunctionID target : persistentCallTargets[site]) {
          if (connectCallEdgeInternal(site, target, /*dynamic=*/false,
                                      /*checkStrongKill=*/false))
            work.push_back(target);
        }
      }
    }
  }

  void updateRecursionStatus() {
    const std::size_t numFuncs = p.functions_.size();
    const FunctionID hubIdx = static_cast<FunctionID>(numFuncs);
    std::vector<std::vector<FunctionID>> cgSucc(numFuncs + 1);
    std::vector<std::vector<FunctionID>> cgPred(numFuncs + 1);
    std::vector<bool> selfCall(numFuncs + 1, false);
    for (CallSiteID site = 1; site < p.callSites_.size(); ++site) {
      const BlockID block = p.callSites_[site].block;
      if (!blockReachable[block])
        continue;
      const FunctionID caller = p.blocks_[block].function;
      for (FunctionID callee : liveCallTargets[site]) {
        if (caller == callee)
          selfCall[caller] = true;
        cgSucc[caller].push_back(callee);
        cgPred[callee].push_back(caller);
      }
    }
    if (unknownHubActive) {
      for (CallSiteID site : unknownHubCallerSites) {
        const BlockID block = p.callSites_[site].block;
        if (!blockReachable[block])
          continue;
        const FunctionID caller = p.blocks_[block].function;
        cgSucc[caller].push_back(hubIdx);
        cgPred[hubIdx].push_back(caller);
      }
      for (FunctionID callee : addressTakenFuncs) {
        if (funcReachable[callee]) {
          cgSucc[hubIdx].push_back(callee);
          cgPred[callee].push_back(hubIdx);
        }
      }
    }
    auto [comp, numComps] = computeSCCs(cgSucc, cgPred);
    std::vector<std::size_t> compSize(numComps, 0);
    for (FunctionID f = 0; f <= hubIdx; ++f)
      ++compSize[comp[f]];

    std::vector<std::uint8_t> newlyRec(numFuncs, 0);
    bool anyNewlyRec = false;
    for (FunctionID f = 1; f < numFuncs; ++f) {
      const bool nowRec = selfCall[f] || compSize[comp[f]] > 1;
      if (nowRec && !funcRecursive[f]) {
        funcRecursive[f] = true;
        newlyRec[f] = 1;
        anyNewlyRec = true;
      }
    }
    if (anyNewlyRec) {
      // Stack objects owned by a newly-recursive function can no longer
      // receive strong updates or stay scoped strictly to their owner.
      for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
        const FunctionID owner = p.objects_[obj].ownerFunction;
        if (owner && newlyRec[owner]) {
          if (effectiveSingleton[obj]) {
            effectiveSingleton[obj] = false;
            markDirtySig(obj);
          }
          if (effectiveOwner[obj] == owner) {
            effectiveOwner[obj] = 0;
            callGraphDirty[obj] = true;
            markDirtySig(obj);
          }
        }
      }
    }
  }

  bool connectCallEdgeInternal(CallSiteID site, FunctionID callee, bool dynamic,
                               bool checkStrongKill) {
    if (!liveCallTargets[site].insert(callee).second)
      return false;
    ++callGraphVersion;
    ++unknownAccessVersion;
    persistentCallTargets[site].insert(callee);
    const auto &cs = p.callSites_[site];
    const FunctionID caller = p.blocks_[cs.block].function;
    bumpFuncStateVer(caller, true);
    callers[callee].insert(caller);
    callerSites[callee].insert(site);
    if (dynamic)
      ++stats.dynamicIndirectTargets;

    // Wire direct argument -> formal parameter and return -> call result
    // copies.
    const auto &calleeInfo = p.functions_[callee];
    const std::size_t paramCount = calleeInfo.formalParams.size();
    for (std::size_t i = 0; i < paramCount; ++i) {
      const NodeID formal = calleeInfo.formalParams[i];
      if (!formal || p.sources_[formal])
        continue;
      const NodeID actual =
          (i < cs.actualArgs.size() && cs.actualArgs[i]) ? cs.actualArgs[i]
                                                         : p.unknownValue();
      addVFGEdge(actual, formal, /*countIndirect=*/false);
    }
    if (cs.result && !p.sources_[cs.result]) {
      const NodeID retNode =
          calleeInfo.returned ? calleeInfo.returned : p.unknownValue();
      addVFGEdge(retNode, cs.result, /*countIndirect=*/false);
    }

    const bool newlyReachable = !funcReachable[callee];
    if (newlyReachable)
      activateFunction(callee);
    (void)checkStrongKill;
    return newlyReachable;
  }

  bool resolveDynamicCalls(bool &strongRestartNeeded) {
    bool connectedAny = false;
    std::vector<FunctionID> batchCallees;
    std::vector<FunctionID> batchCallers;
    std::vector<FunctionID> freshTargets;
    std::vector<FunctionID> freshCallers;
    std::vector<FunctionID> freshNoRetCallers;

    while (!pendingIndirectSites.empty()) {
      const CallSiteID site = *pendingIndirectSites.begin();
      pendingIndirectSites.erase(pendingIndirectSites.begin());
      if (siteIndirectSaturated[site])
        continue;
      const auto &cs = p.callSites_[site];
      if (!blockReachable[cs.block] || !cs.calleePointer)
        continue;
      const FunctionID caller = p.blocks_[cs.block].function;
      const bool hadTargetsBefore = !liveCallTargets[site].empty() ||
                                    liveCallExternal[site] ||
                                    siteCallsUnknownHub[site];
      bool siteConnected = false;
      std::vector<FunctionID> newTargets;
      const auto &calleePts = ptsOf(cs.calleePointer);
      if (calleePts.count(p.unknownObject())) {
        if (!liveCallExternal[site]) {
          liveCallExternal[site] = true;
          persistentCallExternal[site] = true;
          ++alwaysCarryVersion;
          ++callGraphVersion;
          bumpFuncStateVer(caller, true);
          for (FunctionID existingTarget : liveCallTargets[site])
            batchCallees.push_back(existingTarget);
          connectedAny = true;
          siteConnected = true;
        }
        if (cs.unknownExternalTarget &&
            !liveCallTargets[site].count(cs.unknownExternalTarget)) {
          newTargets.push_back(cs.unknownExternalTarget);
        }
        if (cs.unknownExternalEscapesArgs && !siteUnknownArgsEscaped[site]) {
          siteUnknownArgsEscaped[site] = true;
          for (NodeID arg : cs.actualArgs) {
            if (!arg)
              continue;
            isExternalEscapeNode[arg] = true;
            enableNodeHook(arg);
            std::vector<ObjectID> freshExt;
            ptsOf(arg).forEachNotIn(externallyEscapedSet, [&](ObjectID o) {
              if (o != p.unknownObject())
                freshExt.push_back(o);
            });
            for (ObjectID o : freshExt)
              markExternalEscape(o);
          }
        }
        if (!siteCallsUnknownHub[site] && !addressTakenFuncs.empty()) {
          bool hubJustActivated = false;
          if (connectUnknownHubSite(site, /*dynamic=*/true, hubJustActivated)) {
            connectedAny = true;
            siteConnected = true;
            for (FunctionID candidate : addressTakenFuncs) {
              batchCallees.push_back(candidate);
            }
          }
        }
      }
      if (siteCallsUnknownHub[site]) {
        // `unknownHub` already connects every `addressTakenFunc` in O(1); only
        // connect concrete function objects that were not marked address-taken.
        if (!nonAddrTakenFuncObjSet.empty()) {
          calleePts.forEachIn(nonAddrTakenFuncObjSet, [&](ObjectID obj) {
            if (const FunctionID fn = p.functionOfObject_[obj]) {
              if (!liveCallTargets[site].count(fn))
                newTargets.push_back(fn);
            }
          });
        } else if (liveCallExternal[site]) {
          siteIndirectSaturated[site] = 1;
        }
      } else if (!funcObjectSet.empty()) {
        calleePts.forEachIn(funcObjectSet, [&](ObjectID obj) {
          if (const FunctionID fn = p.functionOfObject_[obj]) {
            if (!liveCallTargets[site].count(fn))
              newTargets.push_back(fn);
          }
        });
      }
      const bool canFreshStrongKill = !hadTargetsBefore &&
                                      !liveCallExternal[site] &&
                                      !siteCallsUnknownHub[site] &&
                                      !cs.isInvoke;
      for (FunctionID callee : newTargets) {
        connectCallEdgeInternal(site, callee, /*dynamic=*/true,
                                /*checkStrongKill=*/false);
        connectedAny = true;
        siteConnected = true;
        batchCallees.push_back(callee);
        if (canFreshStrongKill)
          freshTargets.push_back(callee);
      }
      if (siteConnected) {
        batchCallers.push_back(caller);
        if (canFreshStrongKill)
          freshCallers.push_back(caller);
      }
      if (canFreshStrongKill && !newTargets.empty() && cs.mayReturn) {
        const LocalBlock siteLocal = blockToLocal[cs.block];
        const bool hasCodeAfter =
            !funcCFGs[caller].succs[siteLocal].empty() ||
            (callEventPos[site] + 1 < p.blocks_[cs.block].events.size());
        if (hasCodeAfter) {
          bool allNoRet = true;
          for (FunctionID t : liveCallTargets[site]) {
            if (funcCFGs[t].exitLocal != InvalidLocal) {
              allNoRet = false;
              break;
            }
          }
          if (allNoRet)
            freshNoRetCallers.push_back(caller);
        }
      }
    }

    if (!connectedAny)
      return false;

    // Single generation-stamped call-graph traversal for the whole batch to
    // mark processed objects in affected callees and callers dirty.
    const std::uint64_t dirtyGen = ++cgGen;
    const std::uint64_t uDirtyGen = ++univDirtyGen;
    std::vector<FunctionID> subWork;
    for (FunctionID callee : batchCallees) {
      if (cgVisitGen[callee] != dirtyGen) {
        cgVisitGen[callee] = dirtyGen;
        subWork.push_back(callee);
      }
    }
    bool hubVisited = false;
    while (!subWork.empty()) {
      const FunctionID f = subWork.back();
      subWork.pop_back();
      forEachAccessedObjectInFunc(f, uDirtyGen, [&](ObjectID obj) {
        if (objLocalOnly[obj])
          return;
        callGraphDirty[obj] = true;
        markDirtySig(obj);
      });
      for (CallSiteID subSite : funcCallSites[f]) {
        for (FunctionID next : liveCallTargets[subSite]) {
          if (cgVisitGen[next] != dirtyGen) {
            cgVisitGen[next] = dirtyGen;
            subWork.push_back(next);
          }
        }
        if (siteCallsUnknownHub[subSite] && !hubVisited) {
          hubVisited = true;
          for (FunctionID next : addressTakenFuncs) {
            if (funcReachable[next] && cgVisitGen[next] != dirtyGen) {
              cgVisitGen[next] = dirtyGen;
              subWork.push_back(next);
            }
          }
        }
      }
    }
    for (FunctionID caller : batchCallers) {
      forEachAccessedObjectInFunc(caller, uDirtyGen, [&](ObjectID obj) {
        if (objLocalOnly[obj])
          return;
        callGraphDirty[obj] = true;
        markDirtySig(obj);
      });
    }

    computePointedToBy();

    if (!weak) {
      // Check if any already-emitted strong singleton gained a new strong
      // store from newly activated callees.
      for (ObjectID obj : dirtySigList) {
        if (!p.objects_[obj].memory || !emittedFlow[obj] ||
            !canStrongUpdate(obj))
          continue;
        const Signature newSig = signature(obj);
        for (AccessID strong : newSig.strong) {
          if (!previous[obj].strong.count(strong)) {
            strongRestartNeeded = true;
            break;
          }
        }
        if (strongRestartNeeded)
          break;
      }
      // If any callsite previously had no targets, check whether a newly
      // connected callee can now materialize a strong kill (strong-eligible
      // exit-reaching singleton store or all-noreturn call) for an
      // already-emitted singleton in caller or an ancestor.
      if (!strongRestartNeeded && !freshTargets.empty()) {
        const std::uint64_t subGen = ++cgGen;
        std::vector<FunctionID> fWork;
        for (FunctionID t : freshTargets) {
          if (cgVisitGen[t] != subGen) {
            cgVisitGen[t] = subGen;
            fWork.push_back(t);
          }
        }
        bool subHubVisited = false;
        while (!fWork.empty()) {
          const FunctionID f = fWork.back();
          fWork.pop_back();
          for (CallSiteID sub : funcCallSites[f]) {
            for (FunctionID next : liveCallTargets[sub]) {
              if (cgVisitGen[next] != subGen) {
                cgVisitGen[next] = subGen;
                fWork.push_back(next);
              }
            }
            if (siteCallsUnknownHub[sub] && !subHubVisited) {
              subHubVisited = true;
              for (FunctionID next : addressTakenFuncs) {
                if (funcReachable[next] && cgVisitGen[next] != subGen) {
                  cgVisitGen[next] = subGen;
                  fWork.push_back(next);
                }
              }
            }
          }
        }
        auto hasStrongStoreInSubFuncs = [&](ObjectID obj,
                                            const NodeSet &accesses) {
          for (AccessID id : accesses) {
            const auto &acc = p.accesses_[id];
            if (acc.kind != Program::AccessKind::Store || !acc.allowStrong)
              continue;
            const FunctionID accFn = p.blocks_[acc.block].function;
            if (cgVisitGen[accFn] != subGen)
              continue;
            if (!blockReachable[acc.block] ||
                funcCFGs[accFn].exitLocal == InvalidLocal ||
                !funcCFGs[accFn].canReachExit[blockToLocal[acc.block]])
              continue;
            const auto &ptrPts = ptsOf(acc.pointer);
            if (ptrPts.size() == 1 && ptrPts.count(obj))
              return true;
          }
          return false;
        };
        const std::uint64_t ancGen = ++cgGen;
        std::vector<FunctionID> ancFuncs;
        std::vector<FunctionID> aWork;
        for (FunctionID c : freshCallers) {
          if (cgAncGen[c] != ancGen) {
            cgAncGen[c] = ancGen;
            aWork.push_back(c);
            ancFuncs.push_back(c);
          }
        }
        bool ancHubVisited = false;
        while (!aWork.empty()) {
          const FunctionID f = aWork.back();
          aWork.pop_back();
          for (FunctionID parent : callers[f]) {
            if (cgAncGen[parent] != ancGen) {
              cgAncGen[parent] = ancGen;
              aWork.push_back(parent);
              ancFuncs.push_back(parent);
            }
          }
          if (unknownHubActive && isAddressTakenFunc[f] && !ancHubVisited) {
            ancHubVisited = true;
            for (CallSiteID hSite : unknownHubCallerSites) {
              const BlockID blk = p.callSites_[hSite].block;
              if (!blockReachable[blk])
                continue;
              const FunctionID parent = p.blocks_[blk].function;
              if (cgAncGen[parent] != ancGen) {
                cgAncGen[parent] = ancGen;
                aWork.push_back(parent);
                ancFuncs.push_back(parent);
              }
            }
          }
        }
        const bool anyFreshNoReturn = !freshNoRetCallers.empty();
        for (FunctionID anc : ancFuncs) {
          for (ObjectID obj : funcAccessedByObjects[anc]) {
            if (!emittedFlow[obj] || !canStrongUpdate(obj))
              continue;
            if (anyFreshNoReturn) {
              strongRestartNeeded = true;
              break;
            }
            flushPendingAccesses(obj);
            if (hasStrongStoreInSubFuncs(obj, activeAccesses[obj])) {
              strongRestartNeeded = true;
              break;
            }
          }
          if (strongRestartNeeded)
            break;
        }
      }
    }

    ++reachEpoch;
    updateRecursionStatus();
    updateUnresolvedIndirectCallFlags();
    return true;
  }

  void updateUnresolvedIndirectCallFlags() {
    funcHasUnresolvedIndirectCall.assign(p.functions_.size(), false);
    anyUnresolvedIndirectCall = false;
    for (CallSiteID site = 1; site < p.callSites_.size(); ++site) {
      const auto &cs = p.callSites_[site];
      if (!cs.calleePointer || !blockReachable[cs.block])
        continue;
      if (liveCallTargets[site].empty() && !liveCallExternal[site] &&
          !siteCallsUnknownHub[site]) {
        funcHasUnresolvedIndirectCall[p.blocks_[cs.block].function] = true;
        anyUnresolvedIndirectCall = true;
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Initialization / Reset
  //===--------------------------------------------------------------------===//

  // Exact Offline Variable Substitution (OVS) on the initial static direct-copy
  // graph `p.edges_`: collapses direct-copy SCCs, provably-empty fixed-in
  // nodes, single-predecessor fixed-in nodes (e.g. GEP/BitCast/Freeze/Store
  // wrapper chains), and identical-predecessor fixed-in nodes BEFORE the first
  // propagation wave, then assigns exact spaced topological ranks to the
  // resulting DAG. Uncondensed `adj` is untouched for VFG inspection queries.
  void runOfflineCopySubstitution() {
    const NodeID numProg = static_cast<NodeID>(p.edges_.size());
    if (numProg <= 2)
      return;

    // 1. Identify nodes whose incoming edge set in `p.edges_` is final forever.
    std::vector<std::uint8_t> fixedIn(numProg, 1);
    fixedIn[0] = 0;
    for (NodeID v = 1; v < numProg; ++v) {
      if (p.sources_[v])
        fixedIn[v] = 0;
    }
    for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
      if (p.objects_[obj].initialStore)
        fixedIn[p.objects_[obj].initialStore] = 0;
    }
    for (AccessID id = 1; id < p.accesses_.size(); ++id) {
      if (p.accesses_[id].kind == Program::AccessKind::Load)
        fixedIn[p.accesses_[id].value] = 0;
    }
    for (FunctionID f = 1; f < p.functions_.size(); ++f) {
      if (p.functions_[f].returned)
        fixedIn[p.functions_[f].returned] = 0;
      for (NodeID param : p.functions_[f].formalParams)
        if (param)
          fixedIn[param] = 0;
    }
    for (CallSiteID s = 1; s < p.callSites_.size(); ++s) {
      if (p.callSites_[s].result)
        fixedIn[p.callSites_[s].result] = 0;
    }

    // 2. Iterative Tarjan SCC over `p.edges_` (1..numProg-1).
    constexpr std::uint32_t kUnvis = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> disc(numProg, kUnvis);
    std::vector<std::uint32_t> low(numProg, 0);
    std::vector<std::uint32_t> comp(numProg, 0);
    std::vector<std::uint8_t> onStack(numProg, 0);
    std::vector<NodeID> tarjanStack;
    tarjanStack.reserve(numProg);
    struct Frame {
      NodeID u;
      std::size_t edgeIdx;
    };
    std::vector<Frame> callStack;
    callStack.reserve(1024);
    std::uint32_t nextDisc = 0;
    std::uint32_t numComps = 0;
    for (NodeID start = 1; start < numProg; ++start) {
      if (disc[start] != kUnvis)
        continue;
      disc[start] = low[start] = nextDisc++;
      tarjanStack.push_back(start);
      onStack[start] = 1;
      callStack.push_back({start, 0});
      while (!callStack.empty()) {
        auto &top = callStack.back();
        const auto &edges = p.edges_[top.u];
        if (top.edgeIdx < edges.size()) {
          const NodeID v = edges[top.edgeIdx++];
          if (v == top.u)
            continue;
          if (disc[v] == kUnvis) {
            disc[v] = low[v] = nextDisc++;
            tarjanStack.push_back(v);
            onStack[v] = 1;
            callStack.push_back({v, 0});
          } else if (onStack[v] && disc[v] < low[top.u]) {
            low[top.u] = disc[v];
          }
        } else {
          const NodeID u = top.u;
          callStack.pop_back();
          if (!callStack.empty()) {
            const NodeID parent = callStack.back().u;
            if (low[u] < low[parent])
              low[parent] = low[u];
          }
          if (low[u] == disc[u]) {
            while (true) {
              const NodeID w = tarjanStack.back();
              tarjanStack.pop_back();
              onStack[w] = 0;
              comp[w] = numComps;
              if (w == u)
                break;
            }
            ++numComps;
          }
        }
      }
    }

    std::vector<NodeID> compRep(numComps, 0);
    std::vector<std::uint8_t> compFixed(numComps, 1);
    for (NodeID u = 1; u < numProg; ++u) {
      const std::uint32_t c = comp[u];
      if (compRep[c] == 0)
        compRep[c] = u;
      if (!fixedIn[u])
        compFixed[c] = 0;
    }
    std::vector<std::vector<std::uint32_t>> compPreds(numComps);
    for (NodeID u = 1; u < numProg; ++u) {
      const std::uint32_t cu = comp[u];
      for (NodeID v : p.edges_[u]) {
        const std::uint32_t cv = comp[v];
        if (cu != cv)
          compPreds[cv].push_back(cu);
      }
    }

    // 3. Walk condensed DAG in source-to-sink topological order
    //    (comp[u] > comp[v] for every DAG edge u -> v).
    struct PredVecHash {
      std::size_t operator()(const std::vector<NodeID> &v) const noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (NodeID x : v)
          h = (h ^ x) * 1099511628211ULL;
        return static_cast<std::size_t>(h);
      }
    };
    std::unordered_map<std::vector<NodeID>, NodeID, PredVecHash> predSigMap;
    std::vector<NodeID> compFinalRep(numComps, 0);
    std::vector<std::uint8_t> compIsDead(numComps, 0);
    NodeID deadRep = 0;
    std::vector<NodeID> preds;

    for (std::uint32_t ci = numComps; ci > 0; --ci) {
      const std::uint32_t c = ci - 1;
      if (!compFixed[c]) {
        compFinalRep[c] = compRep[c];
        continue;
      }
      preds.clear();
      for (std::uint32_t pc : compPreds[c]) {
        if (!compIsDead[pc])
          preds.push_back(compFinalRep[pc]);
      }
      if (preds.size() > 1) {
        std::sort(preds.begin(), preds.end());
        preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
      }
      if (preds.empty()) {
        if (!deadRep)
          deadRep = compRep[c];
        compFinalRep[c] = deadRep;
        compIsDead[c] = 1;
      } else if (preds.size() == 1) {
        compFinalRep[c] = preds[0];
      } else {
        auto [it, ins] = predSigMap.emplace(preds, compRep[c]);
        compFinalRep[c] = it->second;
      }
    }

    // Apply `vfgRep` mapping and count collapsed nodes.
    totalCollapsedNodes = 0;
    for (NodeID u = 1; u < numProg; ++u) {
      const NodeID r = compFinalRep[comp[u]];
      vfgRep[u] = r;
      if (r != u)
        ++totalCollapsedNodes;
    }

    // 4. Rebuild deduplicated `propAdj` over surviving representatives.
    for (NodeID u = 1; u < numProg; ++u) {
      const NodeID ru = vfgRep[u];
      if (ru == deadRep)
        continue;
      auto &out = propAdj[ru];
      for (NodeID v : p.edges_[u]) {
        const NodeID rv = vfgRep[v];
        if (rv != ru && rv != deadRep)
          out.push_back(rv);
      }
    }
    std::vector<NodeID> liveReps;
    liveReps.reserve(numProg - totalCollapsedNodes);
    for (NodeID u = 1; u < numProg; ++u) {
      if (vfgRep[u] != u)
        continue;
      liveReps.push_back(u);
      auto &out = propAdj[u];
      if (out.size() > 1) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
      }
    }

    // 5. Assign exact spaced topological ranks via iterative postorder DFS on
    //    the OVS-collapsed DAG so `nodeTopoRank[u] > nodeTopoRank[v]` holds for
    //    every edge `u -> v` and `pendingBackEdges` starts empty.
    std::vector<std::uint8_t> vis(numProg, 0);
    std::uint64_t nextRankIdx = 1;
    callStack.clear();
    for (NodeID start : liveReps) {
      if (vis[start])
        continue;
      vis[start] = 1;
      callStack.push_back({start, 0});
      while (!callStack.empty()) {
        auto &top = callStack.back();
        const auto &edges = propAdj[top.u];
        if (top.edgeIdx < edges.size()) {
          const NodeID v = edges[top.edgeIdx++];
          if (!vis[v]) {
            vis[v] = 1;
            callStack.push_back({v, 0});
          }
        } else {
          nodeTopoRank[top.u] = nextRankIdx << 20;
          ++nextRankIdx;
          callStack.pop_back();
        }
      }
    }
    topoRanksValid = true;
    pendingBackEdges.clear();
    uncompactedCollapsed = 0;
  }

  void reset(bool weakMode, bool preserveCallTargets) {
    weak = weakMode;
    flowTemplates.clear();
    globalWeakFlowCache.clear();
    objectFuncCache.assign(p.objects_.size(), {});
    lastEmitState.assign(p.objects_.size(), {});
    funcAccessedByObjects.assign(p.functions_.size(), {});
    adj.assign(p.edges_.size(), {});
    largeAdjSets.clear();
    vfgRep.resize(p.edges_.size());
    for (NodeID node = 0; node < p.edges_.size(); ++node)
      vfgRep[node] = node;
    propAdj.assign(p.edges_.size(), {});
    largePropAdjSets.clear();
    sccHookNodes.assign(p.edges_.size(), {});
    repHasUnknownStoreHook.assign(p.edges_.size(), 0);
    repHasExternalEscapeHook.assign(p.edges_.size(), 0);
    repHasCallBoundaryHook.assign(p.edges_.size(), 0);
    pendingHookDiffs.clear();
    edgesSinceCondense = 0;
    lastCondenseTrv = 0;
    totalCollapsedNodes = 0;
    uncompactedCollapsed = 0;
    forceCondense = false;
    topoRanksValid = false;
    pendingBackEdges.clear();
    incSccGen = 0;
    incVisGen.clear();
    incLocalIdx.clear();
    edgesCache.clear();
    edgesCacheBuilt = false;
    extraDirectEdges.clear();
    for (NodeID node = 1; node < p.edges_.size(); ++node)
      adj[node] = p.edges_[node];
    nodeTopoRank.assign(p.edges_.size(), 0);
    runOfflineCopySubstitution();
    ptsPool.clear();
    ptsPool.emplace_back(); // slot 0 = immutable empty set
    ptsRefs.assign(1, 1);
    ptsDrained.assign(1, 0);
    ptsReadOnlyJoin.assign(1, 0);
    freePtsIds.clear();
    ptsIdx.assign(p.edges_.size(), 0);
    ptsSlotsAtLastHashCons = 0;
    pted.clear();
    ptedBuilt = false;
    pendingObjSeeds.clear();
    pendingEdgeSeeds.clear();
    propWork = {};
    propDeltaSlot.assign(p.edges_.size(), 0);
    propDeltaPoolId.assign(p.edges_.size(), 0);
    propNeedsHook.assign(p.edges_.size(), 0);
    deltaPool.clear();
    freeDeltaSlots.clear();
    nodeHasHook.assign(p.edges_.size(), false);
    reachEpoch = 1;
    callGraphVersion = 1;
    dirtySigMark.assign(p.objects_.size(), 0);
    dirtySigList.clear();
    callGraphDirty.assign(p.objects_.size(), false);
    activeAccesses.assign(p.objects_.size(), {});
    pendingAccesses.assign(p.objects_.size(), {});
    escapePointers.assign(p.objects_.size(), {});
    escapePtrDirty.assign(p.objects_.size(), false);
    escapePtrNextDedup.assign(p.objects_.size(), 128);
    initialEscapeObjects.assign(p.objects_.size(), {});
    escapedIndirectCalls.assign(p.objects_.size(), {});
    blockedWitness.assign(p.objects_.size(), 0);
    blockedEmptyEpoch.assign(p.objects_.size(), 0);
    effectiveSingleton.assign(p.objects_.size(), false);
    effectiveOwner.assign(p.objects_.size(), 0);
    forcedWeak.assign(p.objects_.size(), false);
    for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
      effectiveSingleton[obj] = p.objects_[obj].singleton;
      effectiveOwner[obj] = p.objects_[obj].ownerFunction;
    }
    previous.assign(p.objects_.size(), {});
    processed.assign(p.objects_.size(), false);
    emittedFlow.assign(p.objects_.size(), false);
    uses.assign(p.edges_.size(), {});
    storedThrough.assign(p.edges_.size(), 0);
    initializedObject.assign(p.edges_.size(), 0);
    indirectCallUses.assign(p.edges_.size(), {});
    indirectArgUses.assign(p.edges_.size(), {});
    isIndirectCalleeLoad.assign(p.edges_.size(), false);
    isCallBoundaryNode.assign(p.edges_.size(), false);
    callBoundarySites.assign(p.edges_.size(), {});
    siteUnknownBoundaryMarked.assign(p.callSites_.size(), false);
    siteUnknownArgsEscaped.assign(p.callSites_.size(), false);
    isExternalEscapeNode.assign(p.edges_.size(), false);
    nodeHadMultiTarget.assign(p.edges_.size(), false);
    objCallExposed.assign(p.objects_.size(), false);
    objExternallyEscaped.assign(p.objects_.size(), false);
    externallyEscapedSet.clear();
    objEscapedToUnknown.assign(p.objects_.size(), false);
    escapedToUnknownSet.clear();
    nodeHasLoadUse.assign(p.edges_.size(), 0);
    skipUseObjSet.clear();
    isEmptyReadOnly.assign(p.objects_.size(), 0);
    entryReadOnlyWired.assign(p.functions_.size(), 0);
    allReadOnlyInitJoin = 0;
    readOnlySourceJoinCache.clear();
    lastBigROSubset.clear();
    lastBigROJoin = 0;
    callVisGen = 1;
    callVisMarkedGen.assign(p.objects_.size(), 0);
    escBlockGen = 0;
    escObjVisGen.assign(p.objects_.size(), 0);
    escPtrVisGen.assign(p.edges_.size(), 0);
    for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
      const bool global =
          p.objects_[obj].isGlobal || obj == p.unknownObject();
      objCallExposed[obj] = global;
      objExternallyEscaped[obj] = global;
      if (global)
        externallyEscapedSet.insertBit(obj);
    }
    reverseEscape.assign(p.objects_.size(), {});
    ptrEscapedBy.assign(p.edges_.size(), {});
    ptrHasExternalTarget.assign(p.edges_.size(), false);
    ptrHasUnknownTarget.assign(p.edges_.size(), false);
    unknownAccessVersion = 1;
    unknownSummaryVersion = 0;
    unknownSummaryChangeVersion = 1;
    unknownFnAccesses.clear();
    unknownDirectRef.assign(p.functions_.size(), false);
    unknownDirectRefFuncs.clear();
    unknownTransRef.assign(p.functions_.size() + 1, false);
    unknownReadEntry.assign(p.functions_.size() + 1, 0);
    unknownModSummary.assign(p.functions_.size() + 1, 0);
    unknownSummaryNodes.clear();
    objLocalOnly.assign(p.objects_.size(), false);
    indirectGen = 0;
    fnDirectModGen.assign(p.functions_.size() + 1, 0);
    fnDirectRefGen.assign(p.functions_.size() + 1, 0);
    fnDirectIdx.assign(p.functions_.size() + 1, 0);
    fnTransModGen.assign(p.functions_.size() + 1, 0);
    fnTransRefGen.assign(p.functions_.size() + 1, 0);
    fnTransStrongGen.assign(p.functions_.size() + 1, 0);
    fnActiveGen.assign(p.functions_.size() + 1, 0);
    fnActiveIdx.assign(p.functions_.size() + 1, 0);
    fnSubgraphGen.assign(p.functions_.size() + 1, 0);
    siteCarryGen.assign(p.callSites_.size(), 0);
    siteCarryVal.assign(p.callSites_.size(), 0);
    fnLocalExitUnkStoreNode.assign(p.functions_.size() + 1, 0);
    fnLocalUnkLoadNode.assign(p.functions_.size() + 1, 0);
    fnHasNonExitUnkStore.assign(p.functions_.size() + 1, 0);
    fnLocalUnkAccessWired.assign(p.accesses_.size(), 0);
    alwaysCarryVersion = 1;
    objCarrySites.assign(p.objects_.size(), {});
    univSccs.clear();
    univSccs.emplace_back(); // index 0 unused
    fnUnivScc.assign(p.functions_.size() + 1, 0);
    univSccCGVer = 0;
    univSccAlwaysVer = 0;
    univSccUnkVer = 0;
    univSccBuildId = 0;
    nextUnivStructVersion = 1;
    nextUnivStateVer = 1;
    univDirtyGen = 0;
    univDirtyStamp.assign(1, 0);
    fnStateVer.assign(p.functions_.size() + 1, 1);
    entryUnkRefVersion = 1;
    hubGlobalVersion = 1;
    localCap.reset();
    weakAncCtxs.clear();
    weakAncCtxs.emplace_back(); // index 0 unused
    weakAncCtxDirtyStamp.assign(1, 0);
    funcWeakAncCtxs.assign(p.functions_.size() + 1, {});
    funcWeakAncCoreWatch.assign(p.functions_.size() + 1, {});
    objWeakAncCtx.assign(p.objects_.size(), 0);
    weakAncSrcJoinCache.clear();
    weakAncSnkJoinCache.clear();
    weakAncSummaryIntern.clear();
    nextWeakAncSummaryId = 1;
    weakAncPreCache.clear();
    weakAncPostCache.clear();
    hubExtSummary.invalidate();
    hubExtBuiltGlobalVer = 0;
    hubExtBuiltUnivBuild = 0;
    anyUnresolvedIndirectCall = false;
    pending.clear();
    finalStrong.clear();

    funcCFGs.assign(p.functions_.size(), {});
    funcReachable.assign(p.functions_.size(), false);
    blockReachable.assign(p.blocks_.size(), false);
    blockToLocal.assign(p.blocks_.size(), InvalidLocal);
    accessEventPos.assign(p.accesses_.size(), 0);
    callEventPos.assign(p.callSites_.size(), 0);
    funcCallSites.assign(p.functions_.size(), {});
    funcHasUnresolvedIndirectCall.assign(p.functions_.size(), false);

    liveCallTargets.assign(p.callSites_.size(), {});
    liveCallExternal.assign(p.callSites_.size(), false);
    siteCallsUnknownHub.assign(p.callSites_.size(), false);
    unknownHubCallerSites.clear();
    unknownHubActive = false;
    unknownHubHasVoidReturn = false;
    unknownHubArgNodes.clear();
    unknownHubRetNode = 0;
    callers.assign(p.functions_.size(), {});
    callerSites.assign(p.functions_.size(), {});
    funcRecursive.assign(p.functions_.size(), false);
    pendingIndirectSites.clear();
    if (!preserveCallTargets) {
      persistentCallTargets.assign(p.callSites_.size(), {});
      persistentCallExternal.assign(p.callSites_.size(), false);
      persistentCallUnknownHub.assign(p.callSites_.size(), false);
    }
    cgGen = 0;
    cgVisitGen.assign(p.functions_.size() + 1, 0);
    cgAncGen.assign(p.functions_.size() + 1, 0);

    addressTakenFuncs.clear();
    isAddressTakenFunc.assign(p.functions_.size(), false);
    for (FunctionID f = 1; f < p.functions_.size(); ++f) {
      if (p.functions_[f].addressTaken) {
        addressTakenFuncs.push_back(f);
        isAddressTakenFunc[f] = true;
      }
    }
    funcObjectSet.clear();
    nonAddrTakenFuncObjSet.clear();
    readOnlyWithInitSet.clear();
    for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
      if (const FunctionID fn = p.functionOfObject_[obj]) {
        funcObjectSet.insertBit(obj);
        if (!isAddressTakenFunc[fn])
          nonAddrTakenFuncObjSet.insertBit(obj);
      }
      if (p.objects_[obj].memory && p.objects_[obj].readOnly) {
        skipUseObjSet.insertBit(obj);
        if (p.initializerKinds_[obj] == Program::InitializerKind::Empty) {
          isEmptyReadOnly[obj] = 1;
        } else {
          readOnlyWithInitSet.insertBit(obj);
        }
      }
    }
    siteIndirectSaturated.assign(p.callSites_.size(), 0);
    resolvedTargetsCache.clear();

    stats.indirectEdges = 0;
    stats.strongUpdateSites = 0;
    stats.expandedControlFlowNodes = 0;
    stats.iteratedDominanceFrontierNodes = 0;
    stats.sparseGraphNodes = 0;
    stats.sparseGraphEdges = 0;
    stats.valueFlowJoinNodes = 0;
    stats.interproceduralAuxEdges = 0;

    // Activate all entry functions (and implicit root function 0 if roots exist).
    for (FunctionID entry : p.entryFunctions_)
      activateFunction(entry);
    updateRecursionStatus();
    updateUnresolvedIndirectCallFlags();

    for (ObjectID object = 1; object < p.objects_.size(); ++object) {
      pendingObjSeeds.emplace_back(object, p.objects_[object].address);
      if (!p.objects_[object].memory || isEmptyReadOnly[object])
        continue;
      if (!p.objects_[object].readOnly) {
        markDirtySig(object);
        pending.insert(object);
      }
      const auto initKind = p.initializerKinds_[object];
      if (initKind == Program::InitializerKind::Empty)
        continue;
      const NodeID initializer =
          initKind == Program::InitializerKind::Explicit
              ? p.initializers_[object]
              : p.unknownValue();
      insertAdj(initializer, p.objects_[object].initialStore);
      initializedObject[p.objects_[object].initialStore] = object;
      enableNodeHook(p.objects_[object].initialStore);
    }
  }

  //===--------------------------------------------------------------------===//
  // Algorithm 1: Incremental Bulk ComputePtedSet + Transitive ComputeEspSet
  //===--------------------------------------------------------------------===//

  void computePointedToBy() {
    condenseVFGSCCs();

    const NodeID numProg = static_cast<NodeID>(p.edges_.size());
    const std::size_t ptsPoolStart = ptsPool.size();
    bool propagatedAny = false;
    std::vector<CallSiteID> unknownBoundarySites;
    std::vector<ObjectID> freshUnkScratch;

    struct DiffDrainState {
      bool unkEsc = false;
      bool extEsc = false;
      bool callVis = false;
    };

    auto drainUnknownEsc = [&](const PointsToSet &diff, DiffDrainState &drain) {
      if (drain.unkEsc)
        return;
      drain.unkEsc = true;
      freshUnkScratch.clear();
      diff.forEachNotIn(escapedToUnknownSet, [&](ObjectID object) {
        if (p.objects_[object].memory)
          freshUnkScratch.push_back(object);
      });
      for (ObjectID object : freshUnkScratch)
        markEscapedToUnknown(object);
    };

    auto drainExternalEsc = [&](const PointsToSet &diff,
                                DiffDrainState &drain) {
      if (drain.extEsc)
        return;
      drain.extEsc = true;
      freshUnkScratch.clear();
      diff.forEachNotIn(externallyEscapedSet, [&](ObjectID object) {
        if (object != p.unknownObject())
          freshUnkScratch.push_back(object);
      });
      for (ObjectID object : freshUnkScratch)
        markExternalEscape(object);
    };

    auto drainCallVis = [&](const PointsToSet &diff, DiffDrainState &drain) {
      if (drain.callVis)
        return;
      drain.callVis = true;
      freshUnkScratch.clear();
      diff.forEachNotIn(externallyEscapedSet, [&](ObjectID object) {
        freshUnkScratch.push_back(object);
      });
      for (ObjectID object : freshUnkScratch) {
        objCallExposed[object] = true;
        markCallVisibilityDirty(object);
      }
    };

    auto processNodeDiff = [&](NodeID node, const PointsToSet &diff,
                               DiffDrainState &drain) {
      if (const ObjectID initObj = initializedObject[node]) {
        if (initObj == p.unknownObject() || objEscapedToUnknown[initObj]) {
          drainUnknownEsc(diff, drain);
        } else if (!drain.unkEsc) {
          diff.forEachNotIn(escapedToUnknownSet, [&](ObjectID object) {
            if (!p.objects_[object].memory)
              return;
            if (initialEscapeObjects[object].insertBit(initObj))
              recordReverseEscape(initObj, object);
          });
        }
      } else if (const NodeID ptr = storedThrough[node]) {
        bool ptrUnk = ptrHasUnknownTarget[ptr];
        if (!ptrUnk && ptsOf(ptr).count(p.unknownObject())) {
          ptrHasUnknownTarget[ptr] = true;
          ptrHasExternalTarget[ptr] = true;
          ptrUnk = true;
          for (ObjectID c : ptrEscapedBy[ptr])
            markEscapedToUnknown(c);
          ptrEscapedBy[ptr].clear();
        }
        if (ptrUnk) {
          drainUnknownEsc(diff, drain);
        } else if (!drain.unkEsc) {
          bool ptrExt = ptrHasExternalTarget[ptr];
          bool addedEsc = false;
          const auto &ptrPts = ptsOf(ptr);
          freshUnkScratch.clear();
          diff.forEachNotIn(escapedToUnknownSet, [&](ObjectID object) {
            if (p.objects_[object].memory)
              freshUnkScratch.push_back(object);
          });
          for (ObjectID object : freshUnkScratch) {
            addEscapePointer(object, ptr);
            if (objExternallyEscaped[object])
              continue;
            if (ptrExt) {
              markExternalEscape(object);
              continue;
            }
            if (ptrEscapedBy[ptr].insertBit(object)) {
              addedEsc = true;
              for (ObjectID target : ptrPts) {
                if (!p.objects_[target].memory)
                  continue;
                if (objExternallyEscaped[target]) {
                  ptrHasExternalTarget[ptr] = true;
                  ptrExt = true;
                  for (ObjectID c : ptrEscapedBy[ptr])
                    markExternalEscape(c);
                  ptrEscapedBy[ptr].clear();
                  break;
                }
                recordReverseEscape(target, object);
              }
            }
          }
          if (addedEsc && !ptrExt)
            enableNodeHook(ptr);
        }
      }
      if (!ptrEscapedBy[node].empty()) {
        if (!ptrHasUnknownTarget[node] && diff.count(p.unknownObject())) {
          ptrHasUnknownTarget[node] = true;
          ptrHasExternalTarget[node] = true;
          for (ObjectID child : ptrEscapedBy[node])
            markEscapedToUnknown(child);
          ptrEscapedBy[node].clear();
        } else {
          bool anyExt = ptrHasExternalTarget[node];
          if (!anyExt) {
            for (ObjectID object : diff) {
              if (p.objects_[object].memory && objExternallyEscaped[object]) {
                anyExt = true;
                break;
              }
            }
          }
          if (anyExt) {
            ptrHasExternalTarget[node] = true;
            for (ObjectID child : ptrEscapedBy[node])
              markExternalEscape(child);
            ptrEscapedBy[node].clear();
          } else {
            for (ObjectID object : diff) {
              if (!p.objects_[object].memory)
                continue;
              for (ObjectID child : ptrEscapedBy[node])
                recordReverseEscape(object, child);
            }
          }
        }
      }
      if (isCallBoundaryNode[node]) {
        if (diff.count(p.unknownObject())) {
          objCallExposed[p.unknownObject()] = true;
          for (CallSiteID site : callBoundarySites[node])
            if (!siteUnknownBoundaryMarked[site])
              unknownBoundarySites.push_back(site);
        }
        bool anyNonAlways = false;
        for (CallSiteID site : callBoundarySites[node]) {
          if (!siteAlwaysCarries(site)) {
            anyNonAlways = true;
            break;
          }
        }
        if (anyNonAlways) {
          diff.forEachNotIn(externallyEscapedSet, [&](ObjectID obj) {
            if (!p.objects_[obj].memory)
              return;
            for (CallSiteID site : callBoundarySites[node]) {
              if (!siteAlwaysCarries(site))
                objCarrySites[obj].insertBit(site);
            }
          });
        }
        drainCallVis(diff, drain);
      }
      if (isExternalEscapeNode[node])
        drainExternalEsc(diff, drain);
      if (!indirectCallUses[node].empty()) {
        for (CallSiteID site : indirectCallUses[node])
          if (!siteIndirectSaturated[site])
            pendingIndirectSites.insert(site);
      }
      if (!indirectArgUses[node].empty() && !drain.unkEsc) {
        std::vector<CallSiteID> unresolvedSites;
        for (CallSiteID site : indirectArgUses[node]) {
          if (liveCallTargets[site].empty() && !liveCallExternal[site] &&
              !siteCallsUnknownHub[site]) {
            unresolvedSites.push_back(site);
          }
        }
        if (!unresolvedSites.empty()) {
          diff.forEachNotIn(escapedToUnknownSet, [&](ObjectID object) {
            for (CallSiteID site : unresolvedSites)
              appendUnique(escapedIndirectCalls[object], site);
          });
        }
      }
      if (!uses[node].empty()) {
        const auto &nodeUses = uses[node];
        if (nodeHasLoadUse[node] && !readOnlyWithInitSet.empty()) {
          const NodeID rNode = findRep(node);
          const std::uint32_t nodePoolId = ptsIdx[rNode];
          NodeID roSrc = kNoReadOnlyJoin;
          if (nodePoolId != 0 && diff.size() == ptsPool[nodePoolId].size()) {
            roSrc = getOrCreatePoolReadOnlyJoin(nodePoolId);
          } else {
            PointsToSet roSub;
            diff.forEachIn(readOnlyWithInitSet,
                           [&](ObjectID obj) { roSub.insertBit(obj); });
            if (!roSub.empty())
              roSrc = getOrCreateReadOnlySourceJoin(roSub);
          }
          if (roSrc != kNoReadOnlyJoin) {
            for (AccessID id : nodeUses) {
              const auto &acc = p.accesses_[id];
              if (acc.kind == Program::AccessKind::Load &&
                  blockReachable[acc.block])
                addIndirect(roSrc, acc.value);
            }
          }
        }
        auto handleUseObj = [&](ObjectID object) {
          if (!p.objects_[object].memory || p.objects_[object].readOnly)
            return;
          auto &pa = pendingAccesses[object];
          for (AccessID id : nodeUses) {
            pa.push_back(id);
            if (object == p.unknownObject())
              recordUnknownAccess(id);
          }
          if (pa.size() >= 1024)
            flushPendingAccesses(object);
          markDirtySig(object);
        };
        diff.forEachNotIn(skipUseObjSet, handleUseObj);
        const auto &nodePts = ptsOf(node);
        if (!nodeHadMultiTarget[node] && nodePts.size() >= 2) {
          nodeHadMultiTarget[node] = true;
          for (ObjectID target : nodePts)
            if (canStrongUpdate(target))
              markDirtySig(target);
        }
      }
    };

    auto repHasAnyHook = [&](NodeID r) -> bool {
      return (r < numProg && nodeHasHook[r]) || !sccHookNodes[r].empty() ||
             repHasUnknownStoreHook[r] || repHasExternalEscapeHook[r] ||
             repHasCallBoundaryHook[r];
    };

    auto fireHooks = [&](NodeID rep, const PointsToSet &d,
                         DiffDrainState &drain) {
      if (repHasUnknownStoreHook[rep])
        drainUnknownEsc(d, drain);
      if (repHasExternalEscapeHook[rep])
        drainExternalEsc(d, drain);
      if (repHasCallBoundaryHook[rep])
        drainCallVis(d, drain);
      if (rep < numProg && nodeHasHook[rep])
        processNodeDiff(rep, d, drain);
      const std::size_t nHooks = sccHookNodes[rep].size();
      for (std::size_t hi = 0; hi < nHooks; ++hi)
        processNodeDiff(sccHookNodes[rep][hi], d, drain);
    };

    PointsToSet diff;
    PointsToSet curDelta;

    // Per-wave memoized COW merge caches: when thousands of nodes share a large
    // PointsToSet `oldId` and receive the same delta (`onlyObj`, a pinned
    // `fullSrcId` pool slot, or an arbitrary `curDelta`), clone & merge ONCE
    // and share the resulting pinned `newId` across all of them in O(1).
    struct WavePoolEntry {
      std::uint32_t newId = 0;
      std::uint32_t diffPoolId = 0;
      PointsToSet smallDiff;
    };
    struct WaveMultiEntry {
      std::uint32_t oldId = 0;
      std::uint32_t newId = 0;
      std::uint32_t diffPoolId = 0;
      PointsToSet delta;
      PointsToSet smallDiff;
    };
    std::unordered_map<std::uint64_t, std::uint32_t> singleMergeCache;
    std::unordered_map<std::uint64_t, WavePoolEntry> poolMergeCache;
    std::unordered_map<std::uint64_t, WaveMultiEntry> multiMergeCache;
    auto flushSingleCache = [&]() {
      for (const auto &kv : singleMergeCache) {
        const std::uint32_t oldId = static_cast<std::uint32_t>(kv.first >> 32);
        releasePtsSlot(oldId);
        if (kv.second)
          releasePtsSlot(kv.second);
      }
      singleMergeCache.clear();
    };
    auto flushPoolCache = [&]() {
      for (const auto &kv : poolMergeCache) {
        const std::uint32_t oldId = static_cast<std::uint32_t>(kv.first >> 32);
        const std::uint32_t srcId =
            static_cast<std::uint32_t>(kv.first & 0xffffffffULL);
        releasePtsSlot(oldId);
        releasePtsSlot(srcId);
        if (kv.second.newId != oldId)
          releasePtsSlot(kv.second.newId);
        if (kv.second.diffPoolId)
          releasePtsSlot(kv.second.diffPoolId);
      }
      poolMergeCache.clear();
    };
    auto flushMultiCache = [&]() {
      for (const auto &kv : multiMergeCache) {
        releasePtsSlot(kv.second.oldId);
        if (kv.second.newId != kv.second.oldId)
          releasePtsSlot(kv.second.newId);
        if (kv.second.diffPoolId)
          releasePtsSlot(kv.second.diffPoolId);
      }
      multiMergeCache.clear();
    };
    struct WaveCacheGuard {
      decltype(flushSingleCache) &flushSingle;
      decltype(flushPoolCache) &flushPool;
      decltype(flushMultiCache) &flushMulti;
      ~WaveCacheGuard() {
        flushSingle();
        flushPool();
        flushMulti();
      }
    } waveCacheGuard{flushSingleCache, flushPoolCache, flushMultiCache};

    auto insertBitRepCached = [&](NodeID rv, ObjectID onlyObj) -> bool {
      const std::uint32_t oldId = ptsIdx[rv];
      if (oldId != 0 && ptsRefs[oldId] > 1 && ptsPool[oldId].size() >= 16) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(oldId) << 32) | onlyObj;
        auto it = singleMergeCache.find(key);
        if (it != singleMergeCache.end()) {
          if (it->second == 0)
            return false;
          setPtsIdx(rv, it->second);
          return true;
        }
        if (singleMergeCache.size() >= 4096)
          flushSingleCache();
        if (ptsPool[oldId].count(onlyObj)) {
          ++ptsRefs[oldId];
          singleMergeCache.emplace(key, 0);
          return false;
        }
        const std::uint32_t nid = allocPtsSlot();
        ptsPool[nid] = ptsPool[oldId];
        ptsPool[nid].insertBit(onlyObj);
        setPtsIdx(rv, nid);
        ++ptsRefs[oldId];
        ++ptsRefs[nid];
        singleMergeCache.emplace(key, nid);
        return true;
      }
      return insertBitRep(rv, onlyObj);
    };

    while (hasPendingPropagation()) {
      if (!pendingHookDiffs.empty()) {
        auto hBatches = std::move(pendingHookDiffs);
        pendingHookDiffs.clear();
        for (const auto &batch : hBatches) {
          propagatedAny = true;
          DiffDrainState batchDrain;
          if (batch.unkEscHook)
            drainUnknownEsc(batch.diff, batchDrain);
          if (batch.extEscHook)
            drainExternalEsc(batch.diff, batchDrain);
          if (batch.callVisHook)
            drainCallVis(batch.diff, batchDrain);
          for (NodeID hNode : batch.nodes)
            processNodeDiff(hNode, batch.diff, batchDrain);
        }
      }

      if (!pendingObjSeeds.empty()) {
        auto seeds = std::move(pendingObjSeeds);
        pendingObjSeeds.clear();
        for (const auto &[object, seed] : seeds) {
          const NodeID rSeed = findRep(seed);
          if (insertBitRepCached(rSeed, object)) {
            propagatedAny = true;
            const bool hasHook = repHasAnyHook(rSeed);
            if (hasHook || !propAdj[rSeed].empty())
              enqueueDeltaBit(rSeed, object, hasHook);
          }
        }
      }

      if (!pendingEdgeSeeds.empty()) {
        auto edgeSeeds = std::move(pendingEdgeSeeds);
        pendingEdgeSeeds.clear();
        for (const auto &[from, to] : edgeSeeds) {
          const NodeID rFrom = findRep(from);
          const NodeID rTo = findRep(to);
          if (rFrom == rTo)
            continue;
          const std::uint32_t srcPoolId = ptsIdx[rFrom];
          if (srcPoolId == 0)
            continue;
          const std::uint32_t dstOldId = ptsIdx[rTo];
          if (dstOldId == srcPoolId)
            continue;
          const bool hasHook = repHasAnyHook(rTo);
          const bool hasOut = !propAdj[rTo].empty();
          if (dstOldId == 0) {
            setPtsIdx(rTo, srcPoolId);
            propagatedAny = true;
            if (hasHook || hasOut)
              enqueueDeltaPoolId(rTo, srcPoolId, hasHook);
          } else if (uniteRepWithDiffById(rTo, srcPoolId, hasHook || hasOut,
                                           diff)) {
            propagatedAny = true;
            if (hasHook || hasOut)
              enqueueDeltaSet(rTo, diff, hasHook);
          }
        }
      }

      while (!propWork.empty()) {
        const NodeID u = propWork.top().second;
        propWork.pop();
        const std::uint32_t slot = propDeltaSlot[u];
        const std::uint32_t dPid = propDeltaPoolId[u];
        propDeltaSlot[u] = 0;
        propDeltaPoolId[u] = 0;
        const bool doHook = (propNeedsHook[u] != 0);
        propNeedsHook[u] = 0;

        const std::uint32_t uPoolId = ptsIdx[u];
        bool isFullSlot = false;
        const PointsToSet *activeDeltaPtr = nullptr;
        if (dPid != 0) {
          if (ptsPool[dPid].empty()) {
            releasePtsSlot(dPid);
            continue;
          }
          isFullSlot =
              (uPoolId != 0 && ptsPool[dPid].size() == ptsPool[uPoolId].size());
          if (doHook && repHasAnyHook(u)) {
            DiffDrainState popDrain;
            if (isFullSlot) {
              const std::uint8_t dm = ptsDrained[uPoolId];
              popDrain.unkEsc = (dm & 1U) != 0;
              popDrain.extEsc = (dm & 2U) != 0;
              popDrain.callVis = (dm & 4U) != 0;
            }
            fireHooks(u, ptsPool[dPid], popDrain);
            if (isFullSlot && ptsIdx[u] == uPoolId) {
              std::uint8_t dm = ptsDrained[uPoolId];
              if (popDrain.unkEsc)
                dm |= 1U;
              if (popDrain.extEsc)
                dm |= 2U;
              if (popDrain.callVis)
                dm |= 4U;
              ptsDrained[uPoolId] = dm;
            }
          }
          if (propAdj[u].empty()) {
            releasePtsSlot(dPid);
            continue;
          }
          // `ptsPool` is a `std::deque`, so `&ptsPool[dPid]` stays valid across
          // `allocPtsSlot()` growth with zero copy until released after loop.
          activeDeltaPtr = &ptsPool[dPid];
        } else {
          curDelta.swap(deltaPool[slot - 1]);
          if (deltaPool[slot - 1].size() >= 64)
            PointsToSet().swap(deltaPool[slot - 1]);
          else
            deltaPool[slot - 1].clear();
          freeDeltaSlots.push_back(slot);
          if (curDelta.empty())
            continue;
          isFullSlot =
              (uPoolId != 0 && curDelta.size() == ptsPool[uPoolId].size());
          if (doHook && repHasAnyHook(u)) {
            DiffDrainState popDrain;
            if (isFullSlot) {
              const std::uint8_t dm = ptsDrained[uPoolId];
              popDrain.unkEsc = (dm & 1U) != 0;
              popDrain.extEsc = (dm & 2U) != 0;
              popDrain.callVis = (dm & 4U) != 0;
            }
            fireHooks(u, curDelta, popDrain);
            if (isFullSlot && ptsIdx[u] == uPoolId) {
              std::uint8_t dm = ptsDrained[uPoolId];
              if (popDrain.unkEsc)
                dm |= 1U;
              if (popDrain.extEsc)
                dm |= 2U;
              if (popDrain.callVis)
                dm |= 4U;
              ptsDrained[uPoolId] = dm;
            }
          }
          if (propAdj[u].empty())
            continue;
          activeDeltaPtr = &curDelta;
        }

        const PointsToSet &activeDelta = *activeDeltaPtr;
        ++stats.objectTraversals;
        if (activeDelta.size() == 1) {
          const ObjectID onlyObj = *activeDelta.begin();
          for (NodeID v : propAdj[u]) {
            const NodeID rv = findRep(v);
            if (rv == u)
              continue;
            if (insertBitRepCached(rv, onlyObj)) {
              propagatedAny = true;
              const bool hasHook = repHasAnyHook(rv);
              if (hasHook || !propAdj[rv].empty())
                enqueueDeltaBit(rv, onlyObj, hasHook);
            }
          }
        } else {
          // If `dPid != 0` or `isFullSlot`, we already have an immutable pool
          // ID `fullSrcId`; otherwise pin ONE copy when fanning out a large
          // delta to >= 2 successors so empty/subset successors share in O(1)
          // and `poolMergeCache` matches by integer ID with zero hashing/copy.
          std::uint32_t tempDeltaSrcId = 0;
          std::uint32_t fullSrcId =
              (dPid != 0) ? dPid : (isFullSlot ? uPoolId : 0);
          if (fullSrcId == 0 && activeDelta.size() >= 32 &&
              propAdj[u].size() >= 2) {
            tempDeltaSrcId = allocPtsSlot();
            ptsPool[tempDeltaSrcId] = activeDelta;
            fullSrcId = tempDeltaSrcId;
          }
          bool dHashComputed = false;
          std::uint64_t dHash = 0;
          for (NodeID v : propAdj[u]) {
            const NodeID rv = findRep(v);
            if (rv == u)
              continue;
            const std::uint32_t oldId = ptsIdx[rv];
            if (uPoolId != 0 && oldId == uPoolId)
              continue;
            if (fullSrcId != 0 && oldId == fullSrcId)
              continue;
            const bool hasHook = repHasAnyHook(rv);
            const bool hasOut = !propAdj[rv].empty();
            const bool needDiff = hasHook || hasOut;
            const PointsToSet *diffPtr = nullptr;
            std::uint32_t diffPoolId = 0;
            bool changed = false;

            if (fullSrcId != 0 && oldId != 0 && ptsRefs[oldId] > 1 &&
                ptsPool[oldId].size() >= 32) {
              const std::uint64_t key =
                  (static_cast<std::uint64_t>(oldId) << 32) | fullSrcId;
              auto it = poolMergeCache.find(key);
              if (it != poolMergeCache.end()) {
                if (it->second.newId != oldId) {
                  setPtsIdx(rv, it->second.newId);
                  diffPoolId = it->second.diffPoolId;
                  if (diffPoolId == 0)
                    diffPtr = &it->second.smallDiff;
                  changed = true;
                }
              } else {
                if (poolMergeCache.size() >= 4096)
                  flushPoolCache();
                changed = uniteRepWithDiff(rv, activeDelta, fullSrcId,
                                           /*needDiff=*/true, diff, diffPtr);
                const std::uint32_t newId = ptsIdx[rv];
                ++ptsRefs[oldId];
                ++ptsRefs[fullSrcId];
                if (newId != oldId)
                  ++ptsRefs[newId];
                WavePoolEntry entry;
                entry.newId = newId;
                if (changed && diffPtr) {
                  if (diffPtr == &activeDelta &&
                      ptsPool[fullSrcId].size() >= 16) {
                    entry.diffPoolId = fullSrcId;
                    ++ptsRefs[fullSrcId];
                    diffPoolId = fullSrcId;
                  } else if (diffPtr->size() >= 16) {
                    const std::uint32_t dSlot = allocPtsSlot();
                    ptsPool[dSlot] = *diffPtr;
                    entry.diffPoolId = dSlot;
                    diffPoolId = dSlot;
                  } else {
                    entry.smallDiff = *diffPtr;
                  }
                }
                auto [insIt, _] =
                    poolMergeCache.emplace(key, std::move(entry));
                if (changed && diffPoolId == 0)
                  diffPtr = &insIt->second.smallDiff;
              }
            } else if (oldId != 0 && ptsRefs[oldId] > 1 &&
                       ptsPool[oldId].size() >= 32) {
              if (!dHashComputed) {
                dHash = activeDelta.hash();
                dHashComputed = true;
              }
              const std::uint64_t key =
                  (static_cast<std::uint64_t>(oldId) * 0x9e3779b97f4a7c15ULL) ^
                  dHash;
              auto it = multiMergeCache.find(key);
              if (it != multiMergeCache.end() && it->second.oldId == oldId &&
                  it->second.delta == activeDelta) {
                if (it->second.newId != oldId) {
                  setPtsIdx(rv, it->second.newId);
                  diffPoolId = it->second.diffPoolId;
                  if (diffPoolId == 0)
                    diffPtr = &it->second.smallDiff;
                  changed = true;
                }
              } else {
                if (it == multiMergeCache.end() &&
                    multiMergeCache.size() >= 1024) {
                  flushMultiCache();
                  it = multiMergeCache.end();
                }
                const bool cacheMiss = (it == multiMergeCache.end());
                changed = uniteRepWithDiff(rv, activeDelta, fullSrcId,
                                           needDiff || cacheMiss, diff,
                                           diffPtr);
                if (cacheMiss) {
                  const std::uint32_t newId = ptsIdx[rv];
                  ++ptsRefs[oldId];
                  if (newId != oldId)
                    ++ptsRefs[newId];
                  WaveMultiEntry entry;
                  entry.oldId = oldId;
                  entry.newId = newId;
                  entry.delta = activeDelta;
                  if (changed && diffPtr) {
                    if (diffPtr->size() >= 16) {
                      const std::uint32_t dSlot = allocPtsSlot();
                      ptsPool[dSlot] = *diffPtr;
                      entry.diffPoolId = dSlot;
                      diffPoolId = dSlot;
                    } else {
                      entry.smallDiff = *diffPtr;
                    }
                  }
                  auto [insIt, _] =
                      multiMergeCache.emplace(key, std::move(entry));
                  if (changed && diffPoolId == 0)
                    diffPtr = &insIt->second.smallDiff;
                }
              }
            } else {
              changed = uniteRepWithDiff(rv, activeDelta, fullSrcId, needDiff,
                                         diff, diffPtr);
              if (changed && diffPtr == &activeDelta && fullSrcId != 0)
                diffPoolId = fullSrcId;
            }

            if (changed) {
              propagatedAny = true;
              if (needDiff) {
                if (diffPoolId != 0)
                  enqueueDeltaPoolId(rv, diffPoolId, hasHook);
                else if (diffPtr)
                  enqueueDeltaSet(rv, *diffPtr, hasHook);
              }
            }
          }
          if (tempDeltaSrcId != 0)
            releasePtsSlot(tempDeltaSrcId);
        }
        if (dPid != 0)
          releasePtsSlot(dPid);
      }
    }

    // Reclaim large delta buffers once the wave finishes (`propWork` is empty),
    // and deduplicate large PointsToSets if the wave allocated many new slots.
    if (deltaPool.size() > 256) {
      std::vector<PointsToSet>().swap(deltaPool);
      freeDeltaSlots.clear();
    }
    flushSingleCache();
    flushPoolCache();
    flushMultiCache();
    if (ptsPool.size() > ptsPoolStart + 2048 ||
        ptsPool.size() > ptsSlotsAtLastHashCons + 4096) {
      hashConsLargePts();
      ptsSlotsAtLastHashCons = ptsPool.size();
    }
    if (propagatedAny)
      ++reachEpoch;
    for (CallSiteID site : unknownBoundarySites)
      markCallSiteUnknownBoundary(site);
  }

  Signature signature(ObjectID object) {
    flushPendingAccesses(object);
    Signature result;
    result.accesses = activeAccesses[object];
    if (canStrongUpdate(object)) {
      for (AccessID id : result.accesses) {
        const auto &access = p.accesses_[id];
        const auto &ptrPts = ptsOf(access.pointer);
        if (access.allowStrong && access.kind == Program::AccessKind::Store &&
            ptrPts.size() == 1 && ptrPts.count(object))
          result.strong.insertBit(id);
      }
    }
    return result;
  }

  void ensureUnknownSummary() {
    if (unknownSummaryVersion == unknownAccessVersion)
      return;
    unknownSummaryVersion = unknownAccessVersion;

    const std::size_t numFuncs = p.functions_.size();
    const FunctionID hubIdx = static_cast<FunctionID>(numFuncs);
    const bool hadUnknownDirectRef = !unknownDirectRefFuncs.empty();
    unknownDirectRefFuncs.clear();
    for (FunctionID f = 0; f < numFuncs; ++f) {
      if (funcReachable[f] && unknownDirectRef[f])
        unknownDirectRefFuncs.push_back(f);
    }

    std::vector<std::vector<FunctionID>> cgSucc(numFuncs + 1);
    std::vector<std::vector<FunctionID>> cgPred(numFuncs + 1);
    for (FunctionID f = 0; f < numFuncs; ++f) {
      if (!funcReachable[f])
        continue;
      for (CallSiteID site : funcCallSites[f]) {
        for (FunctionID callee : liveCallTargets[site]) {
          if (funcReachable[callee]) {
            cgSucc[f].push_back(callee);
            cgPred[callee].push_back(f);
          }
        }
      }
    }
    if (unknownHubActive) {
      for (CallSiteID site : unknownHubCallerSites) {
        const BlockID blk = p.callSites_[site].block;
        if (!blockReachable[blk])
          continue;
        const FunctionID caller = p.blocks_[blk].function;
        cgSucc[caller].push_back(hubIdx);
        cgPred[hubIdx].push_back(caller);
      }
      for (FunctionID callee : addressTakenFuncs) {
        if (funcReachable[callee]) {
          cgSucc[hubIdx].push_back(callee);
          cgPred[callee].push_back(hubIdx);
        }
      }
    }
    auto [comp, numComps] = computeSCCs(cgSucc, cgPred);
    std::vector<std::vector<FunctionID>> compFuncs(numComps);
    for (FunctionID f = 0; f < numFuncs; ++f) {
      if (funcReachable[f])
        compFuncs[comp[f]].push_back(f);
    }
    if (unknownHubActive)
      compFuncs[comp[hubIdx]].push_back(hubIdx);

    const std::vector<bool> prevTransRef = unknownTransRef;
    const std::vector<NodeID> prevModSummary = unknownModSummary;
    unknownTransRef.assign(numFuncs + 1, false);
    std::vector<bool> hasUnkMod(numFuncs + 1, false);

    // Kosaraju SCC indices are caller-to-callee; iterate in reverse for
    // bottom-up (callee-before-caller) summary propagation.
    for (std::size_t idx = numComps; idx > 0; --idx) {
      const std::size_t c = idx - 1;
      const auto &members = compFuncs[c];
      if (members.empty())
        continue;

      bool sccRef = false;
      bool anyExit = false;
      for (FunctionID f : members) {
        if (f == hubIdx) {
          anyExit = true;
        } else {
          if (unknownDirectRef[f])
            sccRef = true;
          if (funcCFGs[f].exitLocal != InvalidLocal)
            anyExit = true;
        }
        for (FunctionID callee : cgSucc[f]) {
          if (comp[callee] != c && unknownTransRef[callee])
            sccRef = true;
        }
      }
      for (FunctionID f : members)
        unknownTransRef[f] = sccRef;

      if (!anyExit)
        continue;

      // Determine whether this SCC performs any exit-reaching unknown store,
      // and whether any store value is a concrete node other than
      // `p.unknownValue()`. When all exit-reaching unknown stores only write
      // `p.unknownValue()` (the 99% case for opaque external calls), share
      // `p.unknownValue()` directly across all such functions so callers see
      // a single store bit in `computeWeakLocalFlow` instead of K distinct
      // join IDs.
      bool sccMod = false;
      bool hasNonUnknownVal = false;
      for (FunctionID f : members) {
        if (unknownModSummary[f] && unknownModSummary[f] != p.unknownValue())
          hasNonUnknownVal = true;
        if (f == hubIdx) {
          for (FunctionID callee : cgSucc[hubIdx]) {
            if (comp[callee] != c && hasUnkMod[callee]) {
              sccMod = true;
              if (unknownModSummary[callee] != p.unknownValue())
                hasNonUnknownVal = true;
            }
          }
          continue;
        }
        const auto &fc = funcCFGs[f];
        auto it = unknownFnAccesses.find(f);
        if (it != unknownFnAccesses.end()) {
          for (AccessID id : it->second) {
            const auto &acc = p.accesses_[id];
            if (acc.kind == Program::AccessKind::Store &&
                fc.canReachExit[blockToLocal[acc.block]]) {
              sccMod = true;
              const NodeID val =
                  acc.sourceIsUnknown ? p.unknownValue() : acc.value;
              if (val != p.unknownValue())
                hasNonUnknownVal = true;
            }
          }
        }
        for (CallSiteID site : funcCallSites[f]) {
          const auto &cs = p.callSites_[site];
          if (!cs.mayReturn || !fc.canReachExit[blockToLocal[cs.block]])
            continue;
          for (FunctionID callee : liveCallTargets[site]) {
            if (comp[callee] != c && hasUnkMod[callee]) {
              sccMod = true;
              if (unknownModSummary[callee] != p.unknownValue())
                hasNonUnknownVal = true;
            }
          }
          if (siteCallsUnknownHub[site] && comp[hubIdx] != c &&
              hasUnkMod[hubIdx]) {
            sccMod = true;
            if (unknownModSummary[hubIdx] != p.unknownValue())
              hasNonUnknownVal = true;
          }
        }
      }

      if (!sccMod)
        continue;

      if (!hasNonUnknownVal) {
        for (FunctionID f : members) {
          if (f != hubIdx && funcCFGs[f].exitLocal == InvalidLocal)
            continue;
          hasUnkMod[f] = true;
          unknownModSummary[f] = p.unknownValue();
        }
        continue;
      }

      // Allocate persistent custom summary join nodes only when at least one
      // concrete non-`unknownValue()` store value exists in this SCC.
      NodeID sharedNewNode = 0;
      for (FunctionID f : members) {
        if (f != hubIdx && funcCFGs[f].exitLocal == InvalidLocal)
          continue;
        hasUnkMod[f] = true;
        if (!unknownModSummary[f] ||
            unknownModSummary[f] == p.unknownValue()) {
          if (!sharedNewNode)
            sharedNewNode = addJoin();
          unknownModSummary[f] = sharedNewNode;
        }
      }

      // Wire all exit-reaching unknown store values and callee summaries into
      // every distinct persistent summary node of this SCC (if an SCC merged
      // previously-allocated nodes, link them in a cycle so all share values).
      std::vector<NodeID> sccSumNodes;
      for (FunctionID f : members) {
        if (unknownModSummary[f] && unknownModSummary[f] != p.unknownValue())
          appendUnique(sccSumNodes, unknownModSummary[f]);
      }
      if (sccSumNodes.size() > 1) {
        for (std::size_t i = 0; i < sccSumNodes.size(); ++i)
          addIndirect(sccSumNodes[i], sccSumNodes[(i + 1) % sccSumNodes.size()]);
      }
      const NodeID targetSum = sccSumNodes.front();
      for (FunctionID f : members) {
        if (f == hubIdx) {
          for (FunctionID callee : cgSucc[hubIdx]) {
            if (comp[callee] != c && unknownModSummary[callee] &&
                unknownModSummary[callee] != targetSum) {
              addIndirect(unknownModSummary[callee], targetSum);
            }
          }
          continue;
        }
        const auto &fc = funcCFGs[f];
        auto it = unknownFnAccesses.find(f);
        if (it != unknownFnAccesses.end()) {
          for (AccessID id : it->second) {
            const auto &acc = p.accesses_[id];
            if (acc.kind == Program::AccessKind::Store &&
                fc.canReachExit[blockToLocal[acc.block]]) {
              const NodeID val =
                  acc.sourceIsUnknown ? p.unknownValue() : acc.value;
              addIndirect(val, targetSum);
            }
          }
        }
        for (CallSiteID site : funcCallSites[f]) {
          const auto &cs = p.callSites_[site];
          if (!cs.mayReturn || !fc.canReachExit[blockToLocal[cs.block]])
            continue;
          for (FunctionID callee : liveCallTargets[site]) {
            if (comp[callee] != c && unknownModSummary[callee] &&
                unknownModSummary[callee] != targetSum) {
              addIndirect(unknownModSummary[callee], targetSum);
            }
          }
          if (siteCallsUnknownHub[site] && comp[hubIdx] != c &&
              unknownModSummary[hubIdx] &&
              unknownModSummary[hubIdx] != targetSum) {
            addIndirect(unknownModSummary[hubIdx], targetSum);
          }
        }
      }
    }

    // Maintain persistent per-function local unknown store/load joins so
    // exact SCC-collapsed functions wire O(1) edges per function instead of
    // O(|unknownFnAccesses[f]|).
    for (const auto &[f, accs] : unknownFnAccesses) {
      if (!funcReachable[f])
        continue;
      const auto &fc = funcCFGs[f];
      for (AccessID id : accs) {
        const auto &acc = p.accesses_[id];
        if (!blockReachable[acc.block] || fnLocalUnkAccessWired[id])
          continue;
        fnLocalUnkAccessWired[id] = 1;
        if (acc.kind == Program::AccessKind::Store) {
          if (fc.exitLocal != InvalidLocal &&
              fc.canReachExit[blockToLocal[acc.block]]) {
            if (!fnLocalExitUnkStoreNode[f])
              fnLocalExitUnkStoreNode[f] = addJoin();
            addIndirect(acc.sourceIsUnknown ? p.unknownValue() : acc.value,
                        fnLocalExitUnkStoreNode[f]);
          } else {
            fnHasNonExitUnkStore[f] = 1;
          }
        } else {
          if (!fnLocalUnkLoadNode[f])
            fnLocalUnkLoadNode[f] = addJoin();
          addIndirect(fnLocalUnkLoadNode[f], acc.value);
        }
      }
    }

    // Build/extend the single shared interprocedural read channel for functions
    // that (transitively) perform `load unknown`. Non-unknown objects whose
    // own accesses do not touch `callee`'s subgraph wire `inNode ->
    // unknownReadEntry[callee]` in O(1) instead of pulling the entire call
    // graph into `activeFuncs`.
    for (FunctionID f = 0; f <= hubIdx; ++f) {
      const bool active = (f == hubIdx) ? unknownHubActive : funcReachable[f];
      if (active && unknownTransRef[f] && !unknownReadEntry[f])
        unknownReadEntry[f] = addJoin();
    }
    for (FunctionID f = 0; f <= hubIdx; ++f) {
      const bool active = (f == hubIdx) ? unknownHubActive : funcReachable[f];
      if (!active || !unknownTransRef[f])
        continue;
      const NodeID readEntry = unknownReadEntry[f];
      if (f < numFuncs && fnLocalUnkLoadNode[f])
        addIndirect(readEntry, fnLocalUnkLoadNode[f]);
      for (FunctionID callee : cgSucc[f]) {
        if (unknownTransRef[callee])
          addIndirect(readEntry, unknownReadEntry[callee]);
      }
    }

    // Wire non-empty read-only constant globals into newly active
    // `unknownReadEntry[entry]` channels once per entry function via a single
    // shared join node so `load unknown` soundly sees their initializers.
    if (!readOnlyWithInitSet.empty() && !unknownDirectRefFuncs.empty()) {
      for (FunctionID entry : p.entryFunctions_) {
        if (funcReachable[entry] && unknownTransRef[entry] &&
            !entryReadOnlyWired[entry]) {
          entryReadOnlyWired[entry] = 1;
          const NodeID readEntry = unknownReadEntry[entry];
          if (readOnlyWithInitSet.size() == 1) {
            const ObjectID onlyRO = *readOnlyWithInitSet.begin();
            addIndirect(p.objects_[onlyRO].initialStore, readEntry);
            emittedFlow[onlyRO] = true;
          } else {
            if (!allReadOnlyInitJoin) {
              allReadOnlyInitJoin = addJoin();
              for (ObjectID obj : readOnlyWithInitSet) {
                addIndirect(p.objects_[obj].initialStore, allReadOnlyInitJoin);
                emittedFlow[obj] = true;
              }
            }
            addIndirect(allReadOnlyInitJoin, readEntry);
          }
        }
      }
    }

    // If any function newly gained transitive `load unknown` or changed its
    // `unknownModSummary` NodeID (0 -> unknownValue, 0 -> customNode, or
    // unknownValue -> customNode), mark processed objects in its callers dirty.
    bool globalUnknownRefEvent =
        (!hadUnknownDirectRef && !unknownDirectRefFuncs.empty());
    bool anySummaryChanged = false;
    const std::uint64_t uDirtyGen = ++univDirtyGen;
    for (FunctionID f = 0; f <= hubIdx; ++f) {
      const bool refBecameTrue =
          f < prevTransRef.size() && !prevTransRef[f] && unknownTransRef[f];
      const bool modChanged =
          f < prevModSummary.size() && prevModSummary[f] != unknownModSummary[f];
      if (!refBecameTrue && !modChanged)
        continue;
      anySummaryChanged = true;
      if (f == hubIdx) {
        ++hubGlobalVersion;
        bumpFuncStateVer(hubIdx, true);
        forEachAccessedObjectInFunc(hubIdx, uDirtyGen, [&](ObjectID obj) {
          if (processed[obj] && !objLocalOnly[obj]) {
            callGraphDirty[obj] = true;
            markDirtySig(obj);
          }
        });
        for (CallSiteID site : unknownHubCallerSites) {
          const BlockID blk = p.callSites_[site].block;
          if (!blockReachable[blk])
            continue;
          const FunctionID caller = p.blocks_[blk].function;
          bumpFuncStateVer(caller, true);
          forEachAccessedObjectInFunc(caller, uDirtyGen, [&](ObjectID obj) {
            if (processed[obj] && !objLocalOnly[obj]) {
              callGraphDirty[obj] = true;
              markDirtySig(obj);
            }
          });
        }
        continue;
      }
      if (refBecameTrue && p.entryFunctions_.count(f)) {
        globalUnknownRefEvent = true;
        ++entryUnkRefVersion;
      }
      if (unknownHubActive && isAddressTakenFunc[f])
        bumpFuncStateVer(hubIdx, true);
      for (FunctionID caller : callers[f]) {
        bumpFuncStateVer(caller, true);
        forEachAccessedObjectInFunc(caller, uDirtyGen, [&](ObjectID obj) {
          if (processed[obj] && !objLocalOnly[obj]) {
            callGraphDirty[obj] = true;
            markDirtySig(obj);
          }
        });
      }
    }
    if (anySummaryChanged)
      ++unknownSummaryChangeVersion;
    if (globalUnknownRefEvent) {
      for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
        if (p.objects_[obj].memory && processed[obj] && !objLocalOnly[obj]) {
          callGraphDirty[obj] = true;
          markDirtySig(obj);
        }
      }
    }
  }

  // Exact Theorem X: compute universal-carry returning call-graph SCCs (`G_U`)
  // over `siteAlwaysCarries` returning exit-reaching callsites (valid for EVERY
  // weak object, global or non-global). Only SCCs with >= 2 concrete functions
  // satisfying the intra-SCC Cycle-Return Condition (`exit_u ->* entry_u`) are
  // collapsed to a single super-vertex.
  void ensureUnivCallGraphSCCs() {
    if (univSccCGVer == callGraphVersion &&
        univSccAlwaysVer == alwaysCarryVersion &&
        univSccUnkVer == unknownAccessVersion)
      return;

    const std::size_t numFuncs = p.functions_.size();
    const FunctionID hubIdx = static_cast<FunctionID>(numFuncs);

    auto refreshSCCUnknownSummaries = [&](UnivWeakSCC &scc) {
      bool changed = false;
      for (FunctionID f : scc.members) {
        if (fnHasNonExitUnkStore[f]) {
          if (std::find(scc.nonExitUnkFuncs.begin(), scc.nonExitUnkFuncs.end(),
                        f) == scc.nonExitUnkFuncs.end()) {
            scc.nonExitUnkFuncs.push_back(f);
            changed = true;
          }
        } else if (fnLocalExitUnkStoreNode[f]) {
          if (!scc.unkStoreJoin)
            scc.unkStoreJoin = addJoin();
          if (addIndirect(fnLocalExitUnkStoreNode[f], scc.unkStoreJoin))
            changed = true;
          scc.hasLocalExitUnkStore = true;
        }
        if (fnLocalUnkLoadNode[f]) {
          if (!scc.unkLoadJoin)
            scc.unkLoadJoin = addJoin();
          if (addIndirect(scc.unkLoadJoin, fnLocalUnkLoadNode[f]))
            changed = true;
        }
      }
      if (changed)
        ++scc.unkVersion;
    };

    // Fast path: only unknownAccessVersion changed -> update SCC unknown joins
    // incrementally without rebuilding SCCs or boundary lists.
    if (univSccCGVer == callGraphVersion &&
        univSccAlwaysVer == alwaysCarryVersion) {
      for (std::size_t u = 1; u < univSccs.size(); ++u)
        refreshSCCUnknownSummaries(univSccs[u]);
      univSccUnkVer = unknownAccessVersion;
      return;
    }

    // Full rebuild of G_U SCCs.
    std::vector<std::vector<FunctionID>> guSucc(numFuncs + 1);
    std::vector<std::vector<FunctionID>> guPred(numFuncs + 1);
    for (FunctionID u = 1; u < numFuncs; ++u) {
      if (!funcReachable[u])
        continue;
      const auto &fc = funcCFGs[u];
      if (fc.exitLocal == InvalidLocal)
        continue;
      for (CallSiteID site : funcCallSites[u]) {
        const auto &cs = p.callSites_[site];
        if (!cs.mayReturn || !siteAlwaysCarries(site) ||
            !fc.canReachExit[blockToLocal[cs.block]])
          continue;
        for (FunctionID v : liveCallTargets[site]) {
          if (funcReachable[v] && funcCFGs[v].exitLocal != InvalidLocal) {
            guSucc[u].push_back(v);
            guPred[v].push_back(u);
          }
        }
        if (siteCallsUnknownHub[site] && unknownHubActive) {
          guSucc[u].push_back(hubIdx);
          guPred[hubIdx].push_back(u);
        }
      }
    }
    if (unknownHubActive) {
      for (FunctionID v : addressTakenFuncs) {
        if (funcReachable[v] && funcCFGs[v].exitLocal != InvalidLocal) {
          guSucc[hubIdx].push_back(v);
          guPred[v].push_back(hubIdx);
        }
      }
    }

    auto guSccRes = computeSCCs(guSucc, guPred);
    const auto &comp = guSccRes.first;
    const std::size_t numComps = guSccRes.second;
    std::vector<std::vector<FunctionID>> compMembers(numComps);
    for (FunctionID f = 1; f <= hubIdx; ++f)
      compMembers[comp[f]].push_back(f);

    std::vector<UnivWeakSCC> oldUnivSccs = std::move(univSccs);
    std::vector<std::uint32_t> oldFnUnivScc = std::move(fnUnivScc);
    univSccs.clear();
    univSccs.emplace_back(); // index 0 unused
    fnUnivScc.assign(numFuncs + 1, 0);

    for (std::size_t c = 0; c < numComps; ++c) {
      const auto &rawMembers = compMembers[c];
      std::size_t concreteCount = 0;
      bool hubInC = false;
      for (FunctionID f : rawMembers) {
        if (f == hubIdx)
          hubInC = true;
        else
          ++concreteCount;
      }
      if (concreteCount < 2)
        continue;

      // Verify exact intra-SCC Cycle-Return Condition: some function w in c
      // has a returning siteAlwaysCarries callsite targeting c that can reach
      // a siteAlwaysCarries callsite targeting c in w's CFG.
      bool cycleReturn = false;
      for (FunctionID w : rawMembers) {
        if (w == hubIdx)
          continue;
        struct Ep {
          LocalBlock blk;
          std::uint32_t pos;
        };
        std::vector<Ep> outEps;
        std::vector<Ep> inEps;
        for (CallSiteID site : funcCallSites[w]) {
          if (!siteAlwaysCarries(site))
            continue;
          const auto &cs = p.callSites_[site];
          bool targetsC = (hubInC && siteCallsUnknownHub[site]);
          if (!targetsC) {
            for (FunctionID v : liveCallTargets[site]) {
              if (comp[v] == c) {
                targetsC = true;
                break;
              }
            }
          }
          if (!targetsC)
            continue;
          const Ep ep{blockToLocal[cs.block], callEventPos[site]};
          inEps.push_back(ep);
          if (cs.mayReturn)
            outEps.push_back(ep);
        }
        if (outEps.empty() || inEps.empty())
          continue;
        // Fast intra-block check first.
        for (const auto &oe : outEps) {
          for (const auto &ie : inEps) {
            if (oe.blk == ie.blk && oe.pos < ie.pos) {
              cycleReturn = true;
              break;
            }
          }
          if (cycleReturn)
            break;
        }
        if (cycleReturn)
          break;
        ensureFunctionSCCs(w);
        auto &fc = funcCFGs[w];
        const std::uint64_t ug = ++fc.weakGen;
        std::vector<std::size_t> bWork;
        std::vector<std::uint8_t> compHasIn(fc.compBlocks.size(), 0);
        for (const auto &ie : inEps) {
          const std::size_t bc = fc.comp[ie.blk];
          if (!compHasIn[bc]) {
            compHasIn[bc] = 1;
            for (auto pred : fc.compPreds[bc]) {
              if (fc.weakNeededGen[pred] != ug) {
                fc.weakNeededGen[pred] = ug;
                bWork.push_back(pred);
              }
            }
          }
        }
        while (!bWork.empty()) {
          const std::size_t bc = bWork.back();
          bWork.pop_back();
          for (auto pred : fc.compPreds[bc]) {
            if (fc.weakNeededGen[pred] != ug) {
              fc.weakNeededGen[pred] = ug;
              bWork.push_back(pred);
            }
          }
        }
        for (const auto &oe : outEps) {
          const std::size_t bc = fc.comp[oe.blk];
          if (fc.weakNeededGen[bc] == ug ||
              (compHasIn[bc] && fc.compCyclic[bc])) {
            cycleReturn = true;
            break;
          }
        }
        if (cycleReturn)
          break;
      }
      if (!cycleReturn)
        continue;

      const std::uint32_t uId = static_cast<std::uint32_t>(univSccs.size());
      univSccs.emplace_back();
      auto &scc = univSccs.back();
      scc.containsHub = hubInC;
      scc.members.reserve(concreteCount);
      for (FunctionID f : rawMembers) {
        fnUnivScc[f] = uId;
        if (f < numFuncs) {
          scc.members.push_back(f);
          if (p.entryFunctions_.count(f))
            scc.containsEntry = true;
        }
      }
      std::sort(scc.members.begin(), scc.members.end());
      scc.repFunc = scc.members[0];

      // Deduplicate external callees & callers for this SCC once.
      std::vector<std::uint8_t> aRetMark(numFuncs + 1, 0);
      std::vector<std::uint8_t> aNoRetMark(numFuncs + 1, 0);
      std::vector<std::uint8_t> gRetMark(numFuncs + 1, 0);
      std::vector<std::uint8_t> gNoRetMark(numFuncs + 1, 0);
      for (FunctionID f : scc.members) {
        const auto &fc = funcCFGs[f];
        bool fHasNonExitExtRet = false;
        for (CallSiteID site : funcCallSites[f]) {
          const auto &cs = p.callSites_[site];
          const bool always = siteAlwaysCarries(site);
          const bool reachesExit = fc.canReachExit[blockToLocal[cs.block]];
          const bool isRetExit = cs.mayReturn && reachesExit;
          const bool isNonExitRet = cs.mayReturn && !reachesExit;
          auto observeExt = [&](FunctionID v) {
            if (comp[v] == c)
              return;
            if (isNonExitRet)
              fHasNonExitExtRet = true;
            if (isRetExit) {
              if (!gRetMark[v]) {
                gRetMark[v] = 1;
                scc.globalRetExtCallees.push_back(v);
              }
              if (always && !aRetMark[v]) {
                aRetMark[v] = 1;
                scc.alwaysRetExtCallees.push_back(v);
              }
            } else {
              if (!gNoRetMark[v]) {
                gNoRetMark[v] = 1;
                scc.globalNoRetExtCallees.push_back(v);
              }
              if (always && !aNoRetMark[v]) {
                aNoRetMark[v] = 1;
                scc.alwaysNoRetExtCallees.push_back(v);
              }
            }
          };
          for (FunctionID v : liveCallTargets[site])
            observeExt(v);
          if (siteCallsUnknownHub[site] && unknownHubActive)
            observeExt(hubIdx);
        }
        if (fHasNonExitExtRet)
          scc.nonExitExtRetFuncs.push_back(f);
      }
      if (hubInC) {
        for (FunctionID v : addressTakenFuncs) {
          if (!funcReachable[v] || comp[v] == c)
            continue;
          if (funcCFGs[v].exitLocal != InvalidLocal) {
            if (!gRetMark[v]) {
              gRetMark[v] = 1;
              scc.globalRetExtCallees.push_back(v);
            }
            if (!aRetMark[v]) {
              aRetMark[v] = 1;
              scc.alwaysRetExtCallees.push_back(v);
            }
          } else {
            if (!gNoRetMark[v]) {
              gNoRetMark[v] = 1;
              scc.globalNoRetExtCallees.push_back(v);
            }
            if (!aNoRetMark[v]) {
              aNoRetMark[v] = 1;
              scc.alwaysNoRetExtCallees.push_back(v);
            }
          }
        }
      }
      // Remove duplicates from NoRet that are already covered by Ret, and sort
      // deterministically for structural comparison across rebuilds.
      scc.alwaysNoRetExtCallees.erase(
          std::remove_if(scc.alwaysNoRetExtCallees.begin(),
                         scc.alwaysNoRetExtCallees.end(),
                         [&](FunctionID v) { return aRetMark[v] != 0; }),
          scc.alwaysNoRetExtCallees.end());
      scc.globalNoRetExtCallees.erase(
          std::remove_if(scc.globalNoRetExtCallees.begin(),
                         scc.globalNoRetExtCallees.end(),
                         [&](FunctionID v) { return gRetMark[v] != 0; }),
          scc.globalNoRetExtCallees.end());
      std::sort(scc.alwaysRetExtCallees.begin(), scc.alwaysRetExtCallees.end());
      std::sort(scc.alwaysNoRetExtCallees.begin(),
                scc.alwaysNoRetExtCallees.end());
      std::sort(scc.globalRetExtCallees.begin(), scc.globalRetExtCallees.end());
      std::sort(scc.globalNoRetExtCallees.begin(),
                scc.globalNoRetExtCallees.end());
      std::sort(scc.nonExitExtRetFuncs.begin(), scc.nonExitExtRetFuncs.end());

      // Collect deduplicated external caller callsites (caller outside c).
      NodeSet seenCallerSites;
      for (FunctionID f : scc.members) {
        for (CallSiteID site : callerSites[f]) {
          const BlockID blk = p.callSites_[site].block;
          if (!blockReachable[blk])
            continue;
          const FunctionID caller = p.blocks_[blk].function;
          if (comp[caller] != c && seenCallerSites.insertBit(site))
            scc.extCallerSites.push_back(site);
        }
      }
      if (hubInC) {
        for (CallSiteID site : unknownHubCallerSites) {
          const BlockID blk = p.callSites_[site].block;
          if (!blockReachable[blk])
            continue;
          const FunctionID caller = p.blocks_[blk].function;
          if (comp[caller] != c && seenCallerSites.insertBit(site))
            scc.extCallerSites.push_back(site);
        }
      }
    }

    // Build canonical external target sets (requires final `fnUnivScc` for all
    // SCCs) and preserve per-SCC structural versions + persistent summary joins
    // whenever an SCC is structurally identical across rebuilds.
    bool anyStructChanged = (univSccs.size() != oldUnivSccs.size());
    auto canonNew = [&](FunctionID v) -> FunctionID {
      if (const std::uint32_t vu = fnUnivScc[v])
        return univSccs[vu].repFunc;
      return v;
    };
    for (std::size_t u = 1; u < univSccs.size(); ++u) {
      auto &scc = univSccs[u];
      for (FunctionID v : scc.alwaysRetExtCallees) {
        scc.alwaysRetExtRawSet.insertBit(v);
        const FunctionID cv = canonNew(v);
        if (cv == hubIdx)
          scc.alwaysRetCallsHub = true;
        else
          scc.alwaysRetExtCanonSet.insertBit(cv);
      }
      for (FunctionID v : scc.alwaysNoRetExtCallees) {
        scc.alwaysNoRetExtRawSet.insertBit(v);
        const FunctionID cv = canonNew(v);
        if (cv == hubIdx) {
          if (!scc.alwaysRetCallsHub)
            scc.alwaysNoRetCallsHub = true;
        } else if (!scc.alwaysRetExtCanonSet.count(cv)) {
          scc.alwaysNoRetExtCanonSet.insertBit(cv);
        }
      }
      for (FunctionID v : scc.globalRetExtCallees) {
        const FunctionID cv = canonNew(v);
        if (cv == hubIdx)
          scc.globalRetCallsHub = true;
        else
          scc.globalRetExtCanonSet.insertBit(cv);
      }
      for (FunctionID v : scc.globalNoRetExtCallees) {
        const FunctionID cv = canonNew(v);
        if (cv == hubIdx) {
          if (!scc.globalRetCallsHub)
            scc.globalNoRetCallsHub = true;
        } else if (!scc.globalRetExtCanonSet.count(cv)) {
          scc.globalNoRetExtCanonSet.insertBit(cv);
        }
      }

      const std::uint32_t oldU =
          (scc.repFunc < oldFnUnivScc.size()) ? oldFnUnivScc[scc.repFunc] : 0U;
      bool sameAsOld = false;
      if (oldU != 0 && oldU < oldUnivSccs.size()) {
        const auto &old = oldUnivSccs[oldU];
        sameAsOld =
            old.containsHub == scc.containsHub &&
            old.containsEntry == scc.containsEntry &&
            old.members == scc.members &&
            old.nonExitExtRetFuncs == scc.nonExitExtRetFuncs &&
            old.alwaysRetExtCallees == scc.alwaysRetExtCallees &&
            old.alwaysNoRetExtCallees == scc.alwaysNoRetExtCallees &&
            old.globalRetExtCallees == scc.globalRetExtCallees &&
            old.globalNoRetExtCallees == scc.globalNoRetExtCallees &&
            old.alwaysRetExtCanonSet == scc.alwaysRetExtCanonSet &&
            old.alwaysNoRetExtCanonSet == scc.alwaysNoRetExtCanonSet &&
            old.globalRetExtCanonSet == scc.globalRetExtCanonSet &&
            old.globalNoRetExtCanonSet == scc.globalNoRetExtCanonSet &&
            old.alwaysRetCallsHub == scc.alwaysRetCallsHub &&
            old.alwaysNoRetCallsHub == scc.alwaysNoRetCallsHub &&
            old.globalRetCallsHub == scc.globalRetCallsHub &&
            old.globalNoRetCallsHub == scc.globalNoRetCallsHub;
      }
      if (sameAsOld) {
        auto &old = oldUnivSccs[oldU];
        scc.structVersion = old.structVersion;
        scc.stateVer = old.stateVer;
        scc.nonExitUnkFuncs = std::move(old.nonExitUnkFuncs);
        scc.unkStoreJoin = old.unkStoreJoin;
        scc.unkLoadJoin = old.unkLoadJoin;
        scc.hasLocalExitUnkStore = old.hasLocalExitUnkStore;
        scc.unkVersion = old.unkVersion;
        scc.alwaysExtSummary = std::move(old.alwaysExtSummary);
        scc.globalExtSummary = std::move(old.globalExtSummary);
        scc.weakAncCoreWatchIds = std::move(old.weakAncCoreWatchIds);
      } else {
        scc.structVersion = ++nextUnivStructVersion;
        scc.stateVer = ++nextUnivStateVer;
        anyStructChanged = true;
      }
      refreshSCCUnknownSummaries(scc);
    }

    // Migrate `accessedByObjects` and `weakAncCtxIds` from old SCCs
    // (monotonic: every old SCC is a subset of a single new SCC).
    for (std::size_t oldU = 1; oldU < oldUnivSccs.size(); ++oldU) {
      const std::uint32_t newU = fnUnivScc[oldUnivSccs[oldU].repFunc];
      if (!oldUnivSccs[oldU].weakAncCoreWatchIds.empty())
        markCoreWatchesStale(oldUnivSccs[oldU].weakAncCoreWatchIds);
      if (!oldUnivSccs[oldU].accessedByObjects.empty()) {
        if (newU != 0) {
          univSccs[newU].accessedByObjects.unite(
              oldUnivSccs[oldU].accessedByObjects);
        } else {
          for (FunctionID f : oldUnivSccs[oldU].members)
            funcAccessedByObjects[f].unite(oldUnivSccs[oldU].accessedByObjects);
        }
      }
      if (!oldUnivSccs[oldU].weakAncCtxIds.empty()) {
        if (newU != 0) {
          for (std::uint32_t cid : oldUnivSccs[oldU].weakAncCtxIds)
            appendUnique(univSccs[newU].weakAncCtxIds, cid);
        } else {
          for (FunctionID f : oldUnivSccs[oldU].members)
            for (std::uint32_t cid : oldUnivSccs[oldU].weakAncCtxIds)
              appendUnique(funcWeakAncCtxs[f], cid);
        }
      }
    }

    univDirtyStamp.assign(univSccs.size(), 0);
    if (anyStructChanged)
      ++univSccBuildId;
    univSccCGVer = callGraphVersion;
    univSccAlwaysVer = alwaysCarryVersion;
    univSccUnkVer = unknownAccessVersion;
  }

  // Lazily build persistent O((k + 1) log M) external unknown-mod groups and
  // binary segment tree for external `unknownReadEntry` targets of `univSccs[u]`.
  // Must be called with `localCap.active == false` so persistent segment-tree
  // join nodes live in the main VFG and are shared across all objects.
  void ensureUnivExtSummary(std::uint32_t u, bool forGlobal) {
    auto &scc = univSccs[u];
    auto &sum = forGlobal ? scc.globalExtSummary : scc.alwaysExtSummary;
    if (sum.built && sum.builtSumVer == unknownSummaryChangeVersion)
      return;
    sum.invalidate();
    sum.built = true;
    sum.builtSumVer = unknownSummaryChangeVersion;

    const auto &retList =
        forGlobal ? scc.globalRetExtCallees : scc.alwaysRetExtCallees;
    const auto &noRetList =
        forGlobal ? scc.globalNoRetExtCallees : scc.alwaysNoRetExtCallees;
    auto canonV = [&](FunctionID v) -> FunctionID {
      if (const std::uint32_t vu = fnUnivScc[v])
        return univSccs[vu].repFunc;
      return v;
    };

    // Group returning external callees with `unknownModSummary[v] != 0` by
    // distinct summary `NodeID` (typically 1 group: `p.unknownValue()`).
    std::unordered_map<NodeID, std::size_t> modGroupIdx;
    for (FunctionID v : retList) {
      const NodeID mNode = unknownModSummary[v];
      if (!mNode)
        continue;
      auto [it, ins] = modGroupIdx.emplace(mNode, sum.modGroups.size());
      if (ins)
        sum.modGroups.push_back({mNode, {}});
      sum.modGroups[it->second].canonTargets.push_back(canonV(v));
    }
    for (auto &grp : sum.modGroups) {
      std::sort(grp.canonTargets.begin(), grp.canonTargets.end());
      grp.canonTargets.erase(
          std::unique(grp.canonTargets.begin(), grp.canonTargets.end()),
          grp.canonTargets.end());
    }

    // Collect deduplicated `(canonTarget, unknownReadEntry[v])` sorted by
    // `canonTarget` so all members of another UnivWeakSCC form ONE contiguous
    // range `[L, R)` in the binary segment tree.
    auto &seg = sum.readSeg;
    auto pushRead = [&](FunctionID v) {
      if (!unknownTransRef[v])
        return;
      const NodeID rNode = unknownReadEntry[v];
      if (rNode)
        seg.entries.push_back({canonV(v), rNode});
    };
    for (FunctionID v : retList)
      pushRead(v);
    for (FunctionID v : noRetList)
      pushRead(v);
    if (seg.entries.empty())
      return;
    std::sort(seg.entries.begin(), seg.entries.end(),
              [](const UnivExtSegTree::Entry &a,
                 const UnivExtSegTree::Entry &b) {
                return std::tie(a.canonTarget, a.readNode) <
                       std::tie(b.canonTarget, b.readNode);
              });
    seg.entries.erase(
        std::unique(seg.entries.begin(), seg.entries.end(),
                    [](const UnivExtSegTree::Entry &a,
                       const UnivExtSegTree::Entry &b) {
                      return a.canonTarget == b.canonTarget &&
                             a.readNode == b.readNode;
                    }),
        seg.entries.end());
    const std::uint32_t M = static_cast<std::uint32_t>(seg.entries.size());
    for (std::uint32_t i = 0; i < M;) {
      const FunctionID ct = seg.entries[i].canonTarget;
      std::uint32_t j = i + 1;
      while (j < M && seg.entries[j].canonTarget == ct)
        ++j;
      seg.canonRanges.emplace_back(ct, std::make_pair(i, j));
      i = j;
    }
    std::size_t base = 1;
    while (base < M)
      base <<= 1;
    seg.leafBase = base;
    seg.tree.assign(2 * base, 0);
    for (std::uint32_t i = 0; i < M; ++i)
      seg.tree[base + i] = seg.entries[i].readNode;
    for (std::size_t idx = base - 1; idx >= 1; --idx) {
      const NodeID L = seg.tree[2 * idx];
      const NodeID R = seg.tree[2 * idx + 1];
      if (L && R) {
        if (L == R) {
          seg.tree[idx] = L;
        } else {
          const NodeID jn = addJoin();
          addIndirect(jn, L);
          addIndirect(jn, R);
          seg.tree[idx] = jn;
        }
      } else {
        seg.tree[idx] = L ? L : R;
      }
    }
  }

  // Lazily build persistent O((k + 1) log M) modGroups + readSeg over all
  // reachable `addressTakenFuncs` so `separateHubActive` never loops over
  // thousands of inactive address-taken functions per object.
  void ensureHubExtSummary(bool useUniv) {
    const std::uint64_t curBuild = useUniv ? univSccBuildId : 0ULL;
    if (hubExtSummary.built &&
        hubExtSummary.builtSumVer == unknownSummaryChangeVersion &&
        hubExtBuiltGlobalVer == hubGlobalVersion &&
        hubExtBuiltUnivBuild == curBuild)
      return;
    hubExtSummary.invalidate();
    hubExtSummary.built = true;
    hubExtSummary.builtSumVer = unknownSummaryChangeVersion;
    hubExtBuiltGlobalVer = hubGlobalVersion;
    hubExtBuiltUnivBuild = curBuild;

    auto canonV = [&](FunctionID v) -> FunctionID {
      if (useUniv) {
        if (const std::uint32_t vu = fnUnivScc[v])
          return univSccs[vu].repFunc;
      }
      return v;
    };

    std::unordered_map<NodeID, std::size_t> modGroupIdx;
    auto &seg = hubExtSummary.readSeg;
    for (FunctionID v : addressTakenFuncs) {
      if (!funcReachable[v])
        continue;
      const FunctionID cv = canonV(v);
      if (const NodeID mNode = unknownModSummary[v]) {
        auto [it, ins] =
            modGroupIdx.emplace(mNode, hubExtSummary.modGroups.size());
        if (ins)
          hubExtSummary.modGroups.push_back({mNode, {}});
        hubExtSummary.modGroups[it->second].canonTargets.push_back(cv);
      }
      if (unknownTransRef[v] && unknownReadEntry[v])
        seg.entries.push_back({cv, unknownReadEntry[v]});
    }
    for (auto &grp : hubExtSummary.modGroups) {
      std::sort(grp.canonTargets.begin(), grp.canonTargets.end());
      grp.canonTargets.erase(
          std::unique(grp.canonTargets.begin(), grp.canonTargets.end()),
          grp.canonTargets.end());
    }
    if (seg.entries.empty())
      return;
    std::sort(seg.entries.begin(), seg.entries.end(),
              [](const UnivExtSegTree::Entry &a,
                 const UnivExtSegTree::Entry &b) {
                return std::tie(a.canonTarget, a.readNode) <
                       std::tie(b.canonTarget, b.readNode);
              });
    seg.entries.erase(
        std::unique(seg.entries.begin(), seg.entries.end(),
                    [](const UnivExtSegTree::Entry &a,
                       const UnivExtSegTree::Entry &b) {
                      return a.canonTarget == b.canonTarget &&
                             a.readNode == b.readNode;
                    }),
        seg.entries.end());
    const std::uint32_t M = static_cast<std::uint32_t>(seg.entries.size());
    for (std::uint32_t i = 0; i < M;) {
      const FunctionID ct = seg.entries[i].canonTarget;
      std::uint32_t j = i + 1;
      while (j < M && seg.entries[j].canonTarget == ct)
        ++j;
      seg.canonRanges.emplace_back(ct, std::make_pair(i, j));
      i = j;
    }
    std::size_t base = 1;
    while (base < M)
      base <<= 1;
    seg.leafBase = base;
    seg.tree.assign(2 * base, 0);
    for (std::uint32_t i = 0; i < M; ++i)
      seg.tree[base + i] = seg.entries[i].readNode;
    for (std::size_t idx = base - 1; idx >= 1; --idx) {
      const NodeID L = seg.tree[2 * idx];
      const NodeID R = seg.tree[2 * idx + 1];
      if (L && R) {
        if (L == R) {
          seg.tree[idx] = L;
        } else {
          const NodeID jn = addJoin();
          addIndirect(jn, L);
          addIndirect(jn, R);
          seg.tree[idx] = jn;
        }
      } else {
        seg.tree[idx] = L ? L : R;
      }
    }
  }

  enum class EscapeBlockKind {
    Unblocked,
    PendingObject,
    UnknownEscape,
    EmptyPointerOrCall,
  };

  // Transitive escape-order check matching Theorem 1 (Section 4.4):
  // alloc_o must wait until every object `dep` with `dep ✂+ alloc_o` has been
  // processed, plus any unresolved indirect call to which alloc_o is passed.
  EscapeBlockKind inspectEscapeBlock(ObjectID object) {
    if (blockedWitness[object] && pending.count(blockedWitness[object])) {
      return blockedWitness[object] == object ? EscapeBlockKind::UnknownEscape
                                              : EscapeBlockKind::PendingObject;
    }
    if (objEscapedToUnknown[object]) {
      if (pending.count(p.unknownObject())) {
        blockedWitness[object] = p.unknownObject();
        return EscapeBlockKind::PendingObject;
      }
      blockedWitness[object] = object;
      return EscapeBlockKind::UnknownEscape;
    }
    if (blockedEmptyEpoch[object] == reachEpoch)
      return EscapeBlockKind::EmptyPointerOrCall;
    auto setWitness = [&](ObjectID dep) { blockedWitness[object] = dep; };

    const std::uint64_t gen = ++escBlockGen;
    bool sawEmpty = false;
    std::vector<ObjectID> work{object};
    escObjVisGen[object] = gen;
    while (!work.empty()) {
      const ObjectID cur = work.back();
      work.pop_back();
      if (objEscapedToUnknown[cur]) {
        if (pending.count(p.unknownObject())) {
          setWitness(p.unknownObject());
          return EscapeBlockKind::PendingObject;
        }
        setWitness(object);
        return EscapeBlockKind::UnknownEscape;
      }
      for (ObjectID dep : initialEscapeObjects[cur]) {
        if (!p.objects_[dep].memory)
          continue;
        if (dep == p.unknownObject() || objEscapedToUnknown[dep]) {
          if (pending.count(p.unknownObject())) {
            setWitness(p.unknownObject());
            return EscapeBlockKind::PendingObject;
          }
          setWitness(object);
          return EscapeBlockKind::UnknownEscape;
        }
        if (pending.count(dep)) {
          setWitness(dep);
          return EscapeBlockKind::PendingObject;
        }
        if (escObjVisGen[dep] != gen) {
          escObjVisGen[dep] = gen;
          work.push_back(dep);
        }
      }
      for (NodeID ptr : getEscapePointers(cur)) {
        if (escPtrVisGen[ptr] == gen)
          continue;
        escPtrVisGen[ptr] = gen;
        if (ptrHasUnknownTarget[ptr]) {
          if (pending.count(p.unknownObject())) {
            setWitness(p.unknownObject());
            return EscapeBlockKind::PendingObject;
          }
          setWitness(object);
          return EscapeBlockKind::UnknownEscape;
        }
        const auto &targets = ptsOf(ptr);
        if (targets.empty()) {
          sawEmpty = true;
          continue;
        }
        if (targets.count(p.unknownObject())) {
          if (pending.count(p.unknownObject())) {
            setWitness(p.unknownObject());
            return EscapeBlockKind::PendingObject;
          }
          setWitness(object);
          return EscapeBlockKind::UnknownEscape;
        }
        for (ObjectID target : targets) {
          if (!p.objects_[target].memory)
            continue;
          if (pending.count(target)) {
            setWitness(target);
            return EscapeBlockKind::PendingObject;
          }
          if (escObjVisGen[target] != gen) {
            escObjVisGen[target] = gen;
            work.push_back(target);
          }
        }
      }
      for (CallSiteID site : escapedIndirectCalls[cur]) {
        if (liveCallTargets[site].empty() && !liveCallExternal[site])
          sawEmpty = true;
      }
    }
    blockedWitness[object] = 0;
    if (sawEmpty) {
      blockedEmptyEpoch[object] = reachEpoch;
      return EscapeBlockKind::EmptyPointerOrCall;
    }
    blockedEmptyEpoch[object] = 0;
    return EscapeBlockKind::Unblocked;
  }

  bool escapeBlocked(ObjectID object) {
    return inspectEscapeBlock(object) != EscapeBlockKind::Unblocked;
  }

  bool feedsUnresolvedCall(ObjectID object) const {
    for (AccessID id : activeAccesses[object]) {
      const auto &access = p.accesses_[id];
      if (access.kind == Program::AccessKind::Load &&
          isIndirectCalleeLoad[access.value])
        return true;
    }
    for (AccessID id : pendingAccesses[object]) {
      const auto &access = p.accesses_[id];
      if (access.kind == Program::AccessKind::Load &&
          isIndirectCalleeLoad[access.value])
        return true;
    }
    return false;
  }

  bool sharesFunctionWithUnresolvedCall(ObjectID object) const {
    for (AccessID id : activeAccesses[object]) {
      const FunctionID f = p.blocks_[p.accesses_[id].block].function;
      if (funcHasUnresolvedIndirectCall[f])
        return true;
    }
    for (AccessID id : pendingAccesses[object]) {
      const FunctionID f = p.blocks_[p.accesses_[id].block].function;
      if (funcHasUnresolvedIndirectCall[f])
        return true;
    }
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Algorithm 2: Per-function ComputeLocalFlow + PreprocessGlobalFlow
  //===--------------------------------------------------------------------===//

  struct LocalEvent {
    LocalBlock block = 0;
    std::uint32_t pos = 0;
    std::uint8_t phase = 0; // 0 = access / callIn, 1 = callOut, 2 = exitLoad
    bool isStore = false;
    bool isStrong = false;
    NodeID node = 0; // store source value or load destination value
    std::uint32_t sparseNode = 0;
  };

  void computeWeakLocalFlow(FunctionID fn, NodeID entryJoinNode,
                            std::vector<LocalEvent> &events) {
    // Exact Weak Entry-Loop Absorption (Theorem G): if any load event feeds
    // `entryJoinNode` itself (e.g., a callIn to a callee in the same entry SCC
    // or an exit load when entry and exit are merged into one SCC), every weak
    // store that can reach such a load unconditionally flows into
    // `entryJoinNode`. Since `entryJoinNode` already reaches every load in
    // `fn`, wiring `store -> entryJoinNode` directly and dropping the store
    // from local masks preserves exact transitive value flow with 0 join nodes.
    bool hasEntrySink = false;
    for (const auto &ev : events) {
      if (!ev.isStore && ev.node == entryJoinNode) {
        hasEntrySink = true;
        break;
      }
    }
    std::vector<bool> absorbedStore;
    if (hasEntrySink) {
      ensureFunctionSCCs(fn);
      auto &fc = funcCFGs[fn];
      const std::uint64_t ag = ++fc.weakGen;
      std::unordered_map<LocalBlock, std::size_t> lastEntrySinkPos;
      std::vector<std::size_t> strictAncWork;
      std::vector<bool> compHasEntrySink(fc.compBlocks.size(), false);
      for (std::size_t i = 0; i < events.size(); ++i) {
        if (!events[i].isStore && events[i].node == entryJoinNode) {
          const LocalBlock b = events[i].block;
          lastEntrySinkPos[b] = i;
          const std::size_t c = fc.comp[b];
          if (!compHasEntrySink[c]) {
            compHasEntrySink[c] = true;
            for (auto pred : fc.compPreds[c]) {
              if (fc.weakNeededGen[pred] != ag) {
                fc.weakNeededGen[pred] = ag;
                strictAncWork.push_back(pred);
              }
            }
          }
        }
      }
      while (!strictAncWork.empty()) {
        const std::size_t c = strictAncWork.back();
        strictAncWork.pop_back();
        for (auto pred : fc.compPreds[c]) {
          if (fc.weakNeededGen[pred] != ag) {
            fc.weakNeededGen[pred] = ag;
            strictAncWork.push_back(pred);
          }
        }
      }
      absorbedStore.assign(events.size(), false);
      for (std::size_t i = 0; i < events.size(); ++i) {
        if (!events[i].isStore)
          continue;
        const LocalBlock b = events[i].block;
        const std::size_t c = fc.comp[b];
        bool reachesEntry = (fc.weakNeededGen[c] == ag) ||
                            (compHasEntrySink[c] && fc.compCyclic[c]);
        if (!reachesEntry) {
          auto it = lastEntrySinkPos.find(b);
          if (it != lastEntrySinkPos.end() && i < it->second)
            reachesEntry = true;
        }
        if (reachesEntry) {
          absorbedStore[i] = true;
          if (events[i].node != entryJoinNode)
            addIndirect(events[i].node, entryJoinNode);
        }
      }
    }

    // Assign dense local indices to distinct non-trivial store value nodes.
    // Because every store is weak (none kills entryJoinNode), the exact set of
    // values reaching any program point is uniquely identified by its subset of
    // local store indices. We propagate pure bitmasks along the block-SCC DAG
    // and lazily materialize join nodes ONLY at masks actually read by a load
    // (0 join nodes for non-load branches and intermediate merges).
    std::vector<NodeID> storeVals;
    std::unordered_map<NodeID, std::uint32_t> storeIdxOf;
    bool hasNonEntryLoad = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
      const auto &ev = events[i];
      if (!ev.isStore) {
        if (ev.node != entryJoinNode)
          hasNonEntryLoad = true;
        continue;
      }
      if ((!absorbedStore.empty() && absorbedStore[i]) ||
          ev.node == entryJoinNode)
        continue;
      auto [it, inserted] = storeIdxOf.emplace(
          ev.node, static_cast<std::uint32_t>(storeVals.size()));
      if (inserted)
        storeVals.push_back(ev.node);
    }

    if (!hasNonEntryLoad)
      return;

    // Fast path: load-only function (0 remaining local stores) needs 0 join
    // nodes and no SCC walk — every load directly receives entryJoinNode.
    if (storeVals.empty()) {
      for (const auto &ev : events) {
        if (!ev.isStore && ev.node != entryJoinNode)
          addIndirect(entryJoinNode, ev.node);
      }
      return;
    }

    // For >8 distinct remaining store values, delegate to exact pruned sparse
    // SSA / IDF (`O(|Events| + |IDF|)` edges) instead of bitmask propagation
    // (`O(|Loads| * |Stores|)` worst-case fan-in).
    if (storeVals.size() > 8) {
      std::size_t out = 0;
      for (std::size_t i = 0; i < events.size(); ++i) {
        const auto &ev = events[i];
        if (ev.isStore) {
          if ((!absorbedStore.empty() && absorbedStore[i]) ||
              ev.node == entryJoinNode)
            continue;
        } else {
          if (ev.node == entryJoinNode)
            continue;
        }
        if (out != i)
          events[out] = ev;
        ++out;
      }
      events.resize(out);
      computeStrongLocalFlow(fn, entryJoinNode, events);
      return;
    }

    ensureFunctionSCCs(fn);
    auto &fc = funcCFGs[fn];
    const std::uint64_t g = ++fc.weakGen;

    // Group events by local block (already sorted by (block, pos, phase)).
    std::unordered_map<LocalBlock, std::pair<std::size_t, std::size_t>>
        blockRanges;
    std::vector<std::size_t> work;
    std::vector<std::size_t> activeComps;
    for (std::size_t i = 0; i < events.size();) {
      const LocalBlock b = events[i].block;
      std::size_t j = i + 1;
      while (j < events.size() && events[j].block == b)
        ++j;
      blockRanges.emplace(b, std::make_pair(i, j));
      bool hasLoadInBlock = false;
      for (std::size_t k = i; k < j; ++k) {
        if (!events[k].isStore && events[k].node != entryJoinNode) {
          hasLoadInBlock = true;
          break;
        }
      }
      if (hasLoadInBlock) {
        const std::size_t c = fc.comp[b];
        if (fc.weakNeededGen[c] != g) {
          fc.weakNeededGen[c] = g;
          work.push_back(c);
          activeComps.push_back(c);
        }
      }
      i = j;
    }

    // Backward BFS along block-SCC DAG to mark only ancestors of loads.
    while (!work.empty()) {
      const std::size_t c = work.back();
      work.pop_back();
      for (auto pred : fc.compPreds[c]) {
        if (fc.weakNeededGen[pred] != g) {
          fc.weakNeededGen[pred] = g;
          work.push_back(pred);
          activeComps.push_back(pred);
        }
      }
    }

    if (activeComps.empty())
      return;

    // Kosaraju component indices are in source-to-sink topological order.
    std::sort(activeComps.begin(), activeComps.end());
    for (std::uint32_t ai = 0; ai < activeComps.size(); ++ai)
      fc.weakActiveIdx[activeComps[ai]] = ai;

    const std::size_t rootComp = fc.comp[0];

    std::vector<std::uint64_t> outMask(activeComps.size(), 0);
    std::unordered_map<std::uint64_t, NodeID> maskToNode;
    maskToNode.emplace(0ULL, entryJoinNode);

    auto emitLoadWithMask = [&](std::uint64_t mask, NodeID loadDst,
                                NodeID &hintNode, std::uint64_t &hintMask) {
      if (mask == 0) {
        if (entryJoinNode != loadDst)
          addIndirect(entryJoinNode, loadDst);
        return;
      }
      // Single-bit mask whose store value is loadDst itself (e.g., sccExit
      // self-flow): only entryJoinNode needs to connect to loadDst.
      if ((mask & (mask - 1)) == 0) {
        const unsigned bit = static_cast<unsigned>(__builtin_ctzll(mask));
        if (storeVals[bit] == loadDst) {
          if (entryJoinNode != loadDst)
            addIndirect(entryJoinNode, loadDst);
          return;
        }
      }
      NodeID src = 0;
      auto it = maskToNode.find(mask);
      if (it != maskToNode.end()) {
        src = it->second;
      } else {
        src = addJoin();
        std::uint64_t rem = mask;
        if (hintMask != 0 && (hintMask & ~mask) == 0) {
          addIndirect(hintNode, src);
          rem = mask & ~hintMask;
        } else {
          addIndirect(entryJoinNode, src);
        }
        while (rem) {
          const unsigned bit = static_cast<unsigned>(__builtin_ctzll(rem));
          addIndirect(storeVals[bit], src);
          rem &= rem - 1;
        }
        maskToNode.emplace(mask, src);
      }
      hintNode = src;
      hintMask = mask;
      if (src != loadDst)
        addIndirect(src, loadDst);
    };

    for (std::uint32_t ai = 0; ai < activeComps.size(); ++ai) {
      const std::size_t c = activeComps[ai];
      if (c == rootComp) {
        outMask[ai] = 0;
        continue;
      }

      std::uint64_t inMask = 0;
      for (auto pred : fc.compPreds[c]) {
        if (fc.weakNeededGen[pred] == g)
          inMask |= outMask[fc.weakActiveIdx[pred]];
      }

      if (fc.compCyclic[c]) {
        std::uint64_t localStoreMask = 0;
        std::vector<NodeID> loads;
        for (LocalBlock b : fc.compBlocks[c]) {
          auto it = blockRanges.find(b);
          if (it == blockRanges.end())
            continue;
          for (std::size_t k = it->second.first; k < it->second.second; ++k) {
            if (events[k].isStore) {
              if ((absorbedStore.empty() || !absorbedStore[k]) &&
                  events[k].node != entryJoinNode)
                localStoreMask |= (1ULL << storeIdxOf.at(events[k].node));
            } else if (events[k].node != entryJoinNode) {
              loads.push_back(events[k].node);
            }
          }
        }
        const std::uint64_t finalMask = inMask | localStoreMask;
        outMask[ai] = finalMask;
        if (!loads.empty()) {
          NodeID hintNode = entryJoinNode;
          std::uint64_t hintMask = 0;
          if (inMask != 0) {
            auto mit = maskToNode.find(inMask);
            if (mit != maskToNode.end()) {
              hintNode = mit->second;
              hintMask = inMask;
            }
          }
          for (NodeID load : loads)
            emitLoadWithMask(finalMask, load, hintNode, hintMask);
        }
      } else {
        // Trivial acyclic single-block SCC: preserve exact intra-block order.
        std::uint64_t curMask = inMask;
        const LocalBlock b = fc.compBlocks[c][0];
        auto it = blockRanges.find(b);
        if (it != blockRanges.end()) {
          NodeID hintNode = entryJoinNode;
          std::uint64_t hintMask = 0;
          if (inMask != 0) {
            auto mit = maskToNode.find(inMask);
            if (mit != maskToNode.end()) {
              hintNode = mit->second;
              hintMask = inMask;
            }
          }
          for (std::size_t k = it->second.first; k < it->second.second; ++k) {
            if (events[k].isStore) {
              if ((absorbedStore.empty() || !absorbedStore[k]) &&
                  events[k].node != entryJoinNode)
                curMask |= (1ULL << storeIdxOf.at(events[k].node));
            } else if (events[k].node != entryJoinNode) {
              emitLoadWithMask(curMask, events[k].node, hintNode, hintMask);
            }
          }
        }
        outMask[ai] = curMask;
      }
    }
  }

  void computeStrongLocalFlow(FunctionID fn, NodeID entryJoinNode,
                              std::vector<LocalEvent> &events) {
    auto &fc = funcCFGs[fn];
    const std::uint64_t g = ++fc.gen;

    using SparseNode = std::uint32_t;
    std::vector<std::vector<SparseNode>> sparseSuccs(1);
    std::vector<NodeSet> sparseEdgeSet(1);
    std::vector<std::vector<SparseNode>> sparsePreds(1);
    std::vector<NodeID> sparseStoreValue(1, 0);
    std::vector<bool> sparseStrong(1, false);

    auto addSparseNode = [&](NodeID storeVal, bool strong) -> SparseNode {
      const SparseNode id = nextID(sparseSuccs.size());
      sparseSuccs.emplace_back();
      sparseEdgeSet.emplace_back();
      sparsePreds.emplace_back();
      sparseStoreValue.push_back(storeVal);
      sparseStrong.push_back(strong);
      return id;
    };
    auto addSparseEdge = [&](SparseNode from, SparseNode to) {
      if (!sparseEdgeSet[from].insertBit(to))
        return;
      sparseSuccs[from].push_back(to);
      sparsePreds[to].push_back(from);
      ++stats.sparseGraphEdges;
    };

    // Collect store blocks and create store sparse nodes.
    std::deque<LocalBlock> idfWork;
    for (auto &ev : events) {
      if (!ev.isStore)
        continue;
      ev.sparseNode = addSparseNode(ev.node, ev.isStrong);
      fc.lastStoreSparse[ev.block] = ev.sparseNode;
      if (fc.defBlockGen[ev.block] != g) {
        fc.defBlockGen[ev.block] = g;
        idfWork.push_back(ev.block);
      }
    }

    // Compute iterated dominance frontier IDF(S) on block-level local CFG.
    std::vector<LocalBlock> idfBlocks;
    while (!idfWork.empty()) {
      const LocalBlock u = idfWork.front();
      idfWork.pop_front();
      for (LocalBlock frontier : fc.df[u]) {
        if (fc.idfGen[frontier] == g)
          continue;
        fc.idfGen[frontier] = g;
        fc.phiSparse[frontier] = addSparseNode(0, false);
        idfBlocks.push_back(frontier);
        ++stats.iteratedDominanceFrontierNodes;
        if (fc.defBlockGen[frontier] != g)
          idfWork.push_back(frontier);
      }
    }

    // Memoized upward dominator-tree lookup for nearest sparse definition
    // leaving a block (avoids traversing the entire dominator tree!).
    std::vector<LocalBlock> trail;
    auto getBlockExitDef = [&](LocalBlock start) -> SparseNode {
      trail.clear();
      LocalBlock cur = start;
      SparseNode result = 0;
      while (true) {
        if (cur == 0) {
          result = 0;
          break;
        }
        if (fc.memoGen[cur] == g) {
          result = fc.memoExitDef[cur];
          break;
        }
        if (fc.defBlockGen[cur] == g) {
          result = fc.lastStoreSparse[cur];
          fc.memoGen[cur] = g;
          fc.memoExitDef[cur] = result;
          break;
        }
        if (fc.idfGen[cur] == g) {
          result = fc.phiSparse[cur];
          fc.memoGen[cur] = g;
          fc.memoExitDef[cur] = result;
          break;
        }
        trail.push_back(cur);
        cur = fc.idom[cur];
      }
      for (LocalBlock b : trail) {
        fc.memoGen[b] = g;
        fc.memoExitDef[b] = result;
      }
      return result;
    };

    // Wire intra-block events in exact order.
    std::vector<std::pair<SparseNode, NodeID>> loadDefs;
    for (std::size_t i = 0; i < events.size();) {
      const LocalBlock b = events[i].block;
      std::size_t j = i + 1;
      while (j < events.size() && events[j].block == b)
        ++j;
      SparseNode curDef = fc.idfGen[b] == g ? fc.phiSparse[b]
                                            : getBlockExitDef(fc.idom[b]);
      for (std::size_t k = i; k < j; ++k) {
        if (events[k].isStore) {
          addSparseEdge(curDef, events[k].sparseNode);
          curDef = events[k].sparseNode;
        } else {
          loadDefs.emplace_back(curDef, events[k].node);
        }
      }
      i = j;
    }

    // Wire IDF phi predecessors from block CFG predecessors.
    for (LocalBlock b : idfBlocks) {
      const SparseNode phi = fc.phiSparse[b];
      for (LocalBlock pred : fc.preds[b])
        addSparseEdge(getBlockExitDef(pred), phi);
    }

    stats.sparseGraphNodes += sparseSuccs.size();

    // Condense non-killed SCCs of the sparse graph and factor into join nodes,
    // pruning sparse nodes that cannot reach any load and eliding single-input
    // pass-through phi nodes.
    const std::size_t count = sparseSuccs.size();
    std::vector<std::vector<SparseNode>> liveSuccs(count);
    std::vector<std::vector<SparseNode>> livePreds(count);
    for (SparseNode u = 0; u < count; ++u) {
      for (SparseNode v : sparseSuccs[u]) {
        if (!sparseStrong[v]) {
          liveSuccs[u].push_back(v);
          livePreds[v].push_back(u);
        }
      }
    }
    std::vector<std::uint8_t> needed(count, 0);
    std::vector<SparseNode> needWork;
    needWork.reserve(loadDefs.size());
    for (const auto &[srcSparse, dstNode] : loadDefs) {
      if (!needed[srcSparse]) {
        needed[srcSparse] = 1;
        needWork.push_back(srcSparse);
      }
    }
    while (!needWork.empty()) {
      const SparseNode u = needWork.back();
      needWork.pop_back();
      for (SparseNode pred : livePreds[u]) {
        if (!needed[pred]) {
          needed[pred] = 1;
          needWork.push_back(pred);
        }
      }
    }
    auto [component, components] = computeSCCs(liveSuccs, livePreds);
    std::vector<std::size_t> compSize(components, 0);
    std::vector<SparseNode> compRep(components, 0);
    std::vector<std::uint8_t> compNeeded(components, 0);
    for (SparseNode u = 0; u < count; ++u) {
      ++compSize[component[u]];
      compRep[component[u]] = u;
      if (needed[u])
        compNeeded[component[u]] = 1;
    }
    std::vector<NodeID> compNodes(components, 0);
    compNodes[component[0]] = entryJoinNode;
    for (std::size_t c = 0; c < components; ++c) {
      if (compNodes[c] || !compNeeded[c])
        continue;
      const SparseNode rep = compRep[c];
      // A singleton SCC for a strong store with a value node needs no wrapper
      // join node: emit edges directly from its store value node.
      if (compSize[c] == 1 && sparseStrong[rep] && sparseStoreValue[rep]) {
        compNodes[c] = sparseStoreValue[rep];
      } else if (compSize[c] == 1 && !sparseStrong[rep] &&
                 sparseStoreValue[rep] == 0) {
        NodeID uniquePred = 0;
        bool multiPred = false;
        for (SparseNode pred : livePreds[rep]) {
          const std::size_t pc = component[pred];
          if (pc == c)
            continue;
          const NodeID pn = compNodes[pc];
          if (!pn)
            continue;
          if (!uniquePred)
            uniquePred = pn;
          else if (uniquePred != pn) {
            multiPred = true;
            break;
          }
        }
        compNodes[c] = (!multiPred && uniquePred) ? uniquePred : addJoin();
      } else {
        compNodes[c] = addJoin();
      }
    }
    for (SparseNode u = 1; u < count; ++u) {
      if (!needed[u])
        continue;
      const NodeID cn = compNodes[component[u]];
      if (!cn)
        continue;
      if (sparseStoreValue[u] && sparseStoreValue[u] != cn)
        addIndirect(sparseStoreValue[u], cn);
      if (!sparseStrong[u]) {
        for (SparseNode pred : livePreds[u]) {
          if (component[pred] != component[u]) {
            const NodeID pn = compNodes[component[pred]];
            if (pn && pn != cn)
              addIndirect(pn, cn);
          }
        }
      }
    }
    for (const auto &[srcSparse, dstNode] : loadDefs) {
      const NodeID sn = compNodes[component[srcSparse]];
      if (sn && sn != dstNode)
        addIndirect(sn, dstNode);
    }
  }

  void computeLocalFlowForFunction(FunctionID fn, NodeID entryJoinNode,
                                   std::vector<LocalEvent> &events,
                                   bool hasAnyStrong) {
    bool hasAnyLoad = false;
    for (const auto &ev : events) {
      if (!ev.isStore) {
        hasAnyLoad = true;
        break;
      }
    }
    if (!hasAnyLoad)
      return;
    ++stats.functionFlowComputations;
    std::sort(events.begin(), events.end(),
              [](const LocalEvent &a, const LocalEvent &b) {
                return std::tie(a.block, a.pos, a.phase) <
                       std::tie(b.block, b.pos, b.phase);
              });
    // Coalesce consecutive duplicate weak stores of the same value in the same
    // block (e.g., repeated `store unknown, unknown` calls) with no intervening
    // load or strong store.
    if (events.size() > 1) {
      std::size_t out = 1;
      for (std::size_t i = 1; i < events.size(); ++i) {
        const auto &prev = events[out - 1];
        const auto &cur = events[i];
        if (cur.isStore && !cur.isStrong && prev.isStore &&
            prev.block == cur.block && prev.node == cur.node) {
          continue;
        }
        if (out != i)
          events[out] = cur;
        ++out;
      }
      events.resize(out);
    }
    if (!hasAnyStrong)
      computeWeakLocalFlow(fn, entryJoinNode, events);
    else
      computeStrongLocalFlow(fn, entryJoinNode, events);
  }

  void attachWeakAncCtx(ObjectID obj, std::uint32_t newCtxId) {
    const std::uint32_t oldCtxId = objWeakAncCtx[obj];
    if (oldCtxId != newCtxId) {
      if (oldCtxId != 0)
        weakAncCtxs[oldCtxId].accessedByObjects.erase(obj);
      if (newCtxId != 0)
        weakAncCtxs[newCtxId].accessedByObjects.insertBit(obj);
      objWeakAncCtx[obj] = newCtxId;
    }
  }

  template <typename ActivePred>
  void wireUnivExtInactive(NodeID readSrcNode, NodeID modDstNode,
                           const UnivExtSummary &extSum,
                           std::size_t totalActiveTargets,
                           ActivePred &&isCanonTargetActive,
                           const std::vector<FunctionID> &excludedCanonTargets) {
    if (modDstNode) {
      for (const auto &grp : extSum.modGroups) {
        if (!grp.summaryNode || grp.summaryNode == modDstNode)
          continue;
        bool hasInactive = (grp.canonTargets.size() > totalActiveTargets);
        if (!hasInactive) {
          for (FunctionID ct : grp.canonTargets) {
            if (!isCanonTargetActive(ct)) {
              hasInactive = true;
              break;
            }
          }
        }
        if (hasInactive)
          addIndirect(grp.summaryNode, modDstNode);
      }
    }
    const auto &seg = extSum.readSeg;
    if (!readSrcNode || seg.entries.empty())
      return;
    const std::uint32_t M = static_cast<std::uint32_t>(seg.entries.size());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> excluded;
    if (!excludedCanonTargets.empty()) {
      excluded.reserve(excludedCanonTargets.size());
      for (FunctionID ct : excludedCanonTargets) {
        auto it = std::lower_bound(
            seg.canonRanges.begin(), seg.canonRanges.end(), ct,
            [](const auto &entry, FunctionID key) { return entry.first < key; });
        if (it != seg.canonRanges.end() && it->first == ct)
          excluded.push_back(it->second);
      }
    }
    if (excluded.empty()) {
      if (seg.tree[1] && readSrcNode != seg.tree[1] &&
          addIndirect(readSrcNode, seg.tree[1])) {
        if (!localCap.active)
          ++stats.interproceduralAuxEdges;
      }
      return;
    }
    std::sort(excluded.begin(), excluded.end());
    auto wireRange = [&](std::uint32_t l, std::uint32_t r) {
      std::size_t L = seg.leafBase + l;
      std::size_t R = seg.leafBase + r;
      while (L < R) {
        if (L & 1U) {
          if (seg.tree[L] && readSrcNode != seg.tree[L] &&
              addIndirect(readSrcNode, seg.tree[L])) {
            if (!localCap.active)
              ++stats.interproceduralAuxEdges;
          }
          ++L;
        }
        if (R & 1U) {
          --R;
          if (seg.tree[R] && readSrcNode != seg.tree[R] &&
              addIndirect(readSrcNode, seg.tree[R])) {
            if (!localCap.active)
              ++stats.interproceduralAuxEdges;
          }
        }
        L >>= 1;
        R >>= 1;
      }
    };
    std::uint32_t cur = 0;
    for (const auto &rng : excluded) {
      if (cur < rng.first)
        wireRange(cur, rng.first);
      if (rng.second > cur)
        cur = rng.second;
    }
    if (cur < M)
      wireRange(cur, M);
  }

  // Build an exact Theorem Y `WeakAncestorContext` summarizing the entire
  // zero-own-access strict-ancestor subgraph `ancFuncs` above `coreFuncs`.
  // On entry, `fnActiveGen`, `fnTransModGen`, `fnTransRefGen` are stamped with
  // `g` for BOTH `ancFuncs` and `coreFuncs`.
  template <typename CarryPred>
  std::uint32_t buildWeakAncestorContext(
      std::uint64_t g, bool useUniv, bool globalVisible, FunctionID scopeRoot,
      bool ancHubActive, bool ancHubMod,
      const std::vector<FunctionID> &coreFuncs,
      const std::vector<FunctionID> &ancFuncs,
      const std::unordered_map<std::uint32_t, std::vector<CallSiteID>>
          &univCarrierSites,
      CarryPred &&callSiteCanCarryObject) {
    const FunctionID hubIdx = static_cast<FunctionID>(p.functions_.size());
    const std::size_t nAnc = ancFuncs.size();
    const std::size_t nCore = coreFuncs.size();
    const std::size_t nAll = nAnc + nCore;
    auto canon = [&](FunctionID f) -> FunctionID {
      if (useUniv) {
        if (const std::uint32_t u = fnUnivScc[f])
          return univSccs[u].repFunc;
      }
      return f;
    };
    auto univIdOfRep = [&](FunctionID rep) -> std::uint32_t {
      return useUniv ? fnUnivScc[rep] : 0U;
    };
    auto isTransMod = [&](FunctionID f) { return fnTransModGen[canon(f)] == g; };
    auto isTransRef = [&](FunctionID f) { return fnTransRefGen[canon(f)] == g; };

    // Index `ANC` at [0, nAnc) and `CORE` boundary ports at [nAnc, nAnc + nCore).
    // Note: `coreFuncs` may include `hubIdx` as a port when `hub` sits in `CORE`!
    bool hubIsPort = false;
    std::size_t hubPortAllIdx = nAnc;
    for (std::size_t i = 0; i < nAnc; ++i)
      fnActiveIdx[ancFuncs[i]] = static_cast<std::uint32_t>(i);
    for (std::size_t ci = 0; ci < nCore; ++ci) {
      if (coreFuncs[ci] == hubIdx) {
        hubIsPort = true;
        hubPortAllIdx = nAnc + ci;
      }
      fnActiveIdx[coreFuncs[ci]] = static_cast<std::uint32_t>(nAnc + ci);
    }

    // Pre-build persistent segment-tree external summaries for any UnivWeakSCC
    // in `ANC` (and `hubExtSummary` if `ancHubActive`) BEFORE `localCap.active`!
    if (useUniv) {
      for (std::size_t i = 0; i < nAnc; ++i) {
        if (const std::uint32_t u = univIdOfRep(ancFuncs[i]))
          ensureUnivExtSummary(u, globalVisible);
      }
    }
    if (ancHubActive)
      ensureHubExtSummary(useUniv);

    // Determine which `CORE` ports have a returning carrying caller in `ANC`.
    std::vector<std::uint8_t> coreCalledRetByAnc(nCore, 0);
    for (std::size_t ci = 0; ci < nCore; ++ci) {
      const FunctionID rep = coreFuncs[ci];
      if (!isTransMod(rep))
        continue;
      if (rep == hubIdx) {
        for (CallSiteID site : unknownHubCallerSites) {
          const auto &cs = p.callSites_[site];
          if (!blockReachable[cs.block] || !cs.mayReturn ||
              !callSiteCanCarryObject(site))
            continue;
          const FunctionID cRep = canon(p.blocks_[cs.block].function);
          if (fnActiveGen[cRep] == g && fnActiveIdx[cRep] < nAnc) {
            coreCalledRetByAnc[ci] = 1;
            break;
          }
        }
      } else if (const std::uint32_t u = univIdOfRep(rep)) {
        for (CallSiteID site : univSccs[u].extCallerSites) {
          const auto &cs = p.callSites_[site];
          if (!blockReachable[cs.block] || !cs.mayReturn ||
              !callSiteCanCarryObject(site))
            continue;
          const FunctionID cRep = canon(p.blocks_[cs.block].function);
          if (fnActiveGen[cRep] == g && fnActiveIdx[cRep] < nAnc) {
            coreCalledRetByAnc[ci] = 1;
            break;
          }
        }
      } else if (funcCFGs[rep].exitLocal != InvalidLocal &&
                 !(scopeRoot && rep == scopeRoot)) {
        for (CallSiteID site : callerSites[rep]) {
          const auto &cs = p.callSites_[site];
          if (!blockReachable[cs.block] || !cs.mayReturn ||
              !callSiteCanCarryObject(site))
            continue;
          const FunctionID cRep = canon(p.blocks_[cs.block].function);
          if (fnActiveGen[cRep] == g && fnActiveIdx[cRep] < nAnc) {
            coreCalledRetByAnc[ci] = 1;
            break;
          }
        }
      }
    }
    if (ancHubActive && ancHubMod) {
      for (FunctionID atFn : addressTakenFuncs) {
        if (!funcReachable[atFn])
          continue;
        const FunctionID ca = canon(atFn);
        if (fnActiveGen[ca] == g && fnActiveIdx[ca] >= nAnc &&
            isTransMod(ca)) {
          coreCalledRetByAnc[fnActiveIdx[ca] - nAnc] = 1;
        }
      }
    }

    // Preprocess `ANC` callsites & UnivWeakSCC active external callees.
    struct AncUnivExt {
      std::vector<std::size_t> activeRetIdxs;   // indices in [0, nAll)
      std::vector<std::size_t> activeNoRetIdxs; // indices in [0, nAll)
      bool activeRetHub = false;
      bool activeNoRetHub = false;
      std::vector<FunctionID> extraRetCallees;
      std::vector<FunctionID> extraNoRetCallees;
      std::vector<FunctionID> extraNonExitRetFuncs;
    };
    std::vector<std::vector<CallFlowSummary>> ancCalls(nAnc);
    std::vector<std::vector<UnknownCallMod>> ancUnkMods(nAnc);
    std::vector<std::vector<UnknownCallMod>> ancUnkReads(nAnc);
    std::vector<AncUnivExt> ancUnivExt(useUniv ? nAnc : 0);
    std::vector<std::uint8_t> ancNeedExit(nAnc, 0);

    const bool hubInUniv = useUniv && (fnUnivScc[hubIdx] != 0);
    const bool hubActive =
        ancHubActive || hubIsPort ||
        (hubInUniv && unknownHubActive &&
         (isTransMod(hubIdx) || isTransRef(hubIdx)));
    const std::size_t ancHubLocalIdx = nAnc;
    const std::size_t ancHubTargetIdx =
        hubIsPort
            ? hubPortAllIdx
            : (hubInUniv && fnActiveGen[canon(hubIdx)] == g
                   ? static_cast<std::size_t>(fnActiveIdx[canon(hubIdx)])
                   : ancHubLocalIdx);

    for (std::size_t i = 0; i < nAnc; ++i) {
      const FunctionID rep = ancFuncs[i];
      if (const std::uint32_t u = univIdOfRep(rep)) {
        const auto &scc = univSccs[u];
        auto &ext = ancUnivExt[i];
        const auto &retCanon = globalVisible ? scc.globalRetExtCanonSet
                                             : scc.alwaysRetExtCanonSet;
        const auto &noRetCanon = globalVisible ? scc.globalNoRetExtCanonSet
                                               : scc.alwaysNoRetExtCanonSet;
        for (std::size_t j = 0; j < nAnc; ++j) {
          if (j == i)
            continue;
          const FunctionID rj = ancFuncs[j];
          if (retCanon.count(rj))
            ext.activeRetIdxs.push_back(j);
          else if (noRetCanon.count(rj))
            ext.activeNoRetIdxs.push_back(j);
        }
        for (std::size_t ci = 0; ci < nCore; ++ci) {
          const FunctionID rj = coreFuncs[ci];
          if (rj == hubIdx)
            continue;
          if (retCanon.count(rj))
            ext.activeRetIdxs.push_back(nAnc + ci);
          else if (noRetCanon.count(rj))
            ext.activeNoRetIdxs.push_back(nAnc + ci);
        }
        if (ancHubActive || hubIsPort) {
          if (globalVisible ? scc.globalRetCallsHub : scc.alwaysRetCallsHub)
            ext.activeRetHub = true;
          else if (globalVisible ? scc.globalNoRetCallsHub
                                 : scc.alwaysNoRetCallsHub)
            ext.activeNoRetHub = true;
        }
        if (!globalVisible) {
          auto csIt = univCarrierSites.find(u);
          if (csIt != univCarrierSites.end()) {
            for (CallSiteID site : csIt->second) {
              const auto &cs = p.callSites_[site];
              const FunctionID caller = p.blocks_[cs.block].function;
              const bool reachesExit =
                  funcCFGs[caller].canReachExit[blockToLocal[cs.block]];
              const bool isRetExit = cs.mayReturn && reachesExit;
              const bool isNonExitRet = cs.mayReturn && !reachesExit;
              auto addExtra = [&](FunctionID v) {
                if (fnUnivScc[v] == u)
                  return;
                if (isNonExitRet)
                  appendUnique(ext.extraNonExitRetFuncs, caller);
                if (isRetExit) {
                  if (!scc.alwaysRetExtRawSet.count(v))
                    appendUnique(ext.extraRetCallees, v);
                } else {
                  if (!scc.alwaysRetExtRawSet.count(v) &&
                      !scc.alwaysNoRetExtRawSet.count(v))
                    appendUnique(ext.extraNoRetCallees, v);
                }
              };
              for (FunctionID v : liveCallTargets[site])
                addExtra(v);
              if (siteCallsUnknownHub[site] && !scc.containsHub &&
                  unknownHubActive)
                addExtra(hubIdx);
            }
          }
        }
        bool hasRelCaller = false;
        if (isTransMod(rep)) {
          for (CallSiteID site : scc.extCallerSites) {
            const auto &cs = p.callSites_[site];
            if (!blockReachable[cs.block] || !cs.mayReturn ||
                !callSiteCanCarryObject(site))
              continue;
            const FunctionID cRep = canon(p.blocks_[cs.block].function);
            if (fnActiveGen[cRep] == g && fnActiveIdx[cRep] < nAnc) {
              hasRelCaller = true;
              break;
            }
          }
          if (!hasRelCaller && ancHubActive && ancHubMod) {
            for (FunctionID m : scc.members) {
              if (isAddressTakenFunc[m]) {
                hasRelCaller = true;
                break;
              }
            }
          }
        }
        ancNeedExit[i] = hasRelCaller ? 1 : 0;
        continue;
      }

      const FunctionID f = rep;
      auto &summaries = ancCalls[i];
      auto &unkMods = ancUnkMods[i];
      auto &unkReads = ancUnkReads[i];
      for (CallSiteID site : funcCallSites[f]) {
        if (!callSiteCanCarryObject(site))
          continue;
        const auto &targets = liveCallTargets[site];
        bool anyAccess = false;
        bool anyUnknownRead = false;
        bool anyMod = false;
        bool hasHub = false;
        std::vector<NodeID> siteUnkSummaries;
        for (FunctionID callee : targets) {
          if (isTransMod(callee) || isTransRef(callee)) {
            anyAccess = true;
            if (isTransMod(callee))
              anyMod = true;
          } else {
            if (unknownTransRef[callee])
              anyUnknownRead = true;
            if (p.callSites_[site].mayReturn && unknownModSummary[callee])
              appendUnique(siteUnkSummaries, unknownModSummary[callee]);
          }
        }
        if (siteCallsUnknownHub[site]) {
          if (hubActive) {
            anyAccess = true;
            hasHub = true;
            if (isTransMod(hubIdx))
              anyMod = true;
          } else {
            if (unknownTransRef[hubIdx]) {
              anyUnknownRead = true;
              hasHub = true;
            }
            if (p.callSites_[site].mayReturn && unknownModSummary[hubIdx])
              appendUnique(siteUnkSummaries, unknownModSummary[hubIdx]);
          }
        }
        for (NodeID sumNode : siteUnkSummaries)
          unkMods.push_back({site, sumNode});
        if (!anyAccess && !anyUnknownRead)
          continue;
        if (!anyAccess && anyUnknownRead && !hasHub &&
            p.callSites_[site].calleePointer == 0 && targets.size() == 1) {
          unkReads.push_back({site, unknownReadEntry[*targets.begin()]});
          continue;
        }
        const bool hasIn = anyAccess || anyUnknownRead;
        const bool hasOut = (anyMod && p.callSites_[site].mayReturn);
        summaries.push_back({site, hasIn, hasOut, false, hasHub});
      }
      bool hasRelCaller = false;
      if (isTransMod(f) && funcCFGs[f].exitLocal != InvalidLocal &&
          !(scopeRoot && f == scopeRoot)) {
        for (CallSiteID site : callerSites[f]) {
          const auto &cs = p.callSites_[site];
          if (!blockReachable[cs.block] || !cs.mayReturn ||
              !callSiteCanCarryObject(site))
            continue;
          const FunctionID cRep = canon(p.blocks_[cs.block].function);
          if (fnActiveGen[cRep] == g && fnActiveIdx[cRep] < nAnc) {
            hasRelCaller = true;
            break;
          }
        }
        if (!hasRelCaller && unknownHubActive && isAddressTakenFunc[f]) {
          if (hubInUniv) {
            const FunctionID hRep = canon(hubIdx);
            if (fnActiveGen[hRep] == g && fnActiveIdx[hRep] < nAnc)
              hasRelCaller = true;
          } else if (ancHubActive && ancHubMod) {
            hasRelCaller = true;
          }
        }
      }
      ancNeedExit[i] = hasRelCaller ? 1 : 0;
    }

    // Emit `ANC` into `localCap` with isolated boundary ports for `INIT` and
    // `CORE`.
    localCap.reset();
    localCap.active = true;

    const NodeID initPort = localCap.allocNode();
    std::vector<NodeID> actEntry(nAll, 0);
    std::vector<NodeID> actExit(nAll, 0);
    for (std::size_t ci = 0; ci < nCore; ++ci) {
      actEntry[nAnc + ci] = localCap.allocNode();
      if (coreCalledRetByAnc[ci])
        actExit[nAnc + ci] = localCap.allocNode();
    }

    // Weak call-graph SCC contraction over `ANC` (+ `ancHub` if active).
    std::vector<std::uint8_t> weakEntryElig(nAnc + 1, 1);
    std::vector<std::uint8_t> weakExitElig(nAnc + 1, 0);
    for (std::size_t i = 0; i < nAnc; ++i)
      weakExitElig[i] = ancNeedExit[i];
    weakEntryElig[ancHubLocalIdx] = ancHubActive ? 1 : 0;
    weakExitElig[ancHubLocalIdx] = (ancHubActive && ancHubMod) ? 1 : 0;

    std::vector<std::vector<std::size_t>> entrySucc(nAnc + 1);
    std::vector<std::vector<std::size_t>> retSucc(nAnc + 1);
    auto addAncGraphEdge = [&](std::size_t i, std::size_t j, bool hasIn,
                               bool retReachesExit) {
      if (j > nAnc || j == i)
        return; // only ANC-to-ANC edges (j < nAnc or j == ancHubLocalIdx)
      if (j == ancHubLocalIdx && !ancHubActive)
        return;
      if (hasIn && weakEntryElig[j]) {
        if (entrySucc[i].empty() || entrySucc[i].back() != j)
          entrySucc[i].push_back(j);
      }
      if (retReachesExit && weakExitElig[i] && weakExitElig[j]) {
        if (retSucc[j].empty() || retSucc[j].back() != i)
          retSucc[j].push_back(i);
      }
    };

    for (std::size_t i = 0; i < nAnc; ++i) {
      const FunctionID rep = ancFuncs[i];
      if (univIdOfRep(rep) != 0) {
        const auto &ext = ancUnivExt[i];
        for (std::size_t j : ext.activeRetIdxs)
          if (j < nAnc)
            addAncGraphEdge(i, j, true, true);
        for (std::size_t j : ext.activeNoRetIdxs)
          if (j < nAnc)
            addAncGraphEdge(i, j, true, false);
        if (ext.activeRetHub)
          addAncGraphEdge(i, ancHubLocalIdx, true, true);
        else if (ext.activeNoRetHub)
          addAncGraphEdge(i, ancHubLocalIdx, true, false);
        auto addExtraEdge = [&](FunctionID v, bool isRet) {
          const FunctionID cv = canon(v);
          if (cv == hubIdx) {
            if (ancHubActive)
              addAncGraphEdge(i, ancHubLocalIdx, true, isRet);
          } else if (fnActiveGen[cv] == g && fnActiveIdx[cv] < nAnc) {
            addAncGraphEdge(i, fnActiveIdx[cv], true, isRet);
          }
        };
        for (FunctionID v : ext.extraRetCallees)
          addExtraEdge(v, true);
        for (FunctionID v : ext.extraNoRetCallees)
          addExtraEdge(v, false);
        continue;
      }
      const auto &fc = funcCFGs[rep];
      for (const auto &cs : ancCalls[i]) {
        const bool retReachesExit =
            weakExitElig[i] && cs.hasOut &&
            fc.canReachExit[blockToLocal[p.callSites_[cs.site].block]];
        for (FunctionID callee : liveCallTargets[cs.site]) {
          const FunctionID cc = canon(callee);
          if (fnActiveGen[cc] == g && fnActiveIdx[cc] < nAnc)
            addAncGraphEdge(i, fnActiveIdx[cc], cs.hasIn, retReachesExit);
        }
        if (cs.hasHub && hubActive && ancHubTargetIdx < nAnc + 1) {
          if (ancHubTargetIdx < nAnc || ancHubActive)
            addAncGraphEdge(i, ancHubTargetIdx, cs.hasIn, retReachesExit);
        }
      }
    }
    if (ancHubActive) {
      for (FunctionID callee : addressTakenFuncs) {
        const FunctionID cc = canon(callee);
        if (fnActiveGen[cc] == g && fnActiveIdx[cc] < nAnc) {
          const std::size_t j = fnActiveIdx[cc];
          addAncGraphEdge(ancHubLocalIdx, j, true, ancHubMod);
        }
      }
    }

    auto entrySccRes = computeSCCs(entrySucc);
    const auto &entryComp = entrySccRes.first;
    const std::size_t entryComps = entrySccRes.second;
    std::vector<std::size_t> entryCompSize(entryComps, 0);
    for (std::size_t i = 0; i <= nAnc; ++i)
      if (weakEntryElig[i])
        ++entryCompSize[entryComp[i]];

    auto retSccRes = computeSCCs(retSucc);
    const auto &retComp = retSccRes.first;
    const std::size_t retComps = retSccRes.second;
    std::vector<std::size_t> retCompSize(retComps, 0);
    for (std::size_t i = 0; i <= nAnc; ++i)
      if (weakExitElig[i])
        ++retCompSize[retComp[i]];

    std::vector<std::vector<std::size_t>> entryCompMembers(entryComps);
    for (std::size_t i = 0; i <= nAnc; ++i)
      entryCompMembers[entryComp[i]].push_back(i);
    std::vector<std::vector<std::size_t>> retCompMembers(retComps);
    for (std::size_t i = 0; i <= nAnc; ++i)
      retCompMembers[retComp[i]].push_back(i);

    const std::size_t totalU = entryComps + retComps;
    std::vector<std::vector<std::size_t>> uniSucc(totalU);
    auto addUniEdge = [&](std::size_t u, std::size_t v) {
      if (u != v && (uniSucc[u].empty() || uniSucc[u].back() != v))
        uniSucc[u].push_back(v);
    };
    for (std::size_t i = 0; i <= nAnc; ++i) {
      if (weakEntryElig[i] && weakExitElig[i]) {
        addUniEdge(entryComp[i], entryComps + retComp[i]);
        if (i < nAnc && univIdOfRep(ancFuncs[i]) != 0 &&
            isTransMod(ancFuncs[i]))
          addUniEdge(entryComps + retComp[i], entryComp[i]);
      }
      for (std::size_t j : entrySucc[i])
        addUniEdge(entryComp[i], entryComp[j]);
      for (std::size_t j : retSucc[i])
        addUniEdge(entryComps + retComp[i], entryComps + retComp[j]);
    }
    for (std::size_t i = 0; i < nAnc; ++i) {
      if (univIdOfRep(ancFuncs[i]) != 0)
        continue;
      const FunctionID f = ancFuncs[i];
      struct CallEp {
        LocalBlock block;
        std::uint32_t pos;
        std::size_t comp;
      };
      std::vector<CallEp> outCalls, inCalls;
      for (const auto &cs : ancCalls[i]) {
        const LocalBlock b = blockToLocal[p.callSites_[cs.site].block];
        const std::uint32_t pos = callEventPos[cs.site];
        if (cs.hasOut) {
          for (FunctionID callee : liveCallTargets[cs.site]) {
            const FunctionID cc = canon(callee);
            if (fnActiveGen[cc] == g && fnActiveIdx[cc] < nAnc &&
                weakExitElig[fnActiveIdx[cc]])
              outCalls.push_back({b, pos, retComp[fnActiveIdx[cc]]});
          }
          if (cs.hasHub && hubActive && ancHubTargetIdx <= nAnc &&
              weakExitElig[ancHubTargetIdx])
            outCalls.push_back({b, pos, retComp[ancHubTargetIdx]});
        }
        if (cs.hasIn) {
          for (FunctionID callee : liveCallTargets[cs.site]) {
            const FunctionID cc = canon(callee);
            if (fnActiveGen[cc] == g && fnActiveIdx[cc] < nAnc &&
                weakEntryElig[fnActiveIdx[cc]])
              inCalls.push_back({b, pos, entryComp[fnActiveIdx[cc]]});
          }
          if (cs.hasHub && hubActive && ancHubTargetIdx <= nAnc &&
              weakEntryElig[ancHubTargetIdx])
            inCalls.push_back({b, pos, entryComp[ancHubTargetIdx]});
        }
      }
      if (outCalls.empty() || inCalls.empty())
        continue;
      ensureFunctionSCCs(f);
      auto &fc = funcCFGs[f];
      std::vector<std::size_t> distinctTargets;
      for (const auto &ic : inCalls)
        appendUnique(distinctTargets, ic.comp);
      if (distinctTargets.size() <= 8) {
        for (std::size_t targetE : distinctTargets) {
          const std::uint64_t ug = ++fc.weakGen;
          std::vector<std::size_t> bWork;
          std::vector<bool> compHasIn(fc.compBlocks.size(), false);
          std::unordered_map<LocalBlock, std::uint32_t> maxInPos;
          for (const auto &ic : inCalls) {
            if (ic.comp != targetE)
              continue;
            auto &mp = maxInPos[ic.block];
            if (ic.pos > mp)
              mp = ic.pos;
            const std::size_t bc = fc.comp[ic.block];
            if (!compHasIn[bc]) {
              compHasIn[bc] = true;
              for (auto pred : fc.compPreds[bc]) {
                if (fc.weakNeededGen[pred] != ug) {
                  fc.weakNeededGen[pred] = ug;
                  bWork.push_back(pred);
                }
              }
            }
          }
          while (!bWork.empty()) {
            const std::size_t bc = bWork.back();
            bWork.pop_back();
            for (auto pred : fc.compPreds[bc]) {
              if (fc.weakNeededGen[pred] != ug) {
                fc.weakNeededGen[pred] = ug;
                bWork.push_back(pred);
              }
            }
          }
          for (const auto &oc : outCalls) {
            const std::size_t bc = fc.comp[oc.block];
            bool reaches = (fc.weakNeededGen[bc] == ug) ||
                           (compHasIn[bc] && fc.compCyclic[bc]);
            if (!reaches) {
              auto mit = maxInPos.find(oc.block);
              if (mit != maxInPos.end() && oc.pos < mit->second)
                reaches = true;
            }
            if (reaches)
              addUniEdge(entryComps + oc.comp, targetE);
          }
        }
      } else {
        for (const auto &oc : outCalls) {
          const std::size_t bc = fc.comp[oc.block];
          for (const auto &ic : inCalls) {
            if ((oc.block == ic.block && oc.pos < ic.pos) ||
                (fc.comp[ic.block] == bc && fc.compCyclic[bc])) {
              addUniEdge(entryComps + oc.comp, ic.comp);
            }
          }
        }
      }
    }

    auto uniSccRes = computeSCCs(uniSucc);
    const auto &uniComp = uniSccRes.first;
    const std::size_t uniComps = uniSccRes.second;
    std::vector<std::size_t> uniTotalElig(uniComps, 0);
    for (std::size_t c = 0; c < entryComps; ++c)
      uniTotalElig[uniComp[c]] += entryCompSize[c];
    for (std::size_t r = 0; r < retComps; ++r)
      uniTotalElig[uniComp[entryComps + r]] += retCompSize[r];

    std::vector<NodeID> uniCompNode(uniComps, 0);
    NodeID hubEntry = 0;
    NodeID hubExit = 0;
    for (std::size_t uc = 0; uc < uniComps; ++uc) {
      if (uniTotalElig[uc] > 1)
        uniCompNode[uc] = addJoin();
    }
    for (std::size_t c = 0; c < entryComps; ++c) {
      const NodeID cn = uniCompNode[uniComp[c]];
      if (!cn)
        continue;
      for (std::size_t i : entryCompMembers[c]) {
        if (i == ancHubLocalIdx) {
          if (ancHubActive)
            hubEntry = cn;
          continue;
        }
        actEntry[i] = cn;
      }
    }
    for (std::size_t r = 0; r < retComps; ++r) {
      const NodeID cn = uniCompNode[uniComp[entryComps + r]];
      if (!cn)
        continue;
      for (std::size_t i : retCompMembers[r]) {
        if (i == ancHubLocalIdx) {
          if (ancHubActive && ancHubMod)
            hubExit = cn;
          continue;
        }
        if (ancNeedExit[i])
          actExit[i] = cn;
      }
    }

    // Assign remaining singleton actEntry (with Theorem F-Dual collapse).
    for (std::size_t idx = entryComps; idx > 0; --idx) {
      const std::size_t c = idx - 1;
      if (uniCompNode[uniComp[c]])
        continue;
      for (std::size_t i : entryCompMembers[c]) {
        if (i == ancHubLocalIdx) {
          if (ancHubActive && !hubEntry)
            hubEntry = addJoin();
          continue;
        }
        if (actEntry[i])
          continue;
        const FunctionID f = ancFuncs[i];
        if (univIdOfRep(f) != 0) {
          actEntry[i] = addJoin();
          continue;
        }
        NodeID collapsedEntry = 0;
        if (!ancNeedExit[i] &&
            unknownFnAccesses.find(f) == unknownFnAccesses.end() &&
            ancUnkMods[i].empty()) {
          bool anyOut = false;
          NodeID uniqueDst = 0;
          bool multipleDst = false;
          auto observeDst = [&](NodeID d) {
            if (!d)
              return;
            if (!uniqueDst)
              uniqueDst = d;
            else if (uniqueDst != d)
              multipleDst = true;
          };
          for (const auto &ur : ancUnkReads[i])
            observeDst(ur.summaryNode);
          for (const auto &cs : ancCalls[i]) {
            if (cs.hasOut) {
              anyOut = true;
              break;
            }
            if (!cs.hasIn)
              continue;
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee) || isTransRef(callee)) {
                const NodeID ce = actEntry[fnActiveIdx[canon(callee)]];
                if (!ce)
                  multipleDst = true;
                else
                  observeDst(ce);
              } else if (unknownTransRef[callee]) {
                observeDst(unknownReadEntry[callee]);
              }
            }
            if (cs.hasHub) {
              if (hubActive) {
                const NodeID he = (hubInUniv || hubIsPort)
                                      ? actEntry[ancHubTargetIdx]
                                      : hubEntry;
                if (!he)
                  multipleDst = true;
                else
                  observeDst(he);
              } else if (unknownTransRef[hubIdx]) {
                observeDst(unknownReadEntry[hubIdx]);
              }
            }
          }
          if (!anyOut && uniqueDst && !multipleDst)
            collapsedEntry = uniqueDst;
        }
        actEntry[i] = collapsedEntry ? collapsedEntry : addJoin();
      }
    }

    // Assign remaining singleton actExit (with Theorem F collapse).
    for (std::size_t c = 0; c < retComps; ++c) {
      if (uniCompNode[uniComp[entryComps + c]])
        continue;
      for (std::size_t i : retCompMembers[c]) {
        if (i == ancHubLocalIdx) {
          if (ancHubActive && ancHubMod && !hubExit)
            hubExit = addJoin();
          continue;
        }
        if (actExit[i] || !ancNeedExit[i])
          continue;
        const FunctionID f = ancFuncs[i];
        if (univIdOfRep(f) != 0) {
          actExit[i] = isTransMod(f) ? actEntry[i] : addJoin();
          continue;
        }
        NodeID collapsedExit = 0;
        const auto &fc = funcCFGs[f];
        bool hasExitLocalStore = (fnLocalExitUnkStoreNode[f] != 0);
        if (!hasExitLocalStore) {
          for (const auto &um : ancUnkMods[i]) {
            if (fc.canReachExit[blockToLocal[p.callSites_[um.site].block]]) {
              hasExitLocalStore = true;
              break;
            }
          }
        }
        if (!hasExitLocalStore) {
          NodeID uniqueExit = 0;
          bool multipleExit = false;
          auto observeExit = [&](NodeID e) {
            if (!e)
              return;
            if (!uniqueExit)
              uniqueExit = e;
            else if (uniqueExit != e)
              multipleExit = true;
          };
          for (const auto &cs : ancCalls[i]) {
            if (!cs.hasOut ||
                !fc.canReachExit[blockToLocal[p.callSites_[cs.site].block]])
              continue;
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee)) {
                const NodeID cx = actExit[fnActiveIdx[canon(callee)]];
                if (!cx)
                  multipleExit = true;
                else
                  observeExit(cx);
              }
            }
            if (cs.hasHub && hubActive && isTransMod(hubIdx)) {
              const NodeID hx = (hubInUniv || hubIsPort)
                                    ? actExit[ancHubTargetIdx]
                                    : hubExit;
              if (!hx)
                multipleExit = true;
              else
                observeExit(hx);
            }
          }
          if (uniqueExit && !multipleExit)
            collapsedExit = uniqueExit;
        }
        actExit[i] = collapsedExit ? collapsedExit : addJoin();
      }
    }

    if ((hubInUniv || hubIsPort) && hubActive) {
      hubEntry = actEntry[ancHubTargetIdx];
      hubExit = actExit[ancHubTargetIdx];
    }

    if (ancHubActive) {
      std::vector<FunctionID> excludedHubCanon;
      for (std::size_t j = 0; j < nAnc; ++j) {
        const FunctionID rep = ancFuncs[j];
        const std::uint32_t u = univIdOfRep(rep);
        bool hasAT = false;
        if (u != 0) {
          for (FunctionID m : univSccs[u].members) {
            if (isAddressTakenFunc[m]) {
              hasAT = true;
              break;
            }
          }
        } else {
          hasAT = isAddressTakenFunc[rep];
        }
        if (!hasAT)
          continue;
        excludedHubCanon.push_back(rep);
        if (hubEntry && actEntry[j] && hubEntry != actEntry[j])
          addIndirect(hubEntry, actEntry[j]);
        if (hubExit && isTransMod(rep) && actExit[j] && actExit[j] != hubExit)
          addIndirect(actExit[j], hubExit);
      }
      for (std::size_t ci = 0; ci < nCore; ++ci) {
        const FunctionID rep = coreFuncs[ci];
        if (rep == hubIdx)
          continue;
        const std::uint32_t u = univIdOfRep(rep);
        bool hasAT = false;
        if (u != 0) {
          for (FunctionID m : univSccs[u].members) {
            if (isAddressTakenFunc[m]) {
              hasAT = true;
              break;
            }
          }
        } else {
          hasAT = isAddressTakenFunc[rep];
        }
        if (!hasAT)
          continue;
        excludedHubCanon.push_back(rep);
        if (hubEntry && actEntry[nAnc + ci] && hubEntry != actEntry[nAnc + ci])
          addIndirect(hubEntry, actEntry[nAnc + ci]);
        if (hubExit && isTransMod(rep) && actExit[nAnc + ci] &&
            actExit[nAnc + ci] != hubExit)
          addIndirect(actExit[nAnc + ci], hubExit);
      }
      auto isAtCanonAct = [&](FunctionID ct) -> bool {
        return fnActiveGen[ct] == g;
      };
      wireUnivExtInactive(hubEntry, hubExit, hubExtSummary, nAll, isAtCanonAct,
                          excludedHubCanon);
    }

    // Compute `ancInitReached` over `ANC` only (since `CORE` never calls `ANC`).
    std::vector<std::uint8_t> ancInitReached(nAnc, 0);
    std::vector<std::size_t> initWork;
    bool hubInitReached = false;
    for (std::size_t i = 0; i < nAnc; ++i) {
      const FunctionID rep = ancFuncs[i];
      const std::uint32_t u = univIdOfRep(rep);
      const bool isEntry =
          p.entryFunctions_.count(rep) || (u != 0 && univSccs[u].containsEntry);
      if (isEntry || (scopeRoot && rep == scopeRoot)) {
        ancInitReached[i] = 1;
        initWork.push_back(i);
      }
    }
    auto markAncInitCallee = [&](FunctionID callee) {
      const FunctionID cc = canon(callee);
      if (cc == hubIdx) {
        if (ancHubActive && !hubInitReached) {
          hubInitReached = true;
          for (FunctionID atFn : addressTakenFuncs) {
            const FunctionID ca = canon(atFn);
            if (fnActiveGen[ca] == g && fnActiveIdx[ca] < nAnc) {
              const std::size_t ci = fnActiveIdx[ca];
              if (!ancInitReached[ci]) {
                ancInitReached[ci] = 1;
                initWork.push_back(ci);
              }
            }
          }
        }
        return;
      }
      if (fnActiveGen[cc] == g && fnActiveIdx[cc] < nAnc) {
        const std::size_t ci = fnActiveIdx[cc];
        if (!ancInitReached[ci]) {
          ancInitReached[ci] = 1;
          initWork.push_back(ci);
        }
      }
    };
    while (!initWork.empty()) {
      const std::size_t idx = initWork.back();
      initWork.pop_back();
      const FunctionID rep = ancFuncs[idx];
      if (univIdOfRep(rep) != 0) {
        const auto &ext = ancUnivExt[idx];
        for (std::size_t j : ext.activeRetIdxs)
          if (j < nAnc && !ancInitReached[j]) {
            ancInitReached[j] = 1;
            initWork.push_back(j);
          }
        for (std::size_t j : ext.activeNoRetIdxs)
          if (j < nAnc && !ancInitReached[j]) {
            ancInitReached[j] = 1;
            initWork.push_back(j);
          }
        if (ext.activeRetHub || ext.activeNoRetHub)
          markAncInitCallee(hubIdx);
        for (FunctionID v : ext.extraRetCallees)
          markAncInitCallee(v);
        for (FunctionID v : ext.extraNoRetCallees)
          markAncInitCallee(v);
        continue;
      }
      for (const auto &cs : ancCalls[idx]) {
        for (FunctionID callee : liveCallTargets[cs.site])
          markAncInitCallee(callee);
        if (cs.hasHub && hubActive)
          markAncInitCallee(hubIdx);
      }
    }

    struct NodeVecHash {
      std::size_t operator()(const std::vector<NodeID> &v) const {
        std::uint64_t h = 14695981039346656037ULL;
        for (NodeID x : v)
          h = hashMix(h, x);
        return static_cast<std::size_t>(h);
      }
    };
    std::unordered_map<std::vector<NodeID>, NodeID, NodeVecHash> multiInNodes;
    std::unordered_map<std::vector<NodeID>, NodeID, NodeVecHash> multiOutNodes;

    auto isCanonActiveAll = [&](FunctionID ct) -> bool {
      if (ct == hubIdx)
        return ancHubActive || hubIsPort;
      return fnActiveGen[ct] == g;
    };
    const std::size_t totalActiveTargets =
        nAll + (ancHubActive ? 1 : 0);

    for (std::size_t ai = 0; ai < nAnc; ++ai) {
      const FunctionID rep = ancFuncs[ai];
      const std::uint32_t u = univIdOfRep(rep);
      const bool isEntry =
          p.entryFunctions_.count(rep) || (u != 0 && univSccs[u].containsEntry);
      if (isEntry || (scopeRoot && rep == scopeRoot) || !ancInitReached[ai])
        addIndirect(initPort, actEntry[ai]);

      if (u != 0) {
        const auto &scc = univSccs[u];
        const auto &ext = ancUnivExt[ai];
        const NodeID C = actEntry[ai];
        const bool uMod = isTransMod(rep);
        if (uMod && scc.unkStoreJoin && scc.unkStoreJoin != C)
          addIndirect(scc.unkStoreJoin, C);
        if (scc.unkLoadJoin && C != scc.unkLoadJoin)
          addIndirect(C, scc.unkLoadJoin);

        // Wire active external callees + O((k + 1) log M) inactive summaries.
        std::vector<FunctionID> excludedCanon;
        excludedCanon.reserve(ext.activeRetIdxs.size() +
                              ext.activeNoRetIdxs.size() + 1);
        auto wireActiveIdx = [&](std::size_t j, bool isRet) {
          const FunctionID rj = (j < nAnc) ? ancFuncs[j] : coreFuncs[j - nAnc];
          excludedCanon.push_back(rj);
          const NodeID dst = actEntry[j];
          if (dst && dst != C)
            addIndirect(C, dst);
          if (isRet && uMod && isTransMod(rj)) {
            const NodeID src = actExit[j];
            if (src && src != C)
              addIndirect(src, C);
          }
        };
        for (std::size_t j : ext.activeRetIdxs)
          wireActiveIdx(j, true);
        for (std::size_t j : ext.activeNoRetIdxs)
          wireActiveIdx(j, false);
        if (ext.activeRetHub || ext.activeNoRetHub) {
          excludedCanon.push_back(hubIdx);
          if (hubEntry && hubEntry != C)
            addIndirect(C, hubEntry);
          if (ext.activeRetHub && uMod && hubExit && hubExit != C)
            addIndirect(hubExit, C);
        }
        const auto &extSum =
            globalVisible ? scc.globalExtSummary : scc.alwaysExtSummary;
        wireUnivExtInactive(C, uMod ? C : 0, extSum, totalActiveTargets,
                            isCanonActiveAll, excludedCanon);

        auto wireExtraV = [&](FunctionID v, bool isRet) {
          const FunctionID cv = canon(v);
          if (cv == hubIdx) {
            if (ancHubActive || hubIsPort) {
              if (hubEntry && hubEntry != C)
                addIndirect(C, hubEntry);
              if (isRet && uMod && hubExit && hubExit != C)
                addIndirect(hubExit, C);
            } else {
              if (unknownTransRef[hubIdx])
                addIndirect(C, unknownReadEntry[hubIdx]);
              if (isRet && uMod && unknownModSummary[hubIdx] &&
                  unknownModSummary[hubIdx] != C)
                addIndirect(unknownModSummary[hubIdx], C);
            }
            return;
          }
          if (isTransMod(cv) || isTransRef(cv)) {
            const NodeID dst = actEntry[fnActiveIdx[cv]];
            if (dst && dst != C)
              addIndirect(C, dst);
            if (isRet && uMod && isTransMod(cv)) {
              const NodeID src = actExit[fnActiveIdx[cv]];
              if (src && src != C)
                addIndirect(src, C);
            }
          } else {
            if (unknownTransRef[v])
              addIndirect(C, unknownReadEntry[v]);
            if (isRet && uMod && unknownModSummary[v] &&
                unknownModSummary[v] != C)
              addIndirect(unknownModSummary[v], C);
          }
        };
        for (FunctionID v : ext.extraRetCallees)
          wireExtraV(v, true);
        for (FunctionID v : ext.extraNoRetCallees)
          wireExtraV(v, false);

        // Special functions inside `u` (zero own accesses).
        std::vector<FunctionID> specialFuncs = scc.nonExitUnkFuncs;
        auto checkNonExitExt = [&](FunctionID f) {
          const auto &fc = funcCFGs[f];
          for (CallSiteID site : funcCallSites[f]) {
            const auto &cs = p.callSites_[site];
            if (!cs.mayReturn || fc.canReachExit[blockToLocal[cs.block]] ||
                !callSiteCanCarryObject(site))
              continue;
            for (FunctionID v : liveCallTargets[site]) {
              if (fnUnivScc[v] == u)
                continue;
              if (isTransMod(v) || (!isTransRef(v) && unknownModSummary[v])) {
                appendUnique(specialFuncs, f);
                return;
              }
            }
            if (siteCallsUnknownHub[site] && !scc.containsHub) {
              if (isTransMod(hubIdx) ||
                  (!hubActive && unknownModSummary[hubIdx])) {
                appendUnique(specialFuncs, f);
                return;
              }
            }
          }
        };
        for (FunctionID f : scc.nonExitExtRetFuncs)
          checkNonExitExt(f);
        for (FunctionID f : ext.extraNonExitRetFuncs)
          checkNonExitExt(f);

        for (FunctionID sf : specialFuncs) {
          std::vector<LocalEvent> events;
          auto uIt = unknownFnAccesses.find(sf);
          if (uIt != unknownFnAccesses.end()) {
            for (AccessID id : uIt->second) {
              const auto &access = p.accesses_[id];
              LocalEvent ev;
              ev.block = blockToLocal[access.block];
              ev.pos = accessEventPos[id];
              ev.phase = 0;
              ev.isStore = (access.kind == Program::AccessKind::Store);
              ev.isStrong = false;
              ev.node = (ev.isStore && access.sourceIsUnknown)
                            ? p.unknownValue()
                            : access.value;
              events.push_back(ev);
            }
          }
          for (CallSiteID site : funcCallSites[sf]) {
            if (!callSiteCanCarryObject(site))
              continue;
            const auto &cs = p.callSites_[site];
            const LocalBlock callBlock = blockToLocal[cs.block];
            const std::uint32_t pos = callEventPos[site];
            std::vector<NodeID> inT, outS;
            auto handleT = [&](FunctionID v) {
              const FunctionID cv = canon(v);
              if (cv == rep) {
                inT.push_back(C);
                return;
              }
              if (cv == hubIdx) {
                if (ancHubActive || hubIsPort) {
                  if (hubEntry)
                    inT.push_back(hubEntry);
                  if (cs.mayReturn && hubExit)
                    outS.push_back(hubExit);
                } else {
                  if (unknownTransRef[hubIdx])
                    inT.push_back(unknownReadEntry[hubIdx]);
                  if (cs.mayReturn && unknownModSummary[hubIdx])
                    outS.push_back(unknownModSummary[hubIdx]);
                }
                return;
              }
              if (isTransMod(cv) || isTransRef(cv)) {
                inT.push_back(actEntry[fnActiveIdx[cv]]);
                if (cs.mayReturn && isTransMod(cv) && actExit[fnActiveIdx[cv]])
                  outS.push_back(actExit[fnActiveIdx[cv]]);
              } else {
                if (unknownTransRef[v])
                  inT.push_back(unknownReadEntry[v]);
                if (cs.mayReturn && unknownModSummary[v])
                  outS.push_back(unknownModSummary[v]);
              }
            };
            for (FunctionID v : liveCallTargets[site])
              handleT(v);
            if (siteCallsUnknownHub[site] && unknownHubActive)
              handleT(hubIdx);
            if (inT.size() > 1) {
              std::sort(inT.begin(), inT.end());
              inT.erase(std::unique(inT.begin(), inT.end()), inT.end());
            }
            if (inT.size() == 1) {
              events.push_back({callBlock, pos, 0, false, false, inT[0], 0});
            } else if (inT.size() >= 2) {
              auto [it, ins] = multiInNodes.emplace(inT, 0);
              if (ins) {
                const NodeID jn = addJoin();
                it->second = jn;
                for (NodeID dst : inT)
                  addIndirect(jn, dst);
              }
              events.push_back(
                  {callBlock, pos, 0, false, false, it->second, 0});
            }
            if (outS.size() > 1) {
              std::sort(outS.begin(), outS.end());
              outS.erase(std::unique(outS.begin(), outS.end()), outS.end());
            }
            if (outS.size() == 1) {
              events.push_back({callBlock, pos, 1, true, false, outS[0], 0});
            } else if (outS.size() >= 2) {
              auto [it, ins] = multiOutNodes.emplace(outS, 0);
              if (ins) {
                const NodeID jn = addJoin();
                it->second = jn;
                for (NodeID src : outS)
                  addIndirect(src, jn);
              }
              events.push_back(
                  {callBlock, pos, 1, true, false, it->second, 0});
            }
          }
          if (uMod) {
            events.push_back({funcCFGs[sf].exitLocal,
                              std::numeric_limits<std::uint32_t>::max(), 2,
                              false, false, C, 0});
          }
          computeLocalFlowForFunction(sf, C, events, /*hasAnyStrong=*/false);
        }
        continue;
      }

      const FunctionID f = rep;
      const auto &summaries = ancCalls[ai];
      const auto &unkMods = ancUnkMods[ai];
      const auto &unkReads = ancUnkReads[ai];

      // Theorem 3A fast path for `ANC` functions whose entry == exit == C.
      if (actEntry[ai] != 0 && actEntry[ai] == actExit[ai]) {
        const auto &fc = funcCFGs[f];
        bool allStoresReachExit = !fnHasNonExitUnkStore[f];
        if (allStoresReachExit) {
          for (const auto &um : unkMods) {
            if (!fc.canReachExit[blockToLocal[p.callSites_[um.site].block]]) {
              allStoresReachExit = false;
              break;
            }
          }
        }
        if (allStoresReachExit) {
          for (const auto &cs : summaries) {
            if (cs.hasOut &&
                !fc.canReachExit[blockToLocal[p.callSites_[cs.site].block]]) {
              allStoresReachExit = false;
              break;
            }
          }
        }
        if (allStoresReachExit) {
          const NodeID C = actEntry[ai];
          if (fnLocalExitUnkStoreNode[f] && fnLocalExitUnkStoreNode[f] != C)
            addIndirect(fnLocalExitUnkStoreNode[f], C);
          if (fnLocalUnkLoadNode[f] && C != fnLocalUnkLoadNode[f])
            addIndirect(C, fnLocalUnkLoadNode[f]);
          for (const auto &um : unkMods)
            if (um.summaryNode != C)
              addIndirect(um.summaryNode, C);
          for (const auto &ur : unkReads)
            if (C != ur.summaryNode)
              addIndirect(C, ur.summaryNode);
          for (const auto &cs : summaries) {
            if (cs.hasIn) {
              for (FunctionID callee : liveCallTargets[cs.site]) {
                NodeID dst = 0;
                if (isTransMod(callee) || isTransRef(callee))
                  dst = actEntry[fnActiveIdx[canon(callee)]];
                else if (unknownTransRef[callee])
                  dst = unknownReadEntry[callee];
                if (dst && dst != C)
                  addIndirect(C, dst);
              }
              if (cs.hasHub) {
                NodeID dst = hubActive ? hubEntry
                                       : (unknownTransRef[hubIdx]
                                              ? unknownReadEntry[hubIdx]
                                              : 0);
                if (dst && dst != C)
                  addIndirect(C, dst);
              }
            }
            if (cs.hasOut) {
              for (FunctionID callee : liveCallTargets[cs.site]) {
                if (isTransMod(callee)) {
                  const NodeID src = actExit[fnActiveIdx[canon(callee)]];
                  if (src && src != C)
                    addIndirect(src, C);
                }
              }
              if (cs.hasHub && hubExit && hubExit != C)
                addIndirect(hubExit, C);
            }
          }
          continue;
        }
      }

      std::vector<LocalEvent> events;
      auto uIt = unknownFnAccesses.find(f);
      if (uIt != unknownFnAccesses.end()) {
        for (AccessID id : uIt->second) {
          const auto &access = p.accesses_[id];
          LocalEvent ev;
          ev.block = blockToLocal[access.block];
          ev.pos = accessEventPos[id];
          ev.phase = 0;
          ev.isStore = (access.kind == Program::AccessKind::Store);
          ev.isStrong = false;
          ev.node = (ev.isStore && access.sourceIsUnknown) ? p.unknownValue()
                                                           : access.value;
          events.push_back(ev);
        }
      }
      for (const auto &um : unkMods) {
        events.push_back({blockToLocal[p.callSites_[um.site].block],
                          callEventPos[um.site], 1, true, false, um.summaryNode,
                          0});
      }
      for (const auto &ur : unkReads) {
        events.push_back({blockToLocal[p.callSites_[ur.site].block],
                          callEventPos[ur.site], 0, false, false,
                          ur.summaryNode, 0});
      }
      for (const auto &cs : summaries) {
        const LocalBlock callBlock = blockToLocal[p.callSites_[cs.site].block];
        const std::uint32_t pos = callEventPos[cs.site];
        if (cs.hasIn) {
          std::vector<NodeID> inTargets;
          for (FunctionID callee : liveCallTargets[cs.site]) {
            if (isTransMod(callee) || isTransRef(callee))
              inTargets.push_back(actEntry[fnActiveIdx[canon(callee)]]);
            else if (unknownTransRef[callee])
              inTargets.push_back(unknownReadEntry[callee]);
          }
          if (cs.hasHub) {
            if (hubActive)
              inTargets.push_back(hubEntry);
            else if (unknownTransRef[hubIdx])
              inTargets.push_back(unknownReadEntry[hubIdx]);
          }
          if (inTargets.size() > 1) {
            std::sort(inTargets.begin(), inTargets.end());
            inTargets.erase(std::unique(inTargets.begin(), inTargets.end()),
                            inTargets.end());
          }
          if (inTargets.size() == 1) {
            events.push_back(
                {callBlock, pos, 0, false, false, inTargets[0], 0});
          } else if (inTargets.size() >= 2) {
            auto [it, ins] = multiInNodes.emplace(inTargets, 0);
            if (ins) {
              const NodeID sharedIn = addJoin();
              it->second = sharedIn;
              for (NodeID dst : inTargets)
                addIndirect(sharedIn, dst);
            }
            events.push_back({callBlock, pos, 0, false, false, it->second, 0});
          }
        }
        if (cs.hasOut) {
          std::vector<NodeID> outSources;
          for (FunctionID callee : liveCallTargets[cs.site]) {
            if (isTransMod(callee)) {
              const NodeID cx = actExit[fnActiveIdx[canon(callee)]];
              if (cx)
                outSources.push_back(cx);
            }
          }
          if (cs.hasHub && hubExit)
            outSources.push_back(hubExit);
          if (outSources.size() > 1) {
            std::sort(outSources.begin(), outSources.end());
            outSources.erase(std::unique(outSources.begin(), outSources.end()),
                             outSources.end());
          }
          if (outSources.size() == 1) {
            events.push_back(
                {callBlock, pos, 1, true, false, outSources[0], 0});
          } else if (outSources.size() >= 2) {
            auto [it, ins] = multiOutNodes.emplace(outSources, 0);
            if (ins) {
              const NodeID sharedOut = addJoin();
              it->second = sharedOut;
              for (NodeID src : outSources)
                addIndirect(src, sharedOut);
            }
            events.push_back({callBlock, pos, 1, true, false, it->second, 0});
          }
        }
      }
      if (actExit[ai]) {
        events.push_back({funcCFGs[f].exitLocal,
                          std::numeric_limits<std::uint32_t>::max(), 2, false,
                          false, actExit[ai], 0});
      }
      computeLocalFlowForFunction(f, actEntry[ai], events,
                                  /*hasAnyStrong=*/false);
    }

    if (globalVisible && !unknownDirectRefFuncs.empty()) {
      for (FunctionID entry : p.entryFunctions_) {
        if (funcReachable[entry] && fnActiveGen[canon(entry)] != g &&
            unknownTransRef[entry]) {
          addIndirect(initPort, unknownReadEntry[entry]);
        }
      }
    }

    localCap.active = false;

    // Extract exact transfer channels from `localCap.adj` into the new
    // `WeakAncestorContext`, and emit SCC-condensed `U_src -> U_snk` once into
    // the real VFG.
    const std::uint32_t ctxId = static_cast<std::uint32_t>(weakAncCtxs.size());
    weakAncCtxs.emplace_back();
    weakAncCtxDirtyStamp.push_back(0);
    auto &ctx = weakAncCtxs.back();
    ctx.id = ctxId;
    ctx.coreFuncs = coreFuncs;
    ctx.coreIsMod.resize(nCore, 0);
    ctx.coreIsRef.resize(nCore, 0);
    for (std::size_t ci = 0; ci < nCore; ++ci) {
      ctx.coreIsMod[ci] = isTransMod(coreFuncs[ci]) ? 1 : 0;
      ctx.coreIsRef[ci] = isTransRef(coreFuncs[ci]) ? 1 : 0;
    }
    ctx.initReachesCoreEntry.assign(nCore, 0);
    ctx.coreNeedExitFromAnc.assign(nCore, 0);
    ctx.unkSrcToCoreEntry.assign(nCore, 0);
    ctx.coreExitToUnkSnk.assign(nCore, 0);

    const std::size_t V = localCap.adj.size();
    std::vector<std::vector<std::uint32_t>> revAdj(V);
    for (std::uint32_t u = 0; u < V; ++u) {
      for (std::uint32_t v : localCap.adj[u])
        revAdj[v].push_back(u);
    }

    std::vector<std::uint32_t> visMark(V, 0);
    std::uint32_t visGen = 0;
    std::vector<std::uint32_t> bfsWork;

    auto makeJoin = [&](std::vector<NodeID> &nodes, bool isSource) -> NodeID {
      if (nodes.empty())
        return 0;
      if (nodes.size() > 1) {
        std::sort(nodes.begin(), nodes.end());
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
      }
      if (nodes.size() == 1)
        return nodes[0];
      auto &cache = isSource ? weakAncSrcJoinCache : weakAncSnkJoinCache;
      auto [it, ins] = cache.emplace(nodes, 0);
      if (ins) {
        const NodeID jn = addJoin();
        it->second = jn;
        if (isSource) {
          for (NodeID s : nodes)
            addIndirect(s, jn);
        } else {
          for (NodeID t : nodes)
            addIndirect(jn, t);
        }
      }
      return it->second;
    };

    // (a) Forward BFS from `initPort`.
    {
      const std::uint32_t mg = ++visGen;
      const std::uint32_t start = initPort & ~kLocalNodeBase;
      visMark[start] = mg;
      bfsWork.clear();
      bfsWork.push_back(start);
      std::vector<NodeID> snks;
      while (!bfsWork.empty()) {
        const std::uint32_t u = bfsWork.back();
        bfsWork.pop_back();
        if (localCap.snkRealNode[u])
          snks.push_back(localCap.snkRealNode[u]);
        for (std::uint32_t v : localCap.adj[u]) {
          if (visMark[v] != mg) {
            visMark[v] = mg;
            bfsWork.push_back(v);
          }
        }
      }
      for (std::size_t ci = 0; ci < nCore; ++ci) {
        if (visMark[actEntry[nAnc + ci] & ~kLocalNodeBase] == mg)
          ctx.initReachesCoreEntry[ci] = 1;
      }
      ctx.initToUnkSnk = makeJoin(snks, /*isSource=*/false);
    }

    // (b) Forward BFS from each `coreExitPort[ci]`.
    for (std::size_t ci = 0; ci < nCore; ++ci) {
      if (!actExit[nAnc + ci])
        continue;
      const std::uint32_t mg = ++visGen;
      const std::uint32_t start = actExit[nAnc + ci] & ~kLocalNodeBase;
      visMark[start] = mg;
      bfsWork.clear();
      bfsWork.push_back(start);
      std::vector<NodeID> snks;
      while (!bfsWork.empty()) {
        const std::uint32_t u = bfsWork.back();
        bfsWork.pop_back();
        if (localCap.snkRealNode[u])
          snks.push_back(localCap.snkRealNode[u]);
        for (std::uint32_t v : localCap.adj[u]) {
          if (visMark[v] != mg) {
            visMark[v] = mg;
            bfsWork.push_back(v);
          }
        }
      }
      bool reachesAny = false;
      for (std::size_t cj = 0; cj < nCore; ++cj) {
        if (visMark[actEntry[nAnc + cj] & ~kLocalNodeBase] == mg) {
          ctx.coreExitToEntry.emplace_back(static_cast<std::uint32_t>(ci),
                                           static_cast<std::uint32_t>(cj));
          reachesAny = true;
        }
      }
      ctx.coreExitToUnkSnk[ci] = makeJoin(snks, /*isSource=*/false);
      if (reachesAny || ctx.coreExitToUnkSnk[ci] != 0)
        ctx.coreNeedExitFromAnc[ci] = 1;
    }

    // (c) Backward BFS from each `coreEntryPort[ci]`.
    for (std::size_t ci = 0; ci < nCore; ++ci) {
      const std::uint32_t mg = ++visGen;
      const std::uint32_t start = actEntry[nAnc + ci] & ~kLocalNodeBase;
      visMark[start] = mg;
      bfsWork.clear();
      bfsWork.push_back(start);
      std::vector<NodeID> srcs;
      while (!bfsWork.empty()) {
        const std::uint32_t u = bfsWork.back();
        bfsWork.pop_back();
        if (localCap.srcRealNode[u])
          srcs.push_back(localCap.srcRealNode[u]);
        for (std::uint32_t pred : revAdj[u]) {
          if (visMark[pred] != mg) {
            visMark[pred] = mg;
            bfsWork.push_back(pred);
          }
        }
      }
      ctx.unkSrcToCoreEntry[ci] = makeJoin(srcs, /*isSource=*/true);
    }

    // (d) Exact `U_src -> U_snk` emission ONCE into the real VFG (SCC-condensed
    // and pass-through elided).
    {
      std::vector<std::uint8_t> fwdSrc(V, 0), bwdSnk(V, 0);
      bfsWork.clear();
      for (std::uint32_t u = 0; u < V; ++u) {
        if (localCap.srcRealNode[u]) {
          fwdSrc[u] = 1;
          bfsWork.push_back(u);
        }
      }
      while (!bfsWork.empty()) {
        const std::uint32_t u = bfsWork.back();
        bfsWork.pop_back();
        for (std::uint32_t v : localCap.adj[u]) {
          if (!fwdSrc[v]) {
            fwdSrc[v] = 1;
            bfsWork.push_back(v);
          }
        }
      }
      bfsWork.clear();
      for (std::uint32_t u = 0; u < V; ++u) {
        if (localCap.snkRealNode[u] && fwdSrc[u]) {
          bwdSnk[u] = 1;
          bfsWork.push_back(u);
        }
      }
      while (!bfsWork.empty()) {
        const std::uint32_t u = bfsWork.back();
        bfsWork.pop_back();
        for (std::uint32_t pred : revAdj[u]) {
          if (fwdSrc[pred] && !bwdSnk[pred]) {
            bwdSnk[pred] = 1;
            bfsWork.push_back(pred);
          }
        }
      }
      std::vector<std::uint32_t> liveVerts;
      std::vector<std::uint32_t> vToLive(
          V, std::numeric_limits<std::uint32_t>::max());
      for (std::uint32_t u = 0; u < V; ++u) {
        if (fwdSrc[u] && bwdSnk[u]) {
          vToLive[u] = static_cast<std::uint32_t>(liveVerts.size());
          liveVerts.push_back(u);
        }
      }
      if (!liveVerts.empty()) {
        const std::size_t nL = liveVerts.size();
        std::vector<std::vector<std::uint32_t>> lSucc(nL), lPred(nL);
        for (std::size_t i = 0; i < nL; ++i) {
          for (std::uint32_t v : localCap.adj[liveVerts[i]]) {
            const std::uint32_t li = vToLive[v];
            if (li != std::numeric_limits<std::uint32_t>::max()) {
              lSucc[i].push_back(li);
              lPred[li].push_back(static_cast<std::uint32_t>(i));
            }
          }
        }
        auto lScc = computeSCCs(lSucc, lPred);
        const auto &lComp = lScc.first;
        const std::size_t lComps = lScc.second;
        std::vector<std::vector<std::uint32_t>> lMembers(lComps);
        for (std::size_t i = 0; i < nL; ++i)
          lMembers[lComp[i]].push_back(static_cast<std::uint32_t>(i));
        std::vector<NodeID> compReal(lComps, 0);
        for (std::size_t c = 0; c < lComps; ++c) {
          if (lMembers[c].size() == 1) {
            const std::uint32_t origV = liveVerts[lMembers[c][0]];
            if (localCap.srcRealNode[origV]) {
              compReal[c] = localCap.srcRealNode[origV];
              continue;
            }
            if (localCap.snkRealNode[origV]) {
              compReal[c] = localCap.snkRealNode[origV];
              continue;
            }
            // Pass-through elision for single-predecessor internal node.
            NodeID uniqPred = 0;
            bool multi = false;
            for (std::uint32_t pi : lPred[lMembers[c][0]]) {
              const std::size_t pc = lComp[pi];
              if (pc == c)
                continue;
              const NodeID pn = compReal[pc];
              if (!pn)
                continue;
              if (!uniqPred)
                uniqPred = pn;
              else if (uniqPred != pn) {
                multi = true;
                break;
              }
            }
            if (!multi && uniqPred) {
              compReal[c] = uniqPred;
              continue;
            }
          }
          compReal[c] = addJoin();
        }
        for (std::size_t i = 0; i < nL; ++i) {
          const NodeID srcN = compReal[lComp[i]];
          for (std::uint32_t j : lSucc[i]) {
            const NodeID dstN = compReal[lComp[j]];
            if (srcN && dstN && srcN != dstN)
              addIndirect(srcN, dstN);
          }
        }
      }
    }

    localCap.reset();

    // Hash-cons exact transfer summary channels so rebuilt contexts producing
    // identical `(coreFuncs, init/unk/exit ports)` share ONE `summaryId` and
    // hit `LastEmitState` with zero redundant edge emissions.
    {
      WeakAncSummaryKey sKey;
      sKey.coreFuncs = ctx.coreFuncs;
      sKey.initReachesCoreEntry = ctx.initReachesCoreEntry;
      sKey.coreNeedExitFromAnc = ctx.coreNeedExitFromAnc;
      sKey.unkSrcToCoreEntry = ctx.unkSrcToCoreEntry;
      sKey.coreExitToUnkSnk = ctx.coreExitToUnkSnk;
      sKey.coreExitToEntry = ctx.coreExitToEntry;
      sKey.initToUnkSnk = ctx.initToUnkSnk;
      auto [sit, sins] = weakAncSummaryIntern.emplace(std::move(sKey), 0);
      if (sins)
        sit->second = nextWeakAncSummaryId++;
      ctx.summaryId = sit->second;
    }
    ctx.stale = false;
    ctx.builtUnivBuild = useUniv ? univSccBuildId : 0ULL;
    ctx.builtEntryUnkRefVer = entryUnkRefVersion;
    ctx.builtHubGlobalVer = hubGlobalVersion;

    // Register `ctxId` on every super-vertex in `ANC` for exact dirty tracking,
    // and watch `CORE` so pre-prop cache invalidates if `CORE` gains callers.
    // `ctxId` is freshly allocated (> all previous IDs), so O(1) back-check
    // suffices instead of linear scanning.
    auto pushFreshCtxId = [&](std::vector<std::uint32_t> &vec) {
      if (vec.empty() || vec.back() != ctxId)
        vec.push_back(ctxId);
    };
    for (FunctionID a : ancFuncs) {
      if (const std::uint32_t u = univIdOfRep(a))
        pushFreshCtxId(univSccs[u].weakAncCtxIds);
      else
        pushFreshCtxId(funcWeakAncCtxs[a]);
    }
    if (ancHubActive)
      pushFreshCtxId(funcWeakAncCtxs[hubIdx]);
    for (FunctionID c : coreFuncs) {
      if (c == hubIdx) {
        pushFreshCtxId(funcWeakAncCoreWatch[hubIdx]);
      } else if (const std::uint32_t u = univIdOfRep(c)) {
        pushFreshCtxId(univSccs[u].weakAncCoreWatchIds);
      } else {
        pushFreshCtxId(funcWeakAncCoreWatch[c]);
      }
    }

    return ctxId;
  }

  // Algorithm 2: PreprocessGlobalFlow + per-function ComputeLocalFlow.
  // Uses generation-stamped function markers so work is O(|ActiveFuncs(object)|)
  // rather than O(|Functions|), summarizes untouched unknown-modifying callees
  // with shared per-function summary nodes, and scopes interprocedural
  // call-in/call-out instrumentation to callsites that can carry `object`.
  void computeIndirect(ObjectID object, const Signature &sig) {
    ++stats.indirectFlowComputations;
    stats.projectedEvents += sig.accesses.size();
    const bool isUnknownObj = (object == p.unknownObject());
    if (!isUnknownObj)
      ensureUnknownSummary();
    if (sig.accesses.empty() && (isUnknownObj || unknownDirectRefFuncs.empty()))
      return;

    // Constant globals with definitive initializers (e.g., C++ vtables, string
    // literals, const tables) reside in immutable read-only memory and are
    // wired incrementally via `readOnlySourceJoinCache` / `allReadOnlyInitJoin`.
    if (!isUnknownObj && p.objects_[object].readOnly)
      return;

    const std::uint64_t g = ++indirectGen;
    const bool objCanStrong = canStrongUpdate(object);

    // Fast O(|sig.accesses|) pre-pass using generation-stamped arrays to
    // collect directly-modifying and directly-referencing functions.
    std::vector<FunctionID> directModFuncs;
    std::vector<FunctionID> directRefFuncs;
    std::vector<FunctionID> directFuncs;
    bool allAccessesReachable = true;
    for (AccessID id : sig.accesses) {
      const auto &access = p.accesses_[id];
      if (!blockReachable[access.block]) {
        allAccessesReachable = false;
        continue;
      }
      const FunctionID fn = p.blocks_[access.block].function;
      const bool seenFn =
          (fnDirectModGen[fn] == g || fnDirectRefGen[fn] == g);
      if (!seenFn) {
        fnDirectIdx[fn] = static_cast<std::uint32_t>(directFuncs.size());
        directFuncs.push_back(fn);
      }
      if (access.kind == Program::AccessKind::Store) {
        if (fnDirectModGen[fn] != g) {
          fnDirectModGen[fn] = g;
          directModFuncs.push_back(fn);
        }
      } else {
        if (fnDirectRefGen[fn] != g) {
          fnDirectRefGen[fn] = g;
          directRefFuncs.push_back(fn);
        }
      }
    }

    // Functions with `load unknown` that have no own accesses to `object` are
    // fed via the shared `unknownReadEntry[callee]` channel rather than pulling
    // the entire call graph into `activeFuncs`.
    if (directRefFuncs.empty()) {
      if (isUnknownObj || unknownDirectRefFuncs.empty())
        return;
      if (directModFuncs.empty()) {
        const bool isGlobVis =
            p.objects_[object].isGlobal || objExternallyEscaped[object];
        if (isGlobVis &&
            p.initializerKinds_[object] != Program::InitializerKind::Empty) {
          bool wired = false;
          for (FunctionID entry : p.entryFunctions_) {
            if (funcReachable[entry] && unknownTransRef[entry]) {
              addIndirect(p.objects_[object].initialStore,
                          unknownReadEntry[entry]);
              wired = true;
            }
          }
          if (wired)
            emittedFlow[object] = true;
        }
        return;
      }
    }

    // Partition own accesses by function in flat dense vectors, with a
    // zero-copy fast path when all reachable accesses live in a single function.
    static const NodeSet kEmptyNodeSet;
    const bool singleDirectFnFast =
        (directFuncs.size() == 1 && allAccessesReachable);
    std::vector<NodeSet> directAccesses;
    std::vector<NodeSet> directStrong;
    if (!singleDirectFnFast) {
      directAccesses.resize(directFuncs.size());
      if (objCanStrong && !sig.strong.empty())
        directStrong.resize(directFuncs.size());
      for (AccessID id : sig.accesses) {
        const auto &access = p.accesses_[id];
        if (!blockReachable[access.block])
          continue;
        const std::uint32_t di = fnDirectIdx[p.blocks_[access.block].function];
        directAccesses[di].insertBit(id);
        if (!directStrong.empty() && sig.strong.count(id))
          directStrong[di].insertBit(id);
      }
    }
    auto ownAccessesOf = [&](FunctionID fn) -> const NodeSet & {
      if (fnDirectModGen[fn] == g || fnDirectRefGen[fn] == g)
        return singleDirectFnFast ? sig.accesses
                                  : directAccesses[fnDirectIdx[fn]];
      return kEmptyNodeSet;
    };
    auto ownStrongOf = [&](FunctionID fn) -> const NodeSet & {
      if (objCanStrong && !sig.strong.empty() &&
          (fnDirectModGen[fn] == g || fnDirectRefGen[fn] == g))
        return singleDirectFnFast ? sig.strong : directStrong[fnDirectIdx[fn]];
      return kEmptyNodeSet;
    };

    // Compute transitive escape carrier set for sound callsite scoping.
    bool globalVisible = p.objects_[object].isGlobal ||
                         objExternallyEscaped[object] || isUnknownObj;
    NodeSet carrierObjects;
    carrierObjects.insertBit(object);
    if (!globalVisible) {
      std::vector<ObjectID> escWork{object};
      while (!escWork.empty() && !globalVisible) {
        const ObjectID cur = escWork.back();
        escWork.pop_back();
        for (ObjectID dep : initialEscapeObjects[cur]) {
          if (!p.objects_[dep].memory)
            continue;
          if (p.objects_[dep].isGlobal || objExternallyEscaped[dep] ||
              dep == p.unknownObject()) {
            globalVisible = true;
            break;
          }
          if (carrierObjects.insertBit(dep))
            escWork.push_back(dep);
        }
        if (globalVisible)
          break;
        for (NodeID ptr : getEscapePointers(cur)) {
          const auto &targets = ptsOf(ptr);
          if (targets.empty() || targets.count(p.unknownObject())) {
            globalVisible = true;
            break;
          }
          for (ObjectID target : targets) {
            if (!p.objects_[target].memory)
              continue;
            if (p.objects_[target].isGlobal || objExternallyEscaped[target]) {
              globalVisible = true;
              break;
            }
            if (carrierObjects.insertBit(target))
              escWork.push_back(target);
          }
          if (globalVisible)
            break;
        }
      }
    }

    // Inverted O(1) callsite carry check: pre-mark only the callsites in
    // `objCarrySites[c]` and `callerSites[owner]` (typically 1-5 callsites!)
    // so `callSiteCanCarryObject` is a single branchless array check with zero
    // `ptsOf` lookups.
    std::vector<CallSiteID> carrierMarkedSites;
    if (!globalVisible) {
      for (ObjectID c : carrierObjects) {
        if (const FunctionID owner = p.objects_[c].ownerFunction) {
          for (CallSiteID s : callerSites[owner]) {
            if (siteCarryGen[s] != g) {
              siteCarryGen[s] = g;
              if (!siteAlwaysCarries(s) &&
                  blockReachable[p.callSites_[s].block])
                carrierMarkedSites.push_back(s);
            }
          }
        }
        for (CallSiteID s : objCarrySites[c]) {
          if (siteCarryGen[s] != g) {
            siteCarryGen[s] = g;
            if (!siteAlwaysCarries(s) && blockReachable[p.callSites_[s].block])
              carrierMarkedSites.push_back(s);
          }
        }
      }
    }

    auto callSiteCanCarryObject = [&](CallSiteID site) -> bool {
      return globalVisible || siteAlwaysCarries(site) ||
             siteCarryGen[site] == g;
    };

    if (!isUnknownObj && !objCanStrong)
      ensureUnivCallGraphSCCs();
    const bool useUniv =
        (!isUnknownObj && !objCanStrong && univSccs.size() > 1);
    const std::uint64_t curUnivBuild = useUniv ? univSccBuildId : 0;
    const FunctionID hubIdx = static_cast<FunctionID>(p.functions_.size());
    auto canon = [&](FunctionID f) -> FunctionID {
      if (useUniv) {
        if (const std::uint32_t u = fnUnivScc[f])
          return univSccs[u].repFunc;
      }
      return f;
    };
    auto univIdOfRep = [&](FunctionID rep) -> std::uint32_t {
      return useUniv ? fnUnivScc[rep] : 0U;
    };

    // Determine whether a local stack object stays strictly within its owner's
    // call-graph subgraph. Fast path O(1) when all accesses are inside owner.
    // Note: `!funcRecursive[owner]` guarantees `fnUnivScc[owner] == 0`.
    FunctionID scopeRoot = 0;
    if (!globalVisible && effectiveOwner[object]) {
      const FunctionID owner = effectiveOwner[object];
      if (funcReachable[owner] && !funcRecursive[owner]) {
        bool onlyOwner = (directFuncs.size() == 1 && directFuncs[0] == owner);
        if (onlyOwner) {
          scopeRoot = owner;
        } else {
          std::vector<FunctionID> downWork{owner};
          fnSubgraphGen[owner] = g;
          while (!downWork.empty()) {
            const FunctionID f = downWork.back();
            downWork.pop_back();
            for (CallSiteID site : funcCallSites[f]) {
              for (FunctionID callee : liveCallTargets[site]) {
                if (fnSubgraphGen[callee] != g) {
                  fnSubgraphGen[callee] = g;
                  downWork.push_back(callee);
                }
              }
              if (siteCallsUnknownHub[site] && fnSubgraphGen[hubIdx] != g) {
                fnSubgraphGen[hubIdx] = g;
                for (FunctionID callee : addressTakenFuncs) {
                  if (funcReachable[callee] && fnSubgraphGen[callee] != g) {
                    fnSubgraphGen[callee] = g;
                    downWork.push_back(callee);
                  }
                }
              }
            }
          }
          bool allInside = true;
          for (FunctionID f : directFuncs) {
            if (fnSubgraphGen[f] != g) {
              allInside = false;
              break;
            }
          }
          if (allInside)
            scopeRoot = owner;
        }
      }
    }

    const bool alreadyEmitted = emittedFlow[object];
    const bool useGlobalWeakCache =
        !isUnknownObj && !objCanStrong &&
        (globalVisible || (carrierMarkedSites.empty() && scopeRoot == 0));

    if (useGlobalWeakCache) {
      GlobalWeakFlowKey cacheKey{callGraphVersion,
                                 globalVisible ? 0ULL : alwaysCarryVersion,
                                 unknownAccessVersion, curUnivBuild,
                                 globalVisible, sig.accesses};
      auto cacheIt = globalWeakFlowCache.find(cacheKey);
      if (cacheIt != globalWeakFlowCache.end()) {
        ++stats.indirectFlowCacheHits;
        const auto &cached = cacheIt->second;
        for (FunctionID f : cached.activeFuncs) {
          if (useUniv && fnUnivScc[f])
            univSccs[fnUnivScc[f]].accessedByObjects.insertBit(object);
          else
            funcAccessedByObjects[f].insertBit(object);
        }
        attachWeakAncCtx(object, cached.weakAncCtxId);
        if (cached.rootInitNode &&
            p.initializerKinds_[object] != Program::InitializerKind::Empty) {
          addIndirect(p.objects_[object].initialStore, cached.rootInitNode);
        }
        lastEmitState[object].valid = false;
        emittedFlow[object] = true;
        return;
      }
    }

    // Group direct-access functions and non-alwaysCarry carrier callsites by
    // UnivWeakSCC (only populated when `useUniv == true`).
    std::unordered_map<std::uint32_t, std::vector<FunctionID>> univOwnFuncs;
    std::unordered_map<std::uint32_t, std::vector<CallSiteID>> univCarrierSites;
    if (useUniv) {
      for (FunctionID f : directFuncs) {
        if (const std::uint32_t u = fnUnivScc[f])
          univOwnFuncs[u].push_back(f);
      }
      if (!globalVisible) {
        for (CallSiteID s : carrierMarkedSites) {
          const FunctionID caller = p.blocks_[p.callSites_[s].block].function;
          if (const std::uint32_t u = fnUnivScc[caller])
            univCarrierSites[u].push_back(s);
        }
      }
    }

    // Backward call-graph propagation for TransMod and TransRef over exact
    // super-vertices (`rep = canon(f)`): any UnivWeakSCC `u` collapses its
    // entire internal call subgraph into 1 super-vertex `univSccs[u].repFunc`.
    std::vector<FunctionID> activeFuncs;
    auto markActive = [&](FunctionID rep) {
      if (fnActiveGen[rep] != g) {
        fnActiveGen[rep] = g;
        fnActiveIdx[rep] = static_cast<std::uint32_t>(activeFuncs.size());
        activeFuncs.push_back(rep);
      }
    };
    auto propagateRefUp = [&](const std::vector<FunctionID> &seeds) {
      std::vector<FunctionID> work;
      for (FunctionID f : seeds) {
        const FunctionID rep = canon(f);
        if (fnTransRefGen[rep] != g) {
          fnTransRefGen[rep] = g;
          markActive(rep);
          work.push_back(rep);
        }
      }
      while (!work.empty()) {
        const FunctionID rep = work.back();
        work.pop_back();
        if (scopeRoot && rep == scopeRoot)
          continue;
        auto visitRefSite = [&](CallSiteID site) {
          const BlockID blk = p.callSites_[site].block;
          if (!blockReachable[blk] || !callSiteCanCarryObject(site))
            return;
          const FunctionID caller = p.blocks_[blk].function;
          if (!funcReachable[caller])
            return;
          const FunctionID cRep = canon(caller);
          if (fnTransRefGen[cRep] != g) {
            fnTransRefGen[cRep] = g;
            markActive(cRep);
            work.push_back(cRep);
          }
        };
        if (const std::uint32_t u = univIdOfRep(rep)) {
          for (CallSiteID site : univSccs[u].extCallerSites)
            visitRefSite(site);
        } else {
          for (CallSiteID site : callerSites[rep])
            visitRefSite(site);
          if (unknownHubActive && isAddressTakenFunc[rep]) {
            const FunctionID hubRep = canon(hubIdx);
            if (fnTransRefGen[hubRep] != g) {
              fnTransRefGen[hubRep] = g;
              if (hubRep != hubIdx) {
                markActive(hubRep);
                work.push_back(hubRep);
              } else {
                for (CallSiteID site : unknownHubCallerSites)
                  visitRefSite(site);
              }
            }
          }
        }
      }
    };
    auto propagateModUp = [&](const std::vector<FunctionID> &seeds) {
      std::vector<FunctionID> work;
      for (FunctionID f : seeds) {
        const FunctionID rep = canon(f);
        markActive(rep);
        if ((univIdOfRep(rep) != 0 || funcCFGs[rep].exitLocal != InvalidLocal) &&
            fnTransModGen[rep] != g) {
          fnTransModGen[rep] = g;
          work.push_back(rep);
        }
      }
      while (!work.empty()) {
        const FunctionID rep = work.back();
        work.pop_back();
        if (scopeRoot && rep == scopeRoot)
          continue;
        auto visitCallerSite = [&](CallSiteID site) {
          const auto &cs = p.callSites_[site];
          const BlockID blk = cs.block;
          if (!blockReachable[blk] || !cs.mayReturn ||
              !callSiteCanCarryObject(site))
            return;
          const FunctionID caller = p.blocks_[blk].function;
          if (!funcReachable[caller])
            return;
          const FunctionID cRep = canon(caller);
          markActive(cRep);
          const auto &callerCfg = funcCFGs[caller];
          if (callerCfg.exitLocal != InvalidLocal &&
              callerCfg.canReachExit[blockToLocal[blk]] &&
              fnTransModGen[cRep] != g) {
            fnTransModGen[cRep] = g;
            work.push_back(cRep);
          }
        };
        if (const std::uint32_t u = univIdOfRep(rep)) {
          for (CallSiteID site : univSccs[u].extCallerSites)
            visitCallerSite(site);
        } else {
          for (CallSiteID site : callerSites[rep])
            visitCallerSite(site);
          if (unknownHubActive && isAddressTakenFunc[rep]) {
            const FunctionID hubRep = canon(hubIdx);
            if (fnTransModGen[hubRep] != g) {
              fnTransModGen[hubRep] = g;
              if (hubRep != hubIdx) {
                markActive(hubRep);
                work.push_back(hubRep);
              } else {
                for (CallSiteID site : unknownHubCallerSites)
                  visitCallerSite(site);
              }
            }
          }
        }
      }
    };

    // Only functions with an exit-reaching own store seed `fnTransModGen`.
    std::vector<FunctionID> exitDirectModFuncs;
    for (FunctionID f : directModFuncs) {
      const auto &fc = funcCFGs[f];
      if (fc.exitLocal == InvalidLocal)
        continue;
      bool reachesExit = false;
      for (AccessID id : ownAccessesOf(f)) {
        const auto &acc = p.accesses_[id];
        if (acc.kind == Program::AccessKind::Store &&
            fc.canReachExit[blockToLocal[acc.block]]) {
          reachesExit = true;
          break;
        }
      }
      if (reachesExit)
        exitDirectModFuncs.push_back(f);
    }

    // Theorem Y Level-1 pre-propagation lookup for weak objects with
    // object-independent callsite carry semantics (`globalVisible` or
    // `carrierMarkedSites.empty()`). Skips `propagateModUp`/`propagateRefUp`
    // and all ancestor functions in O(1)!
    std::uint32_t activeWeakAncCtxId = 0;
    const bool preAncEligible =
        !isUnknownObj && !objCanStrong &&
        (globalVisible || carrierMarkedSites.empty()) &&
        !directFuncs.empty() && directFuncs.size() <= 8;
    WeakAncPreKey preAncKey;
    if (preAncEligible) {
      preAncKey.univBuild = curUnivBuild;
      preAncKey.isGlobal = globalVisible;
      preAncKey.scopeRoot = scopeRoot;
      std::vector<std::pair<FunctionID, std::uint8_t>> dPorts;
      auto mergeDPort = [&](FunctionID rep, std::uint8_t bits) {
        for (auto &dp : dPorts) {
          if (dp.first == rep) {
            dp.second |= bits;
            return;
          }
        }
        dPorts.emplace_back(rep, bits);
      };
      for (FunctionID f : directFuncs)
        mergeDPort(canon(f), 0);
      for (FunctionID f : exitDirectModFuncs)
        mergeDPort(canon(f), 2);
      for (FunctionID f : directRefFuncs)
        mergeDPort(canon(f), 1);
      preAncKey.directPorts.reserve(dPorts.size());
      for (const auto &dp : dPorts)
        preAncKey.directPorts.push_back(
            (static_cast<std::uint64_t>(dp.first) << 2) | dp.second);
      std::sort(preAncKey.directPorts.begin(), preAncKey.directPorts.end());
      auto preIt = weakAncPreCache.find(preAncKey);
      if (preIt != weakAncPreCache.end() && preIt->second.ctxId != 0) {
        const auto &preEnt = preIt->second;
        const auto &cand = weakAncCtxs[preEnt.ctxId];
        if (!cand.stale && cand.builtUnivBuild == curUnivBuild &&
            cand.builtHubGlobalVer == hubGlobalVersion &&
            (!globalVisible ||
             cand.builtEntryUnkRefVer == entryUnkRefVersion)) {
          activeWeakAncCtxId = preEnt.ctxId;
          activeFuncs = preEnt.coreFuncs;
          for (std::size_t ci = 0; ci < activeFuncs.size(); ++ci) {
            const FunctionID rep = activeFuncs[ci];
            fnActiveGen[rep] = g;
            fnActiveIdx[rep] = static_cast<std::uint32_t>(ci);
            if (preEnt.coreIsMod[ci])
              fnTransModGen[rep] = g;
            if (preEnt.coreIsRef[ci])
              fnTransRefGen[rep] = g;
          }
          if (preEnt.coreHasHub) {
            if (preEnt.coreHubMod)
              fnTransModGen[hubIdx] = g;
            if (preEnt.coreHubRef)
              fnTransRefGen[hubIdx] = g;
          }
        }
      }
    }

    if (activeWeakAncCtxId == 0) {
      for (FunctionID f : directModFuncs)
        markActive(canon(f));
      propagateModUp(exitDirectModFuncs);
      propagateRefUp(directRefFuncs);

      // For active super-vertices of a non-unknown object, propagate TransMod
      // upward for any active super-vertex performing an exit-reaching unknown
      // store (directly or via a carrying summarized callee).
      if (!isUnknownObj && !p.objects_[object].readOnly) {
        std::vector<FunctionID> extraModSeeds;
        const bool hubInUnivTmp = useUniv && (fnUnivScc[hubIdx] != 0);
        auto isCanonActTmp = [&](FunctionID ct) -> bool {
          if (ct == hubIdx)
            return unknownHubActive && !hubInUnivTmp &&
                   (fnTransModGen[hubIdx] == g || fnTransRefGen[hubIdx] == g);
          return fnActiveGen[ct] == g;
        };
        const std::size_t maxActBound = activeFuncs.size() + 1;
        for (FunctionID rep : activeFuncs) {
          if (fnTransModGen[rep] == g)
            continue;
          if (const std::uint32_t u = univIdOfRep(rep)) {
            const auto &scc = univSccs[u];
            bool hasExitUnknownMod = scc.hasLocalExitUnkStore;
            if (!hasExitUnknownMod) {
              ensureUnivExtSummary(u, globalVisible);
              const auto &extSum =
                  globalVisible ? scc.globalExtSummary : scc.alwaysExtSummary;
              for (const auto &grp : extSum.modGroups) {
                bool hasInact = (grp.canonTargets.size() > maxActBound);
                if (!hasInact) {
                  for (FunctionID ct : grp.canonTargets) {
                    if (!isCanonActTmp(ct)) {
                      hasInact = true;
                      break;
                    }
                  }
                }
                if (hasInact) {
                  hasExitUnknownMod = true;
                  break;
                }
              }
            }
            if (!hasExitUnknownMod && !globalVisible) {
              auto csIt = univCarrierSites.find(u);
              if (csIt != univCarrierSites.end()) {
                for (CallSiteID site : csIt->second) {
                  const auto &cs = p.callSites_[site];
                  const FunctionID caller = p.blocks_[cs.block].function;
                  if (!cs.mayReturn ||
                      !funcCFGs[caller].canReachExit[blockToLocal[cs.block]])
                    continue;
                  for (FunctionID v : liveCallTargets[site]) {
                    if (fnUnivScc[v] == u)
                      continue;
                    if (fnActiveGen[canon(v)] != g && unknownModSummary[v]) {
                      hasExitUnknownMod = true;
                      break;
                    }
                  }
                  if (!hasExitUnknownMod && siteCallsUnknownHub[site] &&
                      !scc.containsHub) {
                    const FunctionID ch = canon(hubIdx);
                    if (!isCanonActTmp(ch) && unknownModSummary[hubIdx])
                      hasExitUnknownMod = true;
                  }
                  if (hasExitUnknownMod)
                    break;
                }
              }
            }
            if (hasExitUnknownMod)
              extraModSeeds.push_back(rep);
          } else {
            const auto &fc = funcCFGs[rep];
            if (fc.exitLocal == InvalidLocal)
              continue;
            bool hasExitUnknownMod = (fnLocalExitUnkStoreNode[rep] != 0);
            if (!hasExitUnknownMod) {
              for (CallSiteID site : funcCallSites[rep]) {
                const auto &cs = p.callSites_[site];
                if (!cs.mayReturn || !fc.canReachExit[blockToLocal[cs.block]] ||
                    !callSiteCanCarryObject(site))
                  continue;
                for (FunctionID callee : liveCallTargets[site]) {
                  if (fnActiveGen[canon(callee)] != g &&
                      unknownModSummary[callee]) {
                    hasExitUnknownMod = true;
                    break;
                  }
                }
                if (!hasExitUnknownMod && siteCallsUnknownHub[site]) {
                  const FunctionID ch = canon(hubIdx);
                  if (!isCanonActTmp(ch) && unknownModSummary[hubIdx])
                    hasExitUnknownMod = true;
                }
                if (hasExitUnknownMod)
                  break;
              }
            }
            if (hasExitUnknownMod)
              extraModSeeds.push_back(rep);
          }
        }
        if (!extraModSeeds.empty())
          propagateModUp(extraModSeeds);
      }

      // Theorem Y Level-2 post-propagation partition `activeFuncs = CORE U ANC`
      // for weak non-unknown objects: any strict ancestor `a in ANC` (zero own
      // accesses, unreachable downward from `CORE`) is summarized once into a
      // reusable `WeakAncestorContext`.
      if (!isUnknownObj && !objCanStrong && activeFuncs.size() >= 3 &&
          !directFuncs.empty()) {
        const std::size_t nAllAct = activeFuncs.size();
        const bool hubInUnivP = useUniv && (fnUnivScc[hubIdx] != 0);
        const bool sepHubActP =
            unknownHubActive && !hubInUnivP &&
            (fnTransModGen[hubIdx] == g || fnTransRefGen[hubIdx] == g);
        std::vector<std::uint8_t> isCore(nAllAct, 0);
        std::vector<std::size_t> coreWork;
        for (FunctionID f : directFuncs) {
          const FunctionID rep = canon(f);
          if (fnActiveGen[rep] == g) {
            const std::size_t idx = fnActiveIdx[rep];
            if (!isCore[idx]) {
              isCore[idx] = 1;
              coreWork.push_back(idx);
            }
          }
        }
        bool hubInCore = false;
        while (!coreWork.empty() && !hubInCore) {
          const std::size_t idx = coreWork.back();
          coreWork.pop_back();
          const FunctionID rep = activeFuncs[idx];
          if (const std::uint32_t u = univIdOfRep(rep)) {
            const auto &scc = univSccs[u];
            const auto &retC = globalVisible ? scc.globalRetExtCanonSet
                                             : scc.alwaysRetExtCanonSet;
            const auto &noRetC = globalVisible ? scc.globalNoRetExtCanonSet
                                               : scc.alwaysNoRetExtCanonSet;
            if (sepHubActP &&
                ((globalVisible ? scc.globalRetCallsHub
                                : scc.alwaysRetCallsHub) ||
                 (globalVisible ? scc.globalNoRetCallsHub
                                : scc.alwaysNoRetCallsHub))) {
              hubInCore = true;
              break;
            }
            for (std::size_t j = 0; j < nAllAct; ++j) {
              if (isCore[j])
                continue;
              const FunctionID rj = activeFuncs[j];
              if (retC.count(rj) || noRetC.count(rj)) {
                isCore[j] = 1;
                coreWork.push_back(j);
              }
            }
            if (!globalVisible) {
              auto csIt = univCarrierSites.find(u);
              if (csIt != univCarrierSites.end()) {
                for (CallSiteID site : csIt->second) {
                  for (FunctionID v : liveCallTargets[site]) {
                    const FunctionID cv = canon(v);
                    if (fnActiveGen[cv] == g) {
                      const std::size_t j = fnActiveIdx[cv];
                      if (!isCore[j]) {
                        isCore[j] = 1;
                        coreWork.push_back(j);
                      }
                    }
                  }
                  if (siteCallsUnknownHub[site] && sepHubActP) {
                    hubInCore = true;
                    break;
                  }
                }
              }
            }
          } else {
            for (CallSiteID site : funcCallSites[rep]) {
              if (!callSiteCanCarryObject(site))
                continue;
              for (FunctionID callee : liveCallTargets[site]) {
                const FunctionID cc = canon(callee);
                if (fnActiveGen[cc] == g) {
                  const std::size_t j = fnActiveIdx[cc];
                  if (!isCore[j]) {
                    isCore[j] = 1;
                    coreWork.push_back(j);
                  }
                }
              }
              if (siteCallsUnknownHub[site] && sepHubActP) {
                hubInCore = true;
                break;
              }
            }
          }
        }

        if (!hubInCore) {
          // If `sepHubActP` has ZERO reachable `addressTakenFuncs` inside `ANC`,
          // place `hubIdx` in `CORE` (`coreHasHub = true`) so ALL thousands of
          // `addressTakenFuncs` share ONE `WeakAncestorContext` for the callers
          // above `hub` (with `hubIdx` as the single boundary port!).
          bool coreHasHub = false;
          if (sepHubActP) {
            bool hubCallsAnc = false;
            for (std::size_t i = 0; i < nAllAct && !hubCallsAnc; ++i) {
              if (isCore[i])
                continue;
              const FunctionID rep = activeFuncs[i];
              if (const std::uint32_t u = univIdOfRep(rep)) {
                for (FunctionID m : univSccs[u].members) {
                  if (isAddressTakenFunc[m]) {
                    hubCallsAnc = true;
                    break;
                  }
                }
              } else if (isAddressTakenFunc[rep]) {
                hubCallsAnc = true;
              }
            }
            if (!hubCallsAnc)
              coreHasHub = true;
          }
          const bool ancHubAct = sepHubActP && !coreHasHub;

          std::vector<FunctionID> coreFuncs;
          std::vector<FunctionID> ancFuncs;
          for (std::size_t i = 0; i < nAllAct; ++i) {
            if (isCore[i])
              coreFuncs.push_back(activeFuncs[i]);
            else
              ancFuncs.push_back(activeFuncs[i]);
          }
          std::vector<CallSiteID> ancCarrierSites;
          if (!globalVisible && !carrierMarkedSites.empty()) {
            for (CallSiteID s : carrierMarkedSites) {
              const FunctionID cRep =
                  canon(p.blocks_[p.callSites_[s].block].function);
              if (fnActiveGen[cRep] == g && !isCore[fnActiveIdx[cRep]])
                ancCarrierSites.push_back(s);
            }
            std::sort(ancCarrierSites.begin(), ancCarrierSites.end());
          }
          const std::size_t minAncForCtx = ancCarrierSites.empty() ? 2 : 12;
          if (!coreFuncs.empty() && ancFuncs.size() >= minAncForCtx) {
            std::sort(coreFuncs.begin(), coreFuncs.end());
            std::sort(ancFuncs.begin(), ancFuncs.end());

            // Collect ONLY the boundary subset of `CORE` (plus `hubIdx` if
            // `coreHasHub`) that is actually targeted by at least one carrying
            // callsite (or `ancHubAct`) in `ANC`!
            std::vector<FunctionID> ancPortFuncs;
            for (FunctionID c : coreFuncs) {
              bool calledByAnc = false;
              if (const std::uint32_t u = univIdOfRep(c)) {
                for (CallSiteID site : univSccs[u].extCallerSites) {
                  const auto &cs = p.callSites_[site];
                  if (!blockReachable[cs.block] || !callSiteCanCarryObject(site))
                    continue;
                  const FunctionID cRep = canon(p.blocks_[cs.block].function);
                  if (fnActiveGen[cRep] == g && !isCore[fnActiveIdx[cRep]]) {
                    calledByAnc = true;
                    break;
                  }
                }
                if (!calledByAnc && ancHubAct) {
                  for (FunctionID m : univSccs[u].members) {
                    if (isAddressTakenFunc[m]) {
                      calledByAnc = true;
                      break;
                    }
                  }
                }
              } else {
                for (CallSiteID site : callerSites[c]) {
                  const auto &cs = p.callSites_[site];
                  if (!blockReachable[cs.block] || !callSiteCanCarryObject(site))
                    continue;
                  const FunctionID cRep = canon(p.blocks_[cs.block].function);
                  if (fnActiveGen[cRep] == g && !isCore[fnActiveIdx[cRep]]) {
                    calledByAnc = true;
                    break;
                  }
                }
                if (!calledByAnc && ancHubAct && isAddressTakenFunc[c])
                  calledByAnc = true;
              }
              if (calledByAnc)
                ancPortFuncs.push_back(c);
            }
            if (coreHasHub) {
              bool hubCalledByAnc = false;
              for (CallSiteID site : unknownHubCallerSites) {
                const auto &cs = p.callSites_[site];
                if (!blockReachable[cs.block] || !callSiteCanCarryObject(site))
                  continue;
                const FunctionID cRep = canon(p.blocks_[cs.block].function);
                if (fnActiveGen[cRep] == g && !isCore[fnActiveIdx[cRep]]) {
                  hubCalledByAnc = true;
                  break;
                }
              }
              if (hubCalledByAnc)
                ancPortFuncs.push_back(hubIdx);
            }

            if (!ancPortFuncs.empty()) {
              WeakAncPostKey postKey;
              postKey.univBuild = curUnivBuild;
              postKey.entryUnkRefVer = globalVisible ? entryUnkRefVersion : 0ULL;
              postKey.hubStateVer = ancHubAct ? fnStateVer[hubIdx] : 0ULL;
              postKey.isGlobal = globalVisible;
              postKey.scopeRoot = scopeRoot;
              postKey.ancHubState =
                  ancHubAct
                      ? static_cast<std::uint8_t>(
                            (fnTransModGen[hubIdx] == g ? 2U : 0U) |
                            (fnTransRefGen[hubIdx] == g ? 1U : 0U))
                      : 0U;
              postKey.coreStates.reserve(ancPortFuncs.size());
              for (FunctionID pf : ancPortFuncs) {
                postKey.coreStates.push_back(
                    (static_cast<std::uint64_t>(pf) << 2) |
                    (fnTransModGen[pf] == g ? 2ULL : 0ULL) |
                    (fnTransRefGen[pf] == g ? 1ULL : 0ULL));
              }
              postKey.ancStates.reserve(ancFuncs.size());
              postKey.ancStateVers.reserve(ancFuncs.size());
              for (FunctionID a : ancFuncs) {
                postKey.ancStates.push_back(
                    (static_cast<std::uint64_t>(a) << 2) |
                    (fnTransModGen[a] == g ? 2ULL : 0ULL) |
                    (fnTransRefGen[a] == g ? 1ULL : 0ULL));
                const std::uint32_t au = univIdOfRep(a);
                postKey.ancStateVers.push_back(
                    au != 0 ? univSccs[au].stateVer : fnStateVer[a]);
              }
              postKey.ancCarrierSites = std::move(ancCarrierSites);

              auto postIt = weakAncPostCache.find(postKey);
              if (postIt != weakAncPostCache.end()) {
                activeWeakAncCtxId = postIt->second;
                auto &reusedCtx = weakAncCtxs[activeWeakAncCtxId];
                if (reusedCtx.stale) {
                  reusedCtx.stale = false;
                  reusedCtx.builtUnivBuild = curUnivBuild;
                  reusedCtx.builtEntryUnkRefVer = entryUnkRefVersion;
                  reusedCtx.builtHubGlobalVer = hubGlobalVersion;
                  for (FunctionID a : ancFuncs) {
                    if (const std::uint32_t u = univIdOfRep(a))
                      appendUnique(univSccs[u].weakAncCtxIds,
                                   activeWeakAncCtxId);
                    else
                      appendUnique(funcWeakAncCtxs[a], activeWeakAncCtxId);
                  }
                  if (ancHubAct)
                    appendUnique(funcWeakAncCtxs[hubIdx], activeWeakAncCtxId);
                  for (FunctionID pf : ancPortFuncs) {
                    if (pf == hubIdx)
                      appendUnique(funcWeakAncCoreWatch[hubIdx],
                                   activeWeakAncCtxId);
                    else if (const std::uint32_t u = univIdOfRep(pf))
                      appendUnique(univSccs[u].weakAncCoreWatchIds,
                                   activeWeakAncCtxId);
                    else
                      appendUnique(funcWeakAncCoreWatch[pf],
                                   activeWeakAncCtxId);
                  }
                }
              } else {
                activeWeakAncCtxId = buildWeakAncestorContext(
                    g, useUniv, globalVisible, scopeRoot, ancHubAct,
                    ancHubAct && fnTransModGen[hubIdx] == g, ancPortFuncs,
                    ancFuncs, univCarrierSites, callSiteCanCarryObject);
                weakAncPostCache.emplace(std::move(postKey),
                                         activeWeakAncCtxId);
              }
              if (preAncEligible) {
                WeakAncPreEntry preEnt;
                preEnt.ctxId = activeWeakAncCtxId;
                preEnt.coreFuncs = coreFuncs;
                preEnt.coreIsMod.resize(coreFuncs.size(), 0);
                preEnt.coreIsRef.resize(coreFuncs.size(), 0);
                for (std::size_t ci = 0; ci < coreFuncs.size(); ++ci) {
                  preEnt.coreIsMod[ci] =
                      (fnTransModGen[coreFuncs[ci]] == g) ? 1 : 0;
                  preEnt.coreIsRef[ci] =
                      (fnTransRefGen[coreFuncs[ci]] == g) ? 1 : 0;
                }
                preEnt.coreHasHub = coreHasHub;
                preEnt.coreHubMod = coreHasHub && (fnTransModGen[hubIdx] == g);
                preEnt.coreHubRef = coreHasHub && (fnTransRefGen[hubIdx] == g);
                weakAncPreCache[std::move(preAncKey)] = std::move(preEnt);
              }

              // Switch `activeFuncs` to `CORE` only; clear `ANC` markers so
              // `CORE` sees zero active ancestors directly.
              for (FunctionID a : ancFuncs) {
                fnActiveGen[a] = 0;
                fnTransModGen[a] = 0;
                fnTransRefGen[a] = 0;
              }
              if (ancHubAct) {
                fnTransModGen[hubIdx] = 0;
                fnTransRefGen[hubIdx] = 0;
              }
              activeFuncs = std::move(coreFuncs);
              for (std::size_t ci = 0; ci < activeFuncs.size(); ++ci)
                fnActiveIdx[activeFuncs[ci]] = static_cast<std::uint32_t>(ci);
            }
          }
        }
      }
    }

    for (FunctionID rep : activeFuncs) {
      if (const std::uint32_t u = univIdOfRep(rep))
        univSccs[u].accessedByObjects.insertBit(object);
      else
        funcAccessedByObjects[rep].insertBit(object);
    }
    attachWeakAncCtx(object, activeWeakAncCtxId);

    auto isTransMod = [&](FunctionID f) { return fnTransModGen[canon(f)] == g; };
    auto isTransRef = [&](FunctionID f) { return fnTransRefGen[canon(f)] == g; };
    auto isTransStrong = [&](FunctionID f) { return fnTransStrongGen[f] == g; };
    const bool hubInUniv = useUniv && (fnUnivScc[hubIdx] != 0);
    const bool hubActive =
        unknownHubActive && (isTransMod(hubIdx) || isTransRef(hubIdx));
    const bool separateHubActive = hubActive && !hubInUniv;

    // Exact strong-kill propagation (only for `objCanStrong`, so `useUniv == false`).
    if (objCanStrong) {
      std::vector<FunctionID> strongWork;
      for (FunctionID f : activeFuncs) {
        if (!isTransMod(f))
          continue;
        bool seedsStrong = !ownStrongOf(f).empty();
        if (!seedsStrong) {
          const auto &fc = funcCFGs[f];
          for (CallSiteID site : funcCallSites[f]) {
            const auto &cs = p.callSites_[site];
            const auto &targets = liveCallTargets[site];
            if (!cs.mayReturn || targets.empty() || liveCallExternal[site] ||
                siteCallsUnknownHub[site] || cs.isInvoke ||
                !fc.canReachExit[blockToLocal[cs.block]] ||
                !callSiteCanCarryObject(site))
              continue;
            bool allNoRet = true;
            for (FunctionID callee : targets) {
              if (funcCFGs[callee].exitLocal != InvalidLocal) {
                allNoRet = false;
                break;
              }
            }
            if (allNoRet) {
              seedsStrong = true;
              break;
            }
          }
        }
        if (seedsStrong && fnTransStrongGen[f] != g) {
          fnTransStrongGen[f] = g;
          strongWork.push_back(f);
        }
      }
      while (!strongWork.empty()) {
        const FunctionID f = strongWork.back();
        strongWork.pop_back();
        if (scopeRoot && f == scopeRoot)
          continue;
        for (CallSiteID site : callerSites[f]) {
          const auto &cs = p.callSites_[site];
          const BlockID blk = cs.block;
          if (!blockReachable[blk] || !cs.mayReturn ||
              !callSiteCanCarryObject(site))
            continue;
          const FunctionID caller = p.blocks_[blk].function;
          if (isTransMod(caller) &&
              funcCFGs[caller].canReachExit[blockToLocal[blk]] &&
              fnTransStrongGen[caller] != g) {
            fnTransStrongGen[caller] = g;
            strongWork.push_back(caller);
          }
        }
      }
    }

    // PreprocessGlobalFlow: prepare per-super-vertex call summaries, summarized
    // unknown callee mods/reads, and auxiliary entry/exit nodes in dense arrays.
    const std::size_t nAct = activeFuncs.size();

    std::vector<NodeID> actEntry(nAct, 0);
    std::vector<NodeID> actExit(nAct, 0);
    std::vector<std::vector<CallFlowSummary>> actCalls(nAct);
    std::vector<std::vector<UnknownCallMod>> actUnkMods(nAct);
    std::vector<std::vector<UnknownCallMod>> actUnkReads(nAct);
    // For UnivWeakSCC super-vertices: O(|activeFuncs|) active external callees,
    // extra non-alwaysCarry carrier callees, and compact exact signature keys.
    struct UnivExtList {
      std::vector<std::size_t> activeRetIdxs;
      std::vector<std::size_t> activeNoRetIdxs;
      bool activeRetHub = false;
      bool activeNoRetHub = false;
      std::vector<FunctionID> extraRetCallees;
      std::vector<FunctionID> extraNoRetCallees;
      std::vector<FunctionID> extraNonExitRetFuncs;
      std::vector<std::uint64_t> sigKeys;
      NodeSet mergedOwnAcc;
    };
    std::vector<UnivExtList> actUnivExt(useUniv ? nAct : 0);
    std::vector<std::uint8_t> actNeedExit(nAct, 0);
    std::vector<std::uint8_t> actReused(nAct, 0);

    // Map `WeakAncestorContext` boundary ports onto `CORE` slots (`0..nAct-1`,
    // or `nAct` = `hubLocalIdx` when `hubIdx` is a `CORE` port).
    const std::size_t hubLocalIdx = nAct;
    std::vector<std::size_t> ancPortSlot;
    std::vector<std::uint8_t> coreSlotNeedExitFromAnc(nAct + 1, 0);
    std::vector<std::uint8_t> coreSlotInitFromAnc(nAct + 1, 0);
    if (activeWeakAncCtxId != 0) {
      const auto &ctx = weakAncCtxs[activeWeakAncCtxId];
      ancPortSlot.resize(ctx.coreFuncs.size(), nAct + 1);
      for (std::size_t pi = 0; pi < ctx.coreFuncs.size(); ++pi) {
        const FunctionID pf = ctx.coreFuncs[pi];
        std::size_t slot = nAct + 1;
        if (pf == hubIdx) {
          if (separateHubActive)
            slot = hubLocalIdx;
        } else if (fnActiveGen[pf] == g) {
          slot = fnActiveIdx[pf];
        }
        ancPortSlot[pi] = slot;
        if (slot <= nAct) {
          if (ctx.coreNeedExitFromAnc[pi])
            coreSlotNeedExitFromAnc[slot] = 1;
          if (ctx.initReachesCoreEntry[pi])
            coreSlotInitFromAnc[slot] = 1;
        }
      }
    }

    for (std::size_t i = 0; i < nAct; ++i) {
      const FunctionID rep = activeFuncs[i];
      if (const std::uint32_t u = univIdOfRep(rep)) {
        ensureUnivExtSummary(u, globalVisible);
        const auto &scc = univSccs[u];
        const auto &extSum =
            globalVisible ? scc.globalExtSummary : scc.alwaysExtSummary;
        auto &ext = actUnivExt[i];
        auto ownIt = univOwnFuncs.find(u);
        if (ownIt != univOwnFuncs.end()) {
          for (FunctionID f : ownIt->second)
            ext.mergedOwnAcc.unite(ownAccessesOf(f));
        }
        const auto &retCanon = globalVisible ? scc.globalRetExtCanonSet
                                             : scc.alwaysRetExtCanonSet;
        const auto &noRetCanon = globalVisible ? scc.globalNoRetExtCanonSet
                                               : scc.alwaysNoRetExtCanonSet;
        for (std::size_t j = 0; j < nAct; ++j) {
          if (j == i)
            continue;
          const FunctionID rj = activeFuncs[j];
          if (retCanon.count(rj))
            ext.activeRetIdxs.push_back(j);
          else if (noRetCanon.count(rj))
            ext.activeNoRetIdxs.push_back(j);
        }
        if (separateHubActive) {
          if (globalVisible ? scc.globalRetCallsHub : scc.alwaysRetCallsHub)
            ext.activeRetHub = true;
          else if (globalVisible ? scc.globalNoRetCallsHub
                                 : scc.alwaysNoRetCallsHub)
            ext.activeNoRetHub = true;
        }
        if (!globalVisible) {
          auto csIt = univCarrierSites.find(u);
          if (csIt != univCarrierSites.end()) {
            for (CallSiteID site : csIt->second) {
              const auto &cs = p.callSites_[site];
              const FunctionID caller = p.blocks_[cs.block].function;
              const bool reachesExit =
                  funcCFGs[caller].canReachExit[blockToLocal[cs.block]];
              const bool isRetExit = cs.mayReturn && reachesExit;
              const bool isNonExitRet = cs.mayReturn && !reachesExit;
              auto addExtra = [&](FunctionID v) {
                if (fnUnivScc[v] == u)
                  return;
                if (isNonExitRet)
                  appendUnique(ext.extraNonExitRetFuncs, caller);
                if (isRetExit) {
                  if (!scc.alwaysRetExtRawSet.count(v))
                    appendUnique(ext.extraRetCallees, v);
                } else {
                  if (!scc.alwaysRetExtRawSet.count(v) &&
                      !scc.alwaysNoRetExtRawSet.count(v))
                    appendUnique(ext.extraNoRetCallees, v);
                }
              };
              for (FunctionID v : liveCallTargets[site])
                addExtra(v);
              if (siteCallsUnknownHub[site] && !scc.containsHub &&
                  unknownHubActive)
                addExtra(hubIdx);
            }
          }
        }
        // Compact exact signature keys for `LastEmitState` (O(|activeExt|)).
        ext.sigKeys.reserve(ext.activeRetIdxs.size() +
                            ext.activeNoRetIdxs.size() +
                            ext.extraRetCallees.size() +
                            ext.extraNoRetCallees.size() + 3);
        ext.sigKeys.push_back(
            (extSum.builtSumVer << 3) | (isTransMod(rep) ? 4ULL : 0ULL) |
            (ext.activeRetHub ? 2ULL : 0ULL) |
            (ext.activeNoRetHub ? 1ULL : 0ULL));
        for (std::size_t j : ext.activeRetIdxs) {
          const FunctionID rj = activeFuncs[j];
          ext.sigKeys.push_back((static_cast<std::uint64_t>(rj) << 5) | 16ULL |
                                (isTransMod(rj) ? 8ULL : 0ULL) |
                                (isTransRef(rj) ? 4ULL : 0ULL));
        }
        for (std::size_t j : ext.activeNoRetIdxs) {
          const FunctionID rj = activeFuncs[j];
          ext.sigKeys.push_back((static_cast<std::uint64_t>(rj) << 5) |
                                (isTransMod(rj) ? 8ULL : 0ULL) |
                                (isTransRef(rj) ? 4ULL : 0ULL));
        }
        auto pushExtraKey = [&](FunctionID v, bool isRet) {
          const bool mod = isTransMod(v);
          const bool ref = isTransRef(v);
          const std::uint64_t uMod = (!mod && !ref && unknownModSummary[v]) ? 1 : 0;
          const std::uint64_t uRef = (!mod && !ref && unknownTransRef[v]) ? 1 : 0;
          ext.sigKeys.push_back((1ULL << 63) |
                                (static_cast<std::uint64_t>(v) << 5) |
                                (isRet ? 16ULL : 0ULL) | (mod ? 8ULL : 0ULL) |
                                (ref ? 4ULL : 0ULL) | (uMod << 1) | uRef);
        };
        for (FunctionID v : ext.extraRetCallees)
          pushExtraKey(v, true);
        for (FunctionID v : ext.extraNoRetCallees)
          pushExtraKey(v, false);
        std::sort(ext.sigKeys.begin() + 1, ext.sigKeys.end());

        bool hasRelevantCaller = (coreSlotNeedExitFromAnc[i] != 0);
        if (!hasRelevantCaller && isTransMod(rep)) {
          for (CallSiteID site : scc.extCallerSites) {
            const auto &cs = p.callSites_[site];
            if (blockReachable[cs.block] && cs.mayReturn &&
                fnActiveGen[canon(p.blocks_[cs.block].function)] == g &&
                callSiteCanCarryObject(site)) {
              hasRelevantCaller = true;
              break;
            }
          }
          if (!hasRelevantCaller && separateHubActive && isTransMod(hubIdx)) {
            for (FunctionID m : scc.members) {
              if (isAddressTakenFunc[m]) {
                hasRelevantCaller = true;
                break;
              }
            }
          }
        }
        actNeedExit[i] = hasRelevantCaller ? 1 : 0;
        continue;
      }

      const FunctionID f = rep;
      auto &summaries = actCalls[i];
      auto &unkMods = actUnkMods[i];
      auto &unkReads = actUnkReads[i];
      for (CallSiteID site : funcCallSites[f]) {
        if (!callSiteCanCarryObject(site))
          continue;
        const auto &targets = liveCallTargets[site];
        bool anyAccess = false;
        bool anyUnknownRead = false;
        bool anyMod = false;
        bool hasHub = false;
        bool allMod = !targets.empty() && !liveCallExternal[site] &&
                      !siteCallsUnknownHub[site] &&
                      !p.callSites_[site].isInvoke;
        bool allStrongKill = objCanStrong && allMod;
        bool allNoReturn = allMod && p.callSites_[site].mayReturn;
        std::vector<NodeID> siteUnkSummaries;
        for (FunctionID callee : targets) {
          if (funcCFGs[callee].exitLocal != InvalidLocal)
            allNoReturn = false;
          if (isTransMod(callee) || isTransRef(callee)) {
            anyAccess = true;
            if (isTransMod(callee)) {
              anyMod = true;
              if (!isTransStrong(callee))
                allStrongKill = false;
            } else {
              allMod = false;
              allStrongKill = false;
            }
          } else {
            allMod = false;
            allStrongKill = false;
            if (!isUnknownObj) {
              if (unknownTransRef[callee])
                anyUnknownRead = true;
              if (p.callSites_[site].mayReturn && unknownModSummary[callee])
                appendUnique(siteUnkSummaries, unknownModSummary[callee]);
            }
          }
        }
        if (siteCallsUnknownHub[site]) {
          allMod = false;
          allStrongKill = false;
          allNoReturn = false;
          if (hubActive) {
            anyAccess = true;
            hasHub = true;
            if (isTransMod(hubIdx))
              anyMod = true;
          } else if (!isUnknownObj) {
            if (unknownTransRef[hubIdx]) {
              anyUnknownRead = true;
              hasHub = true;
            }
            if (p.callSites_[site].mayReturn && unknownModSummary[hubIdx])
              appendUnique(siteUnkSummaries, unknownModSummary[hubIdx]);
          }
        }
        for (NodeID sumNode : siteUnkSummaries)
          unkMods.push_back({site, sumNode});
        const BlockID siteBlk = p.callSites_[site].block;
        const LocalBlock siteLocal = blockToLocal[siteBlk];
        const bool hasCodeAfterCall =
            !funcCFGs[f].succs[siteLocal].empty() ||
            (callEventPos[site] + 1 < p.blocks_[siteBlk].events.size());
        const bool strongNoReturnKill =
            objCanStrong && allNoReturn && hasCodeAfterCall;
        if (!anyAccess && !anyUnknownRead && !strongNoReturnKill)
          continue;
        if (!anyAccess && anyUnknownRead && !hasHub && !strongNoReturnKill &&
            p.callSites_[site].calleePointer == 0 && targets.size() == 1) {
          unkReads.push_back({site, unknownReadEntry[*targets.begin()]});
          continue;
        }
        const bool hasIn = anyAccess || anyUnknownRead;
        const bool hasOut =
            (anyMod && p.callSites_[site].mayReturn) || strongNoReturnKill;
        const bool outStrong =
            hasOut && objCanStrong && (allStrongKill || strongNoReturnKill);
        summaries.push_back({site, hasIn, hasOut, outStrong, hasHub});
      }
      bool hasRelevantCaller = (coreSlotNeedExitFromAnc[i] != 0);
      if (!hasRelevantCaller && isTransMod(f) &&
          funcCFGs[f].exitLocal != InvalidLocal &&
          !(scopeRoot && f == scopeRoot)) {
        for (CallSiteID site : callerSites[f]) {
          const auto &cs = p.callSites_[site];
          if (blockReachable[cs.block] && cs.mayReturn &&
              fnActiveGen[canon(p.blocks_[cs.block].function)] == g &&
              callSiteCanCarryObject(site)) {
            hasRelevantCaller = true;
            break;
          }
        }
        if (!hasRelevantCaller && unknownHubActive && isAddressTakenFunc[f]) {
          if (hubInUniv) {
            if (fnActiveGen[canon(hubIdx)] == g)
              hasRelevantCaller = true;
          } else if (separateHubActive && isTransMod(hubIdx)) {
            hasRelevantCaller = true;
          }
        }
      }
      actNeedExit[i] = hasRelevantCaller ? 1 : 0;
    }

    // Late interprocedural strong-kill check (only for `objCanStrong`).
    if (alreadyEmitted && objCanStrong) {
      auto &objCache = objectFuncCache[object];
      for (std::size_t i = 0; i < nAct; ++i) {
        auto cacheIt = objCache.find(activeFuncs[i]);
        if (cacheIt == objCache.end())
          continue;
        for (const auto &cs : actCalls[i]) {
          if (!cs.outStrong)
            continue;
          const bool wasStrong = std::any_of(
              cacheIt->second.calls.begin(), cacheIt->second.calls.end(),
              [&](const CallFlowSummary &old) {
                return old.site == cs.site && old.outStrong;
              });
          if (!wasStrong) {
            weakenObjectLocally(
                object,
                "a late interprocedural strong kill invalidated the escape "
                "schedule");
            pending.insert(object);
            return;
          }
        }
      }
    }

    // Exact whole-object skip for already-emitted non-unknown objects: compare
    // in-place before allocating/copying.
    NodeID savedReuseUnivNode = 0;
    NodeSet savedReusePrevOwnAcc;
    if (!isUnknownObj) {
      std::size_t reachAT = 0;
      std::uint64_t hubUnkVer = 0;
      if (separateHubActive) {
        for (FunctionID atFn : addressTakenFuncs)
          if (funcReachable[atFn])
            ++reachAT;
        hubUnkVer = unknownAccessVersion;
      }
      std::size_t entryUnkRefCount = 0;
      if (activeWeakAncCtxId == 0 && globalVisible &&
          !unknownDirectRefFuncs.empty()) {
        for (FunctionID entry : p.entryFunctions_) {
          if (funcReachable[entry] && fnActiveGen[canon(entry)] != g &&
              unknownTransRef[entry])
            ++entryUnkRefCount;
        }
      }
      auto unkCountOfRep = [&](FunctionID rep) -> std::size_t {
        if (const std::uint32_t u = univIdOfRep(rep))
          return static_cast<std::size_t>(univSccs[u].unkVersion);
        auto uIt = unknownFnAccesses.find(rep);
        return uIt == unknownFnAccesses.end() ? 0 : uIt->second.size();
      };

      std::vector<std::uint32_t> sortedOrder(nAct);
      for (std::uint32_t i = 0; i < nAct; ++i)
        sortedOrder[i] = i;
      std::sort(sortedOrder.begin(), sortedOrder.end(),
                [&](std::uint32_t a, std::uint32_t b) {
                  return activeFuncs[a] < activeFuncs[b];
                });

      const std::uint32_t curWeakAncSummaryId =
          activeWeakAncCtxId ? weakAncCtxs[activeWeakAncCtxId].summaryId : 0U;
      const auto &prevState = lastEmitState[object];
      bool stateMatches =
          alreadyEmitted && prevState.valid &&
          prevState.globalVisible == globalVisible &&
          prevState.objCanStrong == objCanStrong &&
          prevState.hubActive == separateHubActive &&
          prevState.scopeRoot == scopeRoot &&
          prevState.reachableAddrTakenCount == reachAT &&
          prevState.hubUnknownVersion == hubUnkVer &&
          prevState.entryUnknownRefCount == entryUnkRefCount &&
          prevState.weakAncSummaryId == curWeakAncSummaryId &&
          prevState.funcs.size() == nAct;
      if (stateMatches) {
        for (std::size_t k = 0; k < nAct; ++k) {
          const std::uint32_t idx = sortedOrder[k];
          const FunctionID rep = activeFuncs[idx];
          const auto &pf = prevState.funcs[k];
          const std::uint32_t u = univIdOfRep(rep);
          const bool isU = (u != 0);
          const std::uint64_t uStructVer = isU ? univSccs[u].structVersion : 0;
          const NodeSet &accRef =
              isU ? actUnivExt[idx].mergedOwnAcc : ownAccessesOf(rep);
          if (pf.fn != rep || pf.univStructVer != uStructVer ||
              pf.needExit != (actNeedExit[idx] != 0) ||
              pf.unkAccessCount != unkCountOfRep(rep) ||
              pf.calls != actCalls[idx] || pf.unknownMods != actUnkMods[idx] ||
              pf.unknownReads != actUnkReads[idx] || pf.accesses != accRef ||
              pf.strong != ownStrongOf(rep)) {
            stateMatches = false;
            break;
          }
          if (isU) {
            if (pf.univExtKeys != actUnivExt[idx].sigKeys) {
              stateMatches = false;
              break;
            }
          } else {
            if (!pf.univExtKeys.empty() ||
                pf.callTargetCounts.size() != actCalls[idx].size()) {
              stateMatches = false;
              break;
            }
            for (std::size_t ci = 0; ci < actCalls[idx].size(); ++ci) {
              if (pf.callTargetCounts[ci] !=
                  liveCallTargets[actCalls[idx][ci].site].size()) {
                stateMatches = false;
                break;
              }
            }
            if (!stateMatches)
              break;
          }
        }
      }
      if (stateMatches) {
        stats.functionFlowReuseHits += nAct;
        return;
      }
      // Exact incremental UnivWeakSCC node reuse for single-SCC weak objects:
      // when `nAct == 1 && !separateHubActive`, reusing `prevState`'s node `C`
      // avoids allocating a new `C` and re-emitting all ~300 own-access edges!
      if (!objCanStrong && !separateHubActive && nAct == 1 &&
          univIdOfRep(activeFuncs[0]) != 0 && alreadyEmitted &&
          prevState.valid && prevState.globalVisible == globalVisible &&
          !prevState.objCanStrong && !prevState.hubActive &&
          prevState.funcs.size() == 1 &&
          prevState.funcs[0].fn == activeFuncs[0] &&
          prevState.funcs[0].univNode != 0) {
        savedReuseUnivNode = prevState.funcs[0].univNode;
        savedReusePrevOwnAcc = prevState.funcs[0].accesses;
      }
      LastEmitState curState;
      curState.valid = true;
      curState.globalVisible = globalVisible;
      curState.objCanStrong = objCanStrong;
      curState.hubActive = separateHubActive;
      curState.scopeRoot = scopeRoot;
      curState.reachableAddrTakenCount = reachAT;
      curState.hubUnknownVersion = hubUnkVer;
      curState.entryUnknownRefCount = entryUnkRefCount;
      curState.weakAncSummaryId = curWeakAncSummaryId;
      curState.funcs.reserve(nAct);
      for (std::uint32_t idx : sortedOrder) {
        const FunctionID rep = activeFuncs[idx];
        const std::uint32_t u = univIdOfRep(rep);
        const bool isU = (u != 0);
        LastEmitFuncState fs;
        fs.fn = rep;
        fs.univStructVer = isU ? univSccs[u].structVersion : 0;
        fs.accesses = isU ? actUnivExt[idx].mergedOwnAcc : ownAccessesOf(rep);
        fs.strong = ownStrongOf(rep);
        fs.unkAccessCount = unkCountOfRep(rep);
        fs.calls = actCalls[idx];
        fs.callTargetCounts.reserve(fs.calls.size());
        for (const auto &cs : fs.calls)
          fs.callTargetCounts.push_back(liveCallTargets[cs.site].size());
        fs.unknownMods = actUnkMods[idx];
        fs.unknownReads = actUnkReads[idx];
        if (isU)
          fs.univExtKeys = actUnivExt[idx].sigKeys;
        fs.needExit = (actNeedExit[idx] != 0);
        curState.funcs.push_back(std::move(fs));
      }
      lastEmitState[object] = std::move(curState);
    }

    emittedFlow[object] = true;
    objLocalOnly[object] =
        !isUnknownObj && !globalVisible && !objCallExposed[object] &&
        activeWeakAncCtxId == 0 && scopeRoot && nAct == 1 &&
        activeFuncs[0] == scopeRoot && actCalls[0].empty() &&
        actUnkMods[0].empty() && actUnkReads[0].empty();

    auto buildMergedFuncAccesses = [&](FunctionID f) -> NodeSet {
      NodeSet merged = ownAccessesOf(f);
      if (!isUnknownObj) {
        auto uIt = unknownFnAccesses.find(f);
        if (uIt != unknownFnAccesses.end())
          merged.unite(uIt->second);
      }
      return merged;
    };

    for (std::size_t i = 0; i < nAct; ++i) {
      const FunctionID f = activeFuncs[i];
      if (univIdOfRep(f) != 0)
        continue;
      const auto &summaries = actCalls[i];
      const auto &unkMods = actUnkMods[i];
      const auto &unkReads = actUnkReads[i];
      const bool needExit = (actNeedExit[i] != 0);
      const NodeSet &str = ownStrongOf(f);

      if (objCanStrong) {
        auto &objCache = objectFuncCache[object];
        auto cacheIt = objCache.find(f);
        if (cacheIt != objCache.end() && !cacheIt->second.elidedWrappers &&
            cacheIt->second.strong == str &&
            cacheIt->second.calls == summaries &&
            cacheIt->second.unknownMods == unkMods &&
            cacheIt->second.unknownReads == unkReads &&
            cacheIt->second.needExit == needExit &&
            cacheIt->second.accesses == buildMergedFuncAccesses(f)) {
          actReused[i] = 1;
          actEntry[i] = cacheIt->second.entryNode;
          actExit[i] = cacheIt->second.exitNode;
          ++stats.functionFlowReuseHits;
          continue;
        }
      }

      // Cross-object template cache for callsite-free, exit-free functions.
      if (summaries.empty() && unkMods.empty() && unkReads.empty() &&
          !needExit) {
        NodeSet fullAcc = buildMergedFuncAccesses(f);
        LocalTemplateKey key{f, fullAcc, str};
        auto tmplIt = flowTemplates.find(key);
        if (tmplIt != flowTemplates.end()) {
          ++stats.indirectFlowCacheHits;
          actEntry[i] = tmplIt->second;
          actExit[i] = 0;
          if (objCanStrong) {
            ObjectFuncCacheEntry entry;
            entry.accesses = std::move(fullAcc);
            entry.strong = str;
            entry.needExit = false;
            entry.entryNode = tmplIt->second;
            entry.exitNode = 0;
            objectFuncCache[object][f] = std::move(entry);
          }
          actReused[i] = 1;
          continue;
        }
      }
    }

    // Exact weak call-graph SCC contraction over active super-vertices.
    const std::size_t hubTargetIdx =
        hubInUniv && fnActiveGen[canon(hubIdx)] == g
            ? static_cast<std::size_t>(fnActiveIdx[canon(hubIdx)])
            : hubLocalIdx;
    if (savedReuseUnivNode != 0) {
      actEntry[0] = savedReuseUnivNode;
      actExit[0] = actNeedExit[0] ? savedReuseUnivNode : 0;
      actReused[0] = 1;
    }
    std::vector<std::uint8_t> weakEntryElig(nAct + 1, 0);
    std::vector<std::uint8_t> weakExitElig(nAct + 1, 0);
    for (std::size_t i = 0; i < nAct; ++i) {
      if (actReused[i])
        continue;
      const FunctionID f = activeFuncs[i];
      bool hasStrong = !ownStrongOf(f).empty();
      if (!hasStrong && objCanStrong) {
        for (const auto &cs : actCalls[i]) {
          if (cs.outStrong) {
            hasStrong = true;
            break;
          }
        }
      }
      if (!hasStrong) {
        weakEntryElig[i] = 1;
        weakExitElig[i] = actNeedExit[i];
      }
    }
    weakEntryElig[hubLocalIdx] = separateHubActive ? 1 : 0;
    weakExitElig[hubLocalIdx] =
        (separateHubActive && isTransMod(hubIdx)) ? 1 : 0;

    std::vector<std::vector<std::size_t>> entrySucc(nAct + 1);
    std::vector<std::vector<std::size_t>> retSucc(nAct + 1);
    for (std::size_t i = 0; i < nAct; ++i) {
      if (!weakEntryElig[i])
        continue;
      const FunctionID rep = activeFuncs[i];
      if (univIdOfRep(rep) != 0) {
        const auto &ext = actUnivExt[i];
        auto addExtIdxEdge = [&](std::size_t j, bool isRet) {
          if (j == i)
            return;
          if (weakEntryElig[j]) {
            if (entrySucc[i].empty() || entrySucc[i].back() != j)
              entrySucc[i].push_back(j);
          }
          if (isRet && weakExitElig[i] && weakExitElig[j]) {
            if (retSucc[j].empty() || retSucc[j].back() != i)
              retSucc[j].push_back(i);
          }
        };
        for (std::size_t j : ext.activeRetIdxs)
          addExtIdxEdge(j, true);
        for (std::size_t j : ext.activeNoRetIdxs)
          addExtIdxEdge(j, false);
        if (ext.activeRetHub)
          addExtIdxEdge(hubLocalIdx, true);
        else if (ext.activeNoRetHub)
          addExtIdxEdge(hubLocalIdx, false);
        auto addExtraVEdge = [&](FunctionID v, bool isRet) {
          const FunctionID cv = canon(v);
          if (cv == hubIdx) {
            if (separateHubActive)
              addExtIdxEdge(hubLocalIdx, isRet);
          } else if (fnActiveGen[cv] == g) {
            addExtIdxEdge(fnActiveIdx[cv], isRet);
          }
        };
        for (FunctionID v : ext.extraRetCallees)
          addExtraVEdge(v, true);
        for (FunctionID v : ext.extraNoRetCallees)
          addExtraVEdge(v, false);
        continue;
      }

      const FunctionID f = rep;
      const auto &fc = funcCFGs[f];
      for (const auto &cs : actCalls[i]) {
        const bool retReachesExit =
            weakExitElig[i] && cs.hasOut &&
            fc.canReachExit[blockToLocal[p.callSites_[cs.site].block]];
        for (FunctionID callee : liveCallTargets[cs.site]) {
          const FunctionID cCallee = canon(callee);
          if (fnActiveGen[cCallee] != g)
            continue;
          const std::size_t j = fnActiveIdx[cCallee];
          if (cs.hasIn && weakEntryElig[j] && i != j) {
            if (entrySucc[i].empty() || entrySucc[i].back() != j)
              entrySucc[i].push_back(j);
          }
          if (retReachesExit && weakExitElig[j] && i != j) {
            if (retSucc[j].empty() || retSucc[j].back() != i)
              retSucc[j].push_back(i);
          }
        }
        if (cs.hasHub && hubActive && hubTargetIdx != i) {
          if (cs.hasIn && weakEntryElig[hubTargetIdx]) {
            if (entrySucc[i].empty() || entrySucc[i].back() != hubTargetIdx)
              entrySucc[i].push_back(hubTargetIdx);
          }
          if (retReachesExit && weakExitElig[hubTargetIdx]) {
            if (retSucc[hubTargetIdx].empty() ||
                retSucc[hubTargetIdx].back() != i)
              retSucc[hubTargetIdx].push_back(i);
          }
        }
      }
    }
    if (separateHubActive) {
      for (std::size_t j = 0; j < nAct; ++j) {
        const FunctionID rep = activeFuncs[j];
        const std::uint32_t u = univIdOfRep(rep);
        bool hasAT = false;
        if (u != 0) {
          for (FunctionID m : univSccs[u].members) {
            if (isAddressTakenFunc[m]) {
              hasAT = true;
              break;
            }
          }
        } else {
          hasAT = isAddressTakenFunc[rep];
        }
        if (!hasAT)
          continue;
        if (weakEntryElig[j])
          entrySucc[hubLocalIdx].push_back(j);
        if (weakExitElig[hubLocalIdx] && weakExitElig[j])
          retSucc[j].push_back(hubLocalIdx);
      }
    }

    auto [entryComp, entryComps] = computeSCCs(entrySucc);
    std::vector<std::size_t> entryCompSize(entryComps, 0);
    for (std::size_t i = 0; i <= nAct; ++i)
      if (weakEntryElig[i])
        ++entryCompSize[entryComp[i]];

    auto [retComp, retComps] = computeSCCs(retSucc);
    std::vector<std::size_t> retCompSize(retComps, 0);
    for (std::size_t i = 0; i <= nAct; ++i)
      if (weakExitElig[i])
        ++retCompSize[retComp[i]];

    std::vector<std::vector<std::size_t>> entryCompMembers(entryComps);
    for (std::size_t i = 0; i <= nAct; ++i)
      entryCompMembers[entryComp[i]].push_back(i);
    std::vector<std::vector<std::size_t>> retCompMembers(retComps);
    for (std::size_t i = 0; i <= nAct; ++i)
      retCompMembers[retComp[i]].push_back(i);

    const std::size_t totalU = entryComps + retComps;
    std::vector<std::vector<std::size_t>> uniSucc(totalU);
    auto addUniEdge = [&](std::size_t u, std::size_t v) {
      if (u != v && (uniSucc[u].empty() || uniSucc[u].back() != v))
        uniSucc[u].push_back(v);
    };
    for (std::size_t i = 0; i <= nAct; ++i) {
      if (weakEntryElig[i] && weakExitElig[i]) {
        addUniEdge(entryComp[i], entryComps + retComp[i]);
        // Every UnivWeakSCC satisfies the intra-SCC Cycle-Return Condition,
        // so whenever it is TransMod & has exit, `exit_u ->* entry_u` holds.
        if (i < nAct && univIdOfRep(activeFuncs[i]) != 0 &&
            isTransMod(activeFuncs[i]))
          addUniEdge(entryComps + retComp[i], entryComp[i]);
      }
      for (std::size_t j : entrySucc[i])
        addUniEdge(entryComp[i], entryComp[j]);
      for (std::size_t j : retSucc[i])
        addUniEdge(entryComps + retComp[i], entryComps + retComp[j]);
    }
    for (std::size_t i = 0; i < nAct; ++i) {
      if (!weakEntryElig[i] || univIdOfRep(activeFuncs[i]) != 0)
        continue;
      const FunctionID f = activeFuncs[i];
      struct CallEndpoint {
        LocalBlock block;
        std::uint32_t pos;
        std::size_t comp;
      };
      std::vector<CallEndpoint> outCalls;
      std::vector<CallEndpoint> inCalls;
      for (const auto &cs : actCalls[i]) {
        const LocalBlock b = blockToLocal[p.callSites_[cs.site].block];
        const std::uint32_t pos = callEventPos[cs.site];
        if (cs.hasOut) {
          for (FunctionID callee : liveCallTargets[cs.site]) {
            const FunctionID cc = canon(callee);
            if (fnActiveGen[cc] == g && weakExitElig[fnActiveIdx[cc]])
              outCalls.push_back({b, pos, retComp[fnActiveIdx[cc]]});
          }
          if (cs.hasHub && hubActive && weakExitElig[hubTargetIdx])
            outCalls.push_back({b, pos, retComp[hubTargetIdx]});
        }
        if (cs.hasIn) {
          for (FunctionID callee : liveCallTargets[cs.site]) {
            const FunctionID cc = canon(callee);
            if (fnActiveGen[cc] == g && weakEntryElig[fnActiveIdx[cc]])
              inCalls.push_back({b, pos, entryComp[fnActiveIdx[cc]]});
          }
          if (cs.hasHub && hubActive && weakEntryElig[hubTargetIdx])
            inCalls.push_back({b, pos, entryComp[hubTargetIdx]});
        }
      }
      if (outCalls.empty() || inCalls.empty())
        continue;
      ensureFunctionSCCs(f);
      auto &fc = funcCFGs[f];
      std::vector<std::size_t> distinctTargets;
      for (const auto &ic : inCalls)
        appendUnique(distinctTargets, ic.comp);
      if (distinctTargets.size() <= 8) {
        for (std::size_t targetE : distinctTargets) {
          const std::uint64_t ug = ++fc.weakGen;
          std::vector<std::size_t> bWork;
          std::vector<bool> compHasIn(fc.compBlocks.size(), false);
          std::unordered_map<LocalBlock, std::uint32_t> maxInPos;
          for (const auto &ic : inCalls) {
            if (ic.comp != targetE)
              continue;
            auto &mp = maxInPos[ic.block];
            if (ic.pos > mp)
              mp = ic.pos;
            const std::size_t bc = fc.comp[ic.block];
            if (!compHasIn[bc]) {
              compHasIn[bc] = true;
              for (auto pred : fc.compPreds[bc]) {
                if (fc.weakNeededGen[pred] != ug) {
                  fc.weakNeededGen[pred] = ug;
                  bWork.push_back(pred);
                }
              }
            }
          }
          while (!bWork.empty()) {
            const std::size_t bc = bWork.back();
            bWork.pop_back();
            for (auto pred : fc.compPreds[bc]) {
              if (fc.weakNeededGen[pred] != ug) {
                fc.weakNeededGen[pred] = ug;
                bWork.push_back(pred);
              }
            }
          }
          for (const auto &oc : outCalls) {
            const std::size_t bc = fc.comp[oc.block];
            bool reaches = (fc.weakNeededGen[bc] == ug) ||
                           (compHasIn[bc] && fc.compCyclic[bc]);
            if (!reaches) {
              auto mit = maxInPos.find(oc.block);
              if (mit != maxInPos.end() && oc.pos < mit->second)
                reaches = true;
            }
            if (reaches)
              addUniEdge(entryComps + oc.comp, targetE);
          }
        }
      } else {
        for (const auto &oc : outCalls) {
          const std::size_t bc = fc.comp[oc.block];
          for (const auto &ic : inCalls) {
            if ((oc.block == ic.block && oc.pos < ic.pos) ||
                (fc.comp[ic.block] == bc && fc.compCyclic[bc])) {
              addUniEdge(entryComps + oc.comp, ic.comp);
            }
          }
        }
      }
    }

    if (activeWeakAncCtxId != 0) {
      for (const auto &pr : weakAncCtxs[activeWeakAncCtxId].coreExitToEntry) {
        const std::size_t sFrom = ancPortSlot[pr.first];
        const std::size_t sTo = ancPortSlot[pr.second];
        if (sFrom <= nAct && sTo <= nAct && weakExitElig[sFrom] &&
            weakEntryElig[sTo])
          addUniEdge(entryComps + retComp[sFrom], entryComp[sTo]);
      }
    }

    auto [uniComp, uniComps] = computeSCCs(uniSucc);
    std::vector<std::size_t> uniTotalElig(uniComps, 0);
    for (std::size_t c = 0; c < entryComps; ++c)
      uniTotalElig[uniComp[c]] += entryCompSize[c];
    for (std::size_t r = 0; r < retComps; ++r)
      uniTotalElig[uniComp[entryComps + r]] += retCompSize[r];

    std::vector<NodeID> uniCompNode(uniComps, 0);
    NodeID hubEntry = 0;
    NodeID hubExit = 0;
    std::vector<std::uint8_t> inWeakMultiSCC(nAct, 0);
    std::vector<std::uint8_t> endpointCollapsed(nAct, 0);
    for (std::size_t uc = 0; uc < uniComps; ++uc) {
      if (uniTotalElig[uc] > 1)
        uniCompNode[uc] = addJoin();
    }
    for (std::size_t c = 0; c < entryComps; ++c) {
      const NodeID cn = uniCompNode[uniComp[c]];
      if (!cn)
        continue;
      for (std::size_t i : entryCompMembers[c]) {
        if (i == hubLocalIdx) {
          if (separateHubActive)
            hubEntry = cn;
          continue;
        }
        if (actReused[i] || !weakEntryElig[i])
          continue;
        actEntry[i] = cn;
        inWeakMultiSCC[i] = 1;
      }
    }
    for (std::size_t r = 0; r < retComps; ++r) {
      const NodeID cn = uniCompNode[uniComp[entryComps + r]];
      if (!cn)
        continue;
      for (std::size_t i : retCompMembers[r]) {
        if (i == hubLocalIdx) {
          if (separateHubActive && isTransMod(hubIdx))
            hubExit = cn;
          continue;
        }
        if (actReused[i] || !actNeedExit[i] || !weakExitElig[i])
          continue;
        actExit[i] = cn;
        inWeakMultiSCC[i] = 1;
      }
    }

    // Assign remaining singleton actEntry in reverse entry-SCC topological
    // order (callees before callers) with Theorem F-Dual read-only collapse.
    for (std::size_t idx = entryComps; idx > 0; --idx) {
      const std::size_t c = idx - 1;
      if (uniCompNode[uniComp[c]])
        continue;
      for (std::size_t i : entryCompMembers[c]) {
        if (i == hubLocalIdx) {
          if (separateHubActive && !hubEntry)
            hubEntry = addJoin();
          continue;
        }
        if (actReused[i] || actEntry[i])
          continue;
        const FunctionID f = activeFuncs[i];
        if (univIdOfRep(f) != 0) {
          actEntry[i] = addJoin();
          continue;
        }
        NodeID collapsedEntry = 0;
        const bool noLocalAcc =
            ownAccessesOf(f).empty() &&
            (isUnknownObj ||
             unknownFnAccesses.find(f) == unknownFnAccesses.end());
        if (weakEntryElig[i] && !actNeedExit[i] && noLocalAcc &&
            actUnkMods[i].empty()) {
          bool anyOut = false;
          NodeID uniqueDst = 0;
          bool multipleDst = false;
          auto observeDst = [&](NodeID d) {
            if (!d)
              return;
            if (!uniqueDst)
              uniqueDst = d;
            else if (uniqueDst != d)
              multipleDst = true;
          };
          for (const auto &ur : actUnkReads[i])
            observeDst(ur.summaryNode);
          for (const auto &cs : actCalls[i]) {
            if (cs.hasOut) {
              anyOut = true;
              break;
            }
            if (!cs.hasIn)
              continue;
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee) || isTransRef(callee)) {
                const NodeID ce = actEntry[fnActiveIdx[canon(callee)]];
                if (!ce)
                  multipleDst = true;
                else
                  observeDst(ce);
              } else if (!isUnknownObj && unknownTransRef[callee]) {
                observeDst(unknownReadEntry[callee]);
              }
            }
            if (cs.hasHub) {
              if (hubActive) {
                const NodeID he =
                    hubInUniv ? actEntry[hubTargetIdx] : hubEntry;
                if (!he)
                  multipleDst = true;
                else
                  observeDst(he);
              } else if (!isUnknownObj && unknownTransRef[hubIdx]) {
                observeDst(unknownReadEntry[hubIdx]);
              }
            }
          }
          if (!anyOut && uniqueDst && !multipleDst)
            collapsedEntry = uniqueDst;
        }
        if (collapsedEntry) {
          actEntry[i] = collapsedEntry;
          endpointCollapsed[i] = 1;
        } else {
          actEntry[i] = addJoin();
        }
      }
    }

    // Assign remaining singleton actExit in forward ret-SCC topological
    // order (callees before callers) with Theorem F transit exit collapse.
    for (std::size_t c = 0; c < retComps; ++c) {
      if (uniCompNode[uniComp[entryComps + c]])
        continue;
      for (std::size_t i : retCompMembers[c]) {
        if (i == hubLocalIdx) {
          if (separateHubActive && isTransMod(hubIdx) && !hubExit)
            hubExit = addJoin();
          continue;
        }
        if (actReused[i] || actExit[i])
          continue;
        if (!actNeedExit[i]) {
          actExit[i] = 0;
          continue;
        }
        const FunctionID f = activeFuncs[i];
        if (univIdOfRep(f) != 0) {
          actExit[i] = isTransMod(f) ? actEntry[i] : addJoin();
          continue;
        }
        NodeID collapsedExit = 0;
        if (weakExitElig[i]) {
          const auto &fc = funcCFGs[f];
          bool hasExitLocalStore =
              (!isUnknownObj && fnLocalExitUnkStoreNode[f] != 0);
          if (!hasExitLocalStore) {
            for (AccessID id : ownAccessesOf(f)) {
              const auto &acc = p.accesses_[id];
              if (acc.kind == Program::AccessKind::Store &&
                  fc.canReachExit[blockToLocal[acc.block]]) {
                hasExitLocalStore = true;
                break;
              }
            }
          }
          if (!hasExitLocalStore) {
            for (const auto &um : actUnkMods[i]) {
              if (fc.canReachExit[blockToLocal[p.callSites_[um.site].block]]) {
                hasExitLocalStore = true;
                break;
              }
            }
          }
          if (!hasExitLocalStore) {
            NodeID uniqueExit = 0;
            bool multipleExit = false;
            auto observeExit = [&](NodeID e) {
              if (!e)
                return;
              if (!uniqueExit)
                uniqueExit = e;
              else if (uniqueExit != e)
                multipleExit = true;
            };
            for (const auto &cs : actCalls[i]) {
              if (!cs.hasOut ||
                  !fc.canReachExit[blockToLocal[p.callSites_[cs.site].block]])
                continue;
              for (FunctionID callee : liveCallTargets[cs.site]) {
                if (isTransMod(callee)) {
                  const NodeID cx = actExit[fnActiveIdx[canon(callee)]];
                  if (!cx)
                    multipleExit = true;
                  else
                    observeExit(cx);
                }
              }
              if (cs.hasHub && hubActive && isTransMod(hubIdx)) {
                const NodeID hx = hubInUniv ? actExit[hubTargetIdx] : hubExit;
                if (!hx)
                  multipleExit = true;
                else
                  observeExit(hx);
              }
            }
            if (uniqueExit && !multipleExit)
              collapsedExit = uniqueExit;
          }
        }
        if (collapsedExit) {
          actExit[i] = collapsedExit;
          endpointCollapsed[i] = 1;
        } else {
          actExit[i] = addJoin();
        }
      }
    }

    if (!isUnknownObj && !objCanStrong && !separateHubActive && nAct == 1 &&
        univIdOfRep(activeFuncs[0]) != 0 && lastEmitState[object].valid &&
        lastEmitState[object].funcs.size() == 1) {
      lastEmitState[object].funcs[0].univNode = actEntry[0];
    }

    if (hubInUniv && hubActive) {
      hubEntry = actEntry[hubTargetIdx];
      hubExit = actExit[hubTargetIdx];
    }

    if (separateHubActive) {
      if (!isUnknownObj) {
        ensureHubExtSummary(useUniv);
        std::vector<FunctionID> excludedHubCanon;
        for (std::size_t j = 0; j < nAct; ++j) {
          const FunctionID rep = activeFuncs[j];
          const std::uint32_t u = univIdOfRep(rep);
          bool hasAT = false;
          if (u != 0) {
            for (FunctionID m : univSccs[u].members) {
              if (isAddressTakenFunc[m]) {
                hasAT = true;
                break;
              }
            }
          } else {
            hasAT = isAddressTakenFunc[rep];
          }
          if (!hasAT)
            continue;
          excludedHubCanon.push_back(rep);
          if (hubEntry && actEntry[j] && hubEntry != actEntry[j] &&
              addIndirect(hubEntry, actEntry[j]))
            ++stats.interproceduralAuxEdges;
          if (hubExit && isTransMod(rep) && actExit[j] &&
              actExit[j] != hubExit && addIndirect(actExit[j], hubExit))
            ++stats.interproceduralAuxEdges;
        }
        auto isAtCanonAct = [&](FunctionID ct) -> bool {
          return fnActiveGen[ct] == g;
        };
        wireUnivExtInactive(hubEntry, hubExit, hubExtSummary, nAct,
                            isAtCanonAct, excludedHubCanon);
      } else {
        for (FunctionID callee : addressTakenFuncs) {
          if (!funcReachable[callee])
            continue;
          if (isTransMod(callee) || isTransRef(callee)) {
            const NodeID dst = actEntry[fnActiveIdx[canon(callee)]];
            if (hubEntry != dst && addIndirect(hubEntry, dst))
              ++stats.interproceduralAuxEdges;
          }
          if (hubExit && isTransMod(callee)) {
            const NodeID src = actExit[fnActiveIdx[canon(callee)]];
            if (src && src != hubExit && addIndirect(src, hubExit))
              ++stats.interproceduralAuxEdges;
          }
        }
      }
    }

    // Wire initialStore into entry super-vertices, scopeRoot, and any active
    // super-vertex not reachable from them along active carrying call edges.
    std::vector<std::uint8_t> initReached(nAct, 0);
    std::vector<std::size_t> initReachWork;
    bool hubInitReached = false;
    for (std::size_t i = 0; i < nAct; ++i) {
      const FunctionID rep = activeFuncs[i];
      const std::uint32_t u = univIdOfRep(rep);
      const bool isEntry =
          p.entryFunctions_.count(rep) || (u != 0 && univSccs[u].containsEntry);
      const bool fromAnc = (coreSlotInitFromAnc[i] != 0);
      if (isEntry || (scopeRoot && rep == scopeRoot) || fromAnc) {
        initReached[i] = 1;
        initReachWork.push_back(i);
      }
    }
    if (separateHubActive && coreSlotInitFromAnc[hubLocalIdx] != 0) {
      hubInitReached = true;
      for (std::size_t ci = 0; ci < nAct; ++ci) {
        if (initReached[ci])
          continue;
        const FunctionID rep = activeFuncs[ci];
        const std::uint32_t u = univIdOfRep(rep);
        bool hasAT = false;
        if (u != 0) {
          for (FunctionID m : univSccs[u].members) {
            if (isAddressTakenFunc[m]) {
              hasAT = true;
              break;
            }
          }
        } else {
          hasAT = isAddressTakenFunc[rep];
        }
        if (hasAT) {
          initReached[ci] = 1;
          initReachWork.push_back(ci);
        }
      }
    }
    while (!initReachWork.empty()) {
      const std::size_t idx = initReachWork.back();
      initReachWork.pop_back();
      const FunctionID rep = activeFuncs[idx];
      auto markCalleeInit = [&](FunctionID callee) {
        const FunctionID cc = canon(callee);
        if (cc == hubIdx) {
          if (separateHubActive && !hubInitReached) {
            hubInitReached = true;
            for (FunctionID atFn : addressTakenFuncs) {
              const FunctionID ca = canon(atFn);
              if (fnActiveGen[ca] == g) {
                const std::size_t ci = fnActiveIdx[ca];
                if (!initReached[ci]) {
                  initReached[ci] = 1;
                  initReachWork.push_back(ci);
                }
              }
            }
          }
          return;
        }
        if (fnActiveGen[cc] == g) {
          const std::size_t ci = fnActiveIdx[cc];
          if (!initReached[ci]) {
            initReached[ci] = 1;
            initReachWork.push_back(ci);
          }
        }
      };
      if (univIdOfRep(rep) != 0) {
        const auto &ext = actUnivExt[idx];
        for (std::size_t j : ext.activeRetIdxs) {
          if (!initReached[j]) {
            initReached[j] = 1;
            initReachWork.push_back(j);
          }
        }
        for (std::size_t j : ext.activeNoRetIdxs) {
          if (!initReached[j]) {
            initReached[j] = 1;
            initReachWork.push_back(j);
          }
        }
        if (ext.activeRetHub || ext.activeNoRetHub)
          markCalleeInit(hubIdx);
        for (FunctionID v : ext.extraRetCallees)
          markCalleeInit(v);
        for (FunctionID v : ext.extraNoRetCallees)
          markCalleeInit(v);
        continue;
      }
      for (const auto &cs : actCalls[idx]) {
        for (FunctionID callee : liveCallTargets[cs.site])
          markCalleeInit(callee);
        if (cs.hasHub && hubActive)
          markCalleeInit(hubIdx);
      }
    }

    // Per-computeIndirect deduplication of multi-target elided callIn/callOut
    // wrapper joins so indirect callsites with K callees emit 1 local event
    // instead of K duplicate LocalEvents and K separate storeVals.
    struct NodeVecHash {
      std::size_t operator()(const std::vector<NodeID> &v) const {
        std::uint64_t h = 14695981039346656037ULL;
        for (NodeID x : v)
          h = hashMix(h, x);
        return static_cast<std::size_t>(h);
      }
    };
    std::unordered_map<std::vector<NodeID>, NodeID, NodeVecHash> multiInNodes;
    std::unordered_map<std::vector<NodeID>, NodeID, NodeVecHash> multiOutNodes;

    std::vector<NodeID> rootTargets;
    if (separateHubActive && coreSlotInitFromAnc[hubLocalIdx] != 0 && hubEntry)
      appendUnique(rootTargets, hubEntry);
    for (std::size_t actIdx = 0; actIdx < nAct; ++actIdx) {
      const FunctionID rep = activeFuncs[actIdx];
      const std::uint32_t u = univIdOfRep(rep);
      const bool isEntry =
          p.entryFunctions_.count(rep) || (u != 0 && univSccs[u].containsEntry);
      const bool fromAnc = (coreSlotInitFromAnc[actIdx] != 0);
      if (isEntry || (scopeRoot && rep == scopeRoot) || fromAnc ||
          !initReached[actIdx])
        appendUnique(rootTargets, actEntry[actIdx]);

      // Exact Theorem X emission for a collapsed UnivWeakSCC super-vertex:
      // all `|scc.members|` functions share ONE node `C = actEntry[actIdx]`,
      // wiring O(1) SCC unknown joins, own accesses, deduplicated external
      // callees, and exact local flow ONLY for the tiny set of special
      // functions containing a noreturn-only store (or store in TransRef-only).
      if (u != 0) {
        const auto &scc = univSccs[u];
        const auto &ext = actUnivExt[actIdx];
        const NodeID C = actEntry[actIdx];
        const bool uMod = isTransMod(rep);

        if (uMod && scc.unkStoreJoin && scc.unkStoreJoin != C)
          addIndirect(scc.unkStoreJoin, C);
        if (scc.unkLoadJoin && C != scc.unkLoadJoin)
          addIndirect(C, scc.unkLoadJoin);

        // Identify any special functions in `u` needing exact local flow.
        std::vector<FunctionID> specialFuncs = scc.nonExitUnkFuncs;
        auto markSpecial = [&](FunctionID sf) {
          appendUnique(specialFuncs, sf);
        };
        auto ownIt = univOwnFuncs.find(u);
        if (ownIt != univOwnFuncs.end()) {
          for (FunctionID f : ownIt->second) {
            const auto &fc = funcCFGs[f];
            bool needsLocal = false;
            for (AccessID id : ownAccessesOf(f)) {
              const auto &acc = p.accesses_[id];
              if (acc.kind == Program::AccessKind::Store &&
                  (!uMod || !fc.canReachExit[blockToLocal[acc.block]])) {
                needsLocal = true;
                break;
              }
            }
            if (needsLocal) {
              markSpecial(f);
            } else {
              for (AccessID id : ownAccessesOf(f)) {
                if (savedReuseUnivNode != 0 && savedReusePrevOwnAcc.count(id))
                  continue;
                const auto &acc = p.accesses_[id];
                if (acc.kind == Program::AccessKind::Store) {
                  const NodeID val =
                      acc.sourceIsUnknown ? p.unknownValue() : acc.value;
                  if (val != C)
                    addIndirect(val, C);
                } else {
                  if (C != acc.value)
                    addIndirect(C, acc.value);
                }
              }
            }
          }
        }
        auto checkNonExitExtRetFunc = [&](FunctionID f) {
          const auto &fc = funcCFGs[f];
          for (CallSiteID site : funcCallSites[f]) {
            const auto &cs = p.callSites_[site];
            if (!cs.mayReturn || fc.canReachExit[blockToLocal[cs.block]] ||
                !callSiteCanCarryObject(site))
              continue;
            for (FunctionID v : liveCallTargets[site]) {
              if (fnUnivScc[v] == u)
                continue;
              if (isTransMod(v) ||
                  (!isTransRef(v) && unknownModSummary[v])) {
                markSpecial(f);
                return;
              }
            }
            if (siteCallsUnknownHub[site] && !scc.containsHub) {
              if (isTransMod(hubIdx) ||
                  (!hubActive && unknownModSummary[hubIdx])) {
                markSpecial(f);
                return;
              }
            }
          }
        };
        for (FunctionID f : scc.nonExitExtRetFuncs)
          checkNonExitExtRetFunc(f);
        for (FunctionID f : ext.extraNonExitRetFuncs)
          checkNonExitExtRetFunc(f);

        // Wire active external callees in O(|activeExt|) and all inactive
        // external callees in O((k + 1) log M) via persistent segment tree &
        // deduplicated mod groups.
        std::vector<FunctionID> excludedCanon;
        excludedCanon.reserve(ext.activeRetIdxs.size() +
                              ext.activeNoRetIdxs.size() + 1);
        auto wireActiveIdx = [&](std::size_t j, bool isRet) {
          const FunctionID rj = activeFuncs[j];
          excludedCanon.push_back(rj);
          const NodeID dst = actEntry[j];
          if (dst && dst != C && addIndirect(C, dst))
            ++stats.interproceduralAuxEdges;
          if (isRet && uMod && isTransMod(rj)) {
            const NodeID src = actExit[j];
            if (src && src != C && addIndirect(src, C))
              ++stats.interproceduralAuxEdges;
          }
        };
        for (std::size_t j : ext.activeRetIdxs)
          wireActiveIdx(j, true);
        for (std::size_t j : ext.activeNoRetIdxs)
          wireActiveIdx(j, false);
        if (ext.activeRetHub || ext.activeNoRetHub) {
          excludedCanon.push_back(hubIdx);
          if (hubEntry && hubEntry != C && addIndirect(C, hubEntry))
            ++stats.interproceduralAuxEdges;
          if (ext.activeRetHub && uMod && hubExit && hubExit != C &&
              addIndirect(hubExit, C))
            ++stats.interproceduralAuxEdges;
        }
        auto isCanonActCur = [&](FunctionID ct) -> bool {
          if (ct == hubIdx)
            return separateHubActive;
          return fnActiveGen[ct] == g;
        };
        const auto &extSum =
            globalVisible ? scc.globalExtSummary : scc.alwaysExtSummary;
        wireUnivExtInactive(C, uMod ? C : 0, extSum,
                            nAct + (separateHubActive ? 1 : 0), isCanonActCur,
                            excludedCanon);

        auto wireExtraCallee = [&](FunctionID v, bool isRet) {
          const FunctionID cv = canon(v);
          if (cv == hubIdx) {
            if (separateHubActive) {
              if (hubEntry && hubEntry != C && addIndirect(C, hubEntry))
                ++stats.interproceduralAuxEdges;
              if (isRet && uMod && hubExit && hubExit != C &&
                  addIndirect(hubExit, C))
                ++stats.interproceduralAuxEdges;
            } else {
              if (unknownTransRef[hubIdx] &&
                  addIndirect(C, unknownReadEntry[hubIdx]))
                ++stats.interproceduralAuxEdges;
              if (isRet && uMod && unknownModSummary[hubIdx] &&
                  unknownModSummary[hubIdx] != C)
                addIndirect(unknownModSummary[hubIdx], C);
            }
            return;
          }
          if (isTransMod(cv) || isTransRef(cv)) {
            const NodeID dst = actEntry[fnActiveIdx[cv]];
            if (dst && dst != C && addIndirect(C, dst))
              ++stats.interproceduralAuxEdges;
            if (isRet && uMod && isTransMod(cv)) {
              const NodeID src = actExit[fnActiveIdx[cv]];
              if (src && src != C && addIndirect(src, C))
                ++stats.interproceduralAuxEdges;
            }
          } else {
            if (unknownTransRef[v] && addIndirect(C, unknownReadEntry[v]))
              ++stats.interproceduralAuxEdges;
            if (isRet && uMod && unknownModSummary[v] &&
                unknownModSummary[v] != C)
              addIndirect(unknownModSummary[v], C);
          }
        };
        for (FunctionID v : ext.extraRetCallees)
          wireExtraCallee(v, true);
        for (FunctionID v : ext.extraNoRetCallees)
          wireExtraCallee(v, false);

        // Run exact local weak flow for any special functions in `u`.
        for (FunctionID sf : specialFuncs) {
          std::vector<LocalEvent> events;
          const NodeSet &ownAcc = ownAccessesOf(sf);
          auto pushAcc = [&](AccessID id) {
            const auto &access = p.accesses_[id];
            LocalEvent ev;
            ev.block = blockToLocal[access.block];
            ev.pos = accessEventPos[id];
            ev.phase = 0;
            ev.isStore = (access.kind == Program::AccessKind::Store);
            ev.isStrong = false;
            ev.node = (ev.isStore && access.sourceIsUnknown) ? p.unknownValue()
                                                             : access.value;
            events.push_back(ev);
          };
          for (AccessID id : ownAcc)
            pushAcc(id);
          auto uIt = unknownFnAccesses.find(sf);
          if (uIt != unknownFnAccesses.end()) {
            for (AccessID id : uIt->second) {
              if (!ownAcc.count(id))
                pushAcc(id);
            }
          }
          for (CallSiteID site : funcCallSites[sf]) {
            if (!callSiteCanCarryObject(site))
              continue;
            const auto &cs = p.callSites_[site];
            const LocalBlock callBlock = blockToLocal[cs.block];
            const std::uint32_t pos = callEventPos[site];
            std::vector<NodeID> inT, outS;
            auto handleT = [&](FunctionID v) {
              const FunctionID cv = canon(v);
              if (cv == rep) {
                inT.push_back(C);
                return;
              }
              if (cv == hubIdx) {
                if (separateHubActive) {
                  if (hubEntry)
                    inT.push_back(hubEntry);
                  if (cs.mayReturn && hubExit)
                    outS.push_back(hubExit);
                } else {
                  if (unknownTransRef[hubIdx])
                    inT.push_back(unknownReadEntry[hubIdx]);
                  if (cs.mayReturn && unknownModSummary[hubIdx])
                    outS.push_back(unknownModSummary[hubIdx]);
                }
                return;
              }
              if (isTransMod(cv) || isTransRef(cv)) {
                inT.push_back(actEntry[fnActiveIdx[cv]]);
                if (cs.mayReturn && isTransMod(cv) && actExit[fnActiveIdx[cv]])
                  outS.push_back(actExit[fnActiveIdx[cv]]);
              } else {
                if (unknownTransRef[v])
                  inT.push_back(unknownReadEntry[v]);
                if (cs.mayReturn && unknownModSummary[v])
                  outS.push_back(unknownModSummary[v]);
              }
            };
            for (FunctionID v : liveCallTargets[site])
              handleT(v);
            if (siteCallsUnknownHub[site] && unknownHubActive)
              handleT(hubIdx);
            if (inT.size() > 1) {
              std::sort(inT.begin(), inT.end());
              inT.erase(std::unique(inT.begin(), inT.end()), inT.end());
            }
            if (inT.size() == 1) {
              events.push_back({callBlock, pos, 0, false, false, inT[0], 0});
            } else if (inT.size() >= 2) {
              auto [it, ins] = multiInNodes.emplace(inT, 0);
              if (ins) {
                const NodeID jn = addJoin();
                it->second = jn;
                for (NodeID dst : inT)
                  addIndirect(jn, dst);
              }
              events.push_back(
                  {callBlock, pos, 0, false, false, it->second, 0});
            }
            if (outS.size() > 1) {
              std::sort(outS.begin(), outS.end());
              outS.erase(std::unique(outS.begin(), outS.end()), outS.end());
            }
            if (outS.size() == 1) {
              events.push_back({callBlock, pos, 1, true, false, outS[0], 0});
            } else if (outS.size() >= 2) {
              auto [it, ins] = multiOutNodes.emplace(outS, 0);
              if (ins) {
                const NodeID jn = addJoin();
                it->second = jn;
                for (NodeID src : outS)
                  addIndirect(src, jn);
              }
              events.push_back(
                  {callBlock, pos, 1, true, false, it->second, 0});
            }
          }
          if (uMod) {
            events.push_back({funcCFGs[sf].exitLocal,
                              std::numeric_limits<std::uint32_t>::max(), 2,
                              false, false, C, 0});
          }
          computeLocalFlowForFunction(sf, C, events, /*hasAnyStrong=*/false);
        }
        continue;
      }

      const FunctionID f = rep;
      const bool elideWrappers = !objCanStrong || inWeakMultiSCC[actIdx] ||
                                 endpointCollapsed[actIdx] ||
                                 (weakEntryElig[actIdx] && nAct > 16);

      auto &summaries = actCalls[actIdx];
      auto &unkMods = actUnkMods[actIdx];
      auto &unkReads = actUnkReads[actIdx];

      if (actReused[actIdx]) {
        if (!elideWrappers) {
          auto &cached = objectFuncCache[object][f];
          for (const auto &cs : summaries) {
            if (cs.hasIn) {
              const NodeID inNode = cached.callInNodes.at(cs.site);
              for (FunctionID callee : liveCallTargets[cs.site]) {
                if (isTransMod(callee) || isTransRef(callee)) {
                  if (addIndirect(inNode, actEntry[fnActiveIdx[canon(callee)]]))
                    ++stats.interproceduralAuxEdges;
                } else if (!isUnknownObj && unknownTransRef[callee]) {
                  if (addIndirect(inNode, unknownReadEntry[callee]))
                    ++stats.interproceduralAuxEdges;
                }
              }
              if (cs.hasHub) {
                if (hubActive) {
                  if (addIndirect(inNode, hubEntry))
                    ++stats.interproceduralAuxEdges;
                } else if (!isUnknownObj && unknownTransRef[hubIdx]) {
                  if (addIndirect(inNode, unknownReadEntry[hubIdx]))
                    ++stats.interproceduralAuxEdges;
                }
              }
            }
            if (cs.hasOut) {
              const NodeID outNode = cached.callOutNodes.at(cs.site);
              for (FunctionID callee : liveCallTargets[cs.site]) {
                if (isTransMod(callee)) {
                  const NodeID cx = actExit[fnActiveIdx[canon(callee)]];
                  if (cx && addIndirect(cx, outNode))
                    ++stats.interproceduralAuxEdges;
                }
              }
              if (cs.hasHub && hubExit) {
                if (addIndirect(hubExit, outNode))
                  ++stats.interproceduralAuxEdges;
              }
            }
          }
        }
        continue;
      }

      // Exact Theorem 3A fast path for non-UnivWeakSCC functions whose entry
      // and exit merged into the same unified SCC node `C`.
      if (weakEntryElig[actIdx] && actEntry[actIdx] != 0 &&
          actEntry[actIdx] == actExit[actIdx]) {
        const auto &fc = funcCFGs[f];
        const NodeSet &ownAcc = ownAccessesOf(f);
        bool allStoresReachExit = (isUnknownObj || !fnHasNonExitUnkStore[f]);
        if (allStoresReachExit) {
          for (AccessID id : ownAcc) {
            const auto &acc = p.accesses_[id];
            if (acc.kind == Program::AccessKind::Store &&
                !fc.canReachExit[blockToLocal[acc.block]]) {
              allStoresReachExit = false;
              break;
            }
          }
        }
        if (allStoresReachExit) {
          for (const auto &um : unkMods) {
            if (!fc.canReachExit[blockToLocal[p.callSites_[um.site].block]]) {
              allStoresReachExit = false;
              break;
            }
          }
        }
        if (allStoresReachExit) {
          for (const auto &cs : summaries) {
            if (cs.hasOut &&
                !fc.canReachExit[blockToLocal[p.callSites_[cs.site].block]]) {
              allStoresReachExit = false;
              break;
            }
          }
        }
        if (allStoresReachExit) {
          const NodeID C = actEntry[actIdx];
          for (AccessID id : ownAcc) {
            const auto &acc = p.accesses_[id];
            if (acc.kind == Program::AccessKind::Store) {
              const NodeID val =
                  acc.sourceIsUnknown ? p.unknownValue() : acc.value;
              if (val != C)
                addIndirect(val, C);
            } else {
              if (C != acc.value)
                addIndirect(C, acc.value);
            }
          }
          if (!isUnknownObj) {
            if (fnLocalExitUnkStoreNode[f] && fnLocalExitUnkStoreNode[f] != C)
              addIndirect(fnLocalExitUnkStoreNode[f], C);
            if (fnLocalUnkLoadNode[f] && C != fnLocalUnkLoadNode[f])
              addIndirect(C, fnLocalUnkLoadNode[f]);
          }
          for (const auto &um : unkMods) {
            if (um.summaryNode != C)
              addIndirect(um.summaryNode, C);
          }
          for (const auto &ur : unkReads) {
            if (C != ur.summaryNode && addIndirect(C, ur.summaryNode))
              ++stats.interproceduralAuxEdges;
          }
          for (const auto &cs : summaries) {
            if (cs.hasIn) {
              for (FunctionID callee : liveCallTargets[cs.site]) {
                NodeID dst = 0;
                if (isTransMod(callee) || isTransRef(callee))
                  dst = actEntry[fnActiveIdx[canon(callee)]];
                else if (!isUnknownObj && unknownTransRef[callee])
                  dst = unknownReadEntry[callee];
                if (dst && dst != C && addIndirect(C, dst))
                  ++stats.interproceduralAuxEdges;
              }
              if (cs.hasHub) {
                NodeID dst = 0;
                if (hubActive)
                  dst = hubEntry;
                else if (!isUnknownObj && unknownTransRef[hubIdx])
                  dst = unknownReadEntry[hubIdx];
                if (dst && dst != C && addIndirect(C, dst))
                  ++stats.interproceduralAuxEdges;
              }
            }
            if (cs.hasOut) {
              for (FunctionID callee : liveCallTargets[cs.site]) {
                if (isTransMod(callee)) {
                  const NodeID src = actExit[fnActiveIdx[canon(callee)]];
                  if (src && src != C && addIndirect(src, C))
                    ++stats.interproceduralAuxEdges;
                }
              }
              if (cs.hasHub && hubExit && hubExit != C) {
                if (addIndirect(hubExit, C))
                  ++stats.interproceduralAuxEdges;
              }
            }
          }
          if (objCanStrong) {
            ObjectFuncCacheEntry cacheEntry;
            cacheEntry.elidedWrappers = true;
            cacheEntry.calls = summaries;
            objectFuncCache[object][f] = std::move(cacheEntry);
          }
          continue;
        }
      }

      // Build local event list for function f.
      std::vector<LocalEvent> events;
      const NodeSet &ownAcc = ownAccessesOf(f);
      const NodeSet &str = ownStrongOf(f);
      bool hasAnyStrong = !str.empty();
      auto appendAccessEvent = [&](AccessID id, bool checkStrong) {
        const auto &access = p.accesses_[id];
        LocalEvent ev;
        ev.block = blockToLocal[access.block];
        ev.pos = accessEventPos[id];
        ev.phase = 0;
        ev.isStore = (access.kind == Program::AccessKind::Store);
        ev.isStrong = checkStrong && ev.isStore && (str.count(id) != 0);
        ev.node = (ev.isStore && access.sourceIsUnknown) ? p.unknownValue()
                                                         : access.value;
        events.push_back(ev);
      };
      for (AccessID id : ownAcc)
        appendAccessEvent(id, !str.empty());
      if (!isUnknownObj) {
        auto uIt = unknownFnAccesses.find(f);
        if (uIt != unknownFnAccesses.end()) {
          for (AccessID id : uIt->second) {
            if (!ownAcc.count(id))
              appendAccessEvent(id, false);
          }
        }
      }

      ObjectFuncCacheEntry cacheEntry;
      if (!elideWrappers) {
        cacheEntry.accesses = buildMergedFuncAccesses(f);
        cacheEntry.strong = str;
        cacheEntry.calls = summaries;
        cacheEntry.unknownMods = unkMods;
        cacheEntry.unknownReads = unkReads;
        cacheEntry.needExit = (actExit[actIdx] != 0);
        cacheEntry.entryNode = actEntry[actIdx];
        cacheEntry.exitNode = actExit[actIdx];
      }

      for (const auto &um : unkMods) {
        const LocalBlock callBlock = blockToLocal[p.callSites_[um.site].block];
        const std::uint32_t pos = callEventPos[um.site];
        events.push_back({callBlock, pos, 1, true, false, um.summaryNode, 0});
      }

      for (const auto &ur : unkReads) {
        const LocalBlock callBlock = blockToLocal[p.callSites_[ur.site].block];
        const std::uint32_t pos = callEventPos[ur.site];
        events.push_back({callBlock, pos, 0, false, false, ur.summaryNode, 0});
        ++stats.interproceduralAuxEdges;
      }

      for (const auto &cs : summaries) {
        const LocalBlock callBlock = blockToLocal[p.callSites_[cs.site].block];
        const std::uint32_t pos = callEventPos[cs.site];
        if (cs.hasIn) {
          if (elideWrappers) {
            std::vector<NodeID> inTargets;
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee) || isTransRef(callee))
                inTargets.push_back(actEntry[fnActiveIdx[canon(callee)]]);
              else if (!isUnknownObj && unknownTransRef[callee])
                inTargets.push_back(unknownReadEntry[callee]);
            }
            if (cs.hasHub) {
              if (hubActive)
                inTargets.push_back(hubEntry);
              else if (!isUnknownObj && unknownTransRef[hubIdx])
                inTargets.push_back(unknownReadEntry[hubIdx]);
            }
            if (inTargets.size() > 1) {
              std::sort(inTargets.begin(), inTargets.end());
              inTargets.erase(std::unique(inTargets.begin(), inTargets.end()),
                              inTargets.end());
            }
            if (inTargets.size() == 1) {
              events.push_back(
                  {callBlock, pos, 0, false, false, inTargets[0], 0});
              ++stats.interproceduralAuxEdges;
            } else if (inTargets.size() >= 2) {
              auto [it, inserted] = multiInNodes.emplace(inTargets, 0);
              if (inserted) {
                const NodeID sharedIn = addJoin();
                it->second = sharedIn;
                for (NodeID dst : inTargets) {
                  if (addIndirect(sharedIn, dst))
                    ++stats.interproceduralAuxEdges;
                }
              }
              events.push_back(
                  {callBlock, pos, 0, false, false, it->second, 0});
            }
          } else {
            const NodeID inNode = addJoin();
            cacheEntry.callInNodes[cs.site] = inNode;
            events.push_back({callBlock, pos, 0, false, false, inNode, 0});
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee) || isTransRef(callee)) {
                if (addIndirect(inNode, actEntry[fnActiveIdx[canon(callee)]]))
                  ++stats.interproceduralAuxEdges;
              } else if (!isUnknownObj && unknownTransRef[callee]) {
                if (addIndirect(inNode, unknownReadEntry[callee]))
                  ++stats.interproceduralAuxEdges;
              }
            }
            if (cs.hasHub) {
              if (hubActive) {
                if (addIndirect(inNode, hubEntry))
                  ++stats.interproceduralAuxEdges;
              } else if (!isUnknownObj && unknownTransRef[hubIdx]) {
                if (addIndirect(inNode, unknownReadEntry[hubIdx]))
                  ++stats.interproceduralAuxEdges;
              }
            }
          }
        }
        if (cs.hasOut) {
          if (elideWrappers) {
            std::vector<NodeID> outSources;
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee)) {
                const NodeID cx = actExit[fnActiveIdx[canon(callee)]];
                if (cx)
                  outSources.push_back(cx);
              }
            }
            if (cs.hasHub && hubExit)
              outSources.push_back(hubExit);
            if (outSources.size() > 1) {
              std::sort(outSources.begin(), outSources.end());
              outSources.erase(
                  std::unique(outSources.begin(), outSources.end()),
                  outSources.end());
            }
            if (outSources.size() == 1) {
              events.push_back(
                  {callBlock, pos, 1, true, false, outSources[0], 0});
              ++stats.interproceduralAuxEdges;
            } else if (outSources.size() >= 2) {
              auto [it, inserted] = multiOutNodes.emplace(outSources, 0);
              if (inserted) {
                const NodeID sharedOut = addJoin();
                it->second = sharedOut;
                for (NodeID src : outSources) {
                  if (addIndirect(src, sharedOut))
                    ++stats.interproceduralAuxEdges;
                }
              }
              events.push_back(
                  {callBlock, pos, 1, true, false, it->second, 0});
            }
          } else {
            const NodeID outNode = addJoin();
            cacheEntry.callOutNodes[cs.site] = outNode;
            if (cs.outStrong)
              hasAnyStrong = true;
            events.push_back(
                {callBlock, pos, 1, true, cs.outStrong, outNode, 0});
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee)) {
                const NodeID cx = actExit[fnActiveIdx[canon(callee)]];
                if (cx && addIndirect(cx, outNode))
                  ++stats.interproceduralAuxEdges;
              }
            }
            if (cs.hasHub && hubExit) {
              if (addIndirect(hubExit, outNode))
                ++stats.interproceduralAuxEdges;
            }
          }
        }
      }

      if (actExit[actIdx]) {
        events.push_back({funcCFGs[f].exitLocal,
                          std::numeric_limits<std::uint32_t>::max(), 2, false,
                          false, actExit[actIdx], 0});
      }

      computeLocalFlowForFunction(f, actEntry[actIdx], events, hasAnyStrong);

      if (summaries.empty() && unkMods.empty() && unkReads.empty() &&
          actExit[actIdx] == 0) {
        flowTemplates.emplace(
            LocalTemplateKey{f, buildMergedFuncAccesses(f), str},
            actEntry[actIdx]);
      }
      if (!elideWrappers) {
        objectFuncCache[object][f] = std::move(cacheEntry);
      } else if (objCanStrong) {
        cacheEntry.elidedWrappers = true;
        cacheEntry.calls = summaries;
        objectFuncCache[object][f] = std::move(cacheEntry);
      }
    }

    // Wire Theorem Y `WeakAncestorContext` summary ports into `CORE`.
    if (activeWeakAncCtxId != 0) {
      const auto &ctx = weakAncCtxs[activeWeakAncCtxId];
      auto slotEntryNode = [&](std::size_t slot) -> NodeID {
        return (slot == hubLocalIdx) ? hubEntry : actEntry[slot];
      };
      auto slotExitNode = [&](std::size_t slot) -> NodeID {
        return (slot == hubLocalIdx) ? hubExit : actExit[slot];
      };
      for (std::size_t pi = 0; pi < ctx.coreFuncs.size(); ++pi) {
        const std::size_t slot = ancPortSlot[pi];
        if (slot > nAct)
          continue;
        const NodeID en = slotEntryNode(slot);
        const NodeID ex = slotExitNode(slot);
        if (ctx.unkSrcToCoreEntry[pi] && en && ctx.unkSrcToCoreEntry[pi] != en)
          addIndirect(ctx.unkSrcToCoreEntry[pi], en);
        if (ctx.coreExitToUnkSnk[pi] && ex && ex != ctx.coreExitToUnkSnk[pi])
          addIndirect(ex, ctx.coreExitToUnkSnk[pi]);
      }
      for (const auto &pr : ctx.coreExitToEntry) {
        const std::size_t sFrom = ancPortSlot[pr.first];
        const std::size_t sTo = ancPortSlot[pr.second];
        if (sFrom <= nAct && sTo <= nAct) {
          const NodeID src = slotExitNode(sFrom);
          const NodeID dst = slotEntryNode(sTo);
          if (src && dst && src != dst)
            addIndirect(src, dst);
        }
      }
      if (ctx.initToUnkSnk)
        appendUnique(rootTargets, ctx.initToUnkSnk);
    } else if (!isUnknownObj && globalVisible && !unknownDirectRefFuncs.empty()) {
      for (FunctionID entry : p.entryFunctions_) {
        if (funcReachable[entry] && fnActiveGen[canon(entry)] != g &&
            unknownTransRef[entry]) {
          appendUnique(rootTargets, unknownReadEntry[entry]);
        }
      }
    }

    if (useGlobalWeakCache) {
      NodeID rootInitNode = 0;
      if (rootTargets.size() == 1) {
        rootInitNode = rootTargets[0];
      } else if (rootTargets.size() > 1) {
        rootInitNode = addJoin();
        for (NodeID t : rootTargets)
          addIndirect(rootInitNode, t);
      }
      if (rootInitNode &&
          p.initializerKinds_[object] != Program::InitializerKind::Empty) {
        addIndirect(p.objects_[object].initialStore, rootInitNode);
      }
      globalWeakFlowCache.emplace(
          GlobalWeakFlowKey{callGraphVersion,
                            globalVisible ? 0ULL : alwaysCarryVersion,
                            unknownAccessVersion, curUnivBuild, globalVisible,
                            sig.accesses},
          GlobalWeakFlowEntry{rootInitNode, std::move(activeFuncs),
                              activeWeakAncCtxId});
    } else if (p.initializerKinds_[object] != Program::InitializerKind::Empty) {
      for (NodeID t : rootTargets)
        addIndirect(p.objects_[object].initialStore, t);
    }
  }

  //===--------------------------------------------------------------------===//
  // Local scalar cycle weakening (replaces global weak fallback)
  //===--------------------------------------------------------------------===//

  void weakenObjectLocally(ObjectID object, const char *reason) {
    if (!forcedWeak[object]) {
      forcedWeak[object] = true;
      stats.usedWeakFallback = true;
      ++stats.weakFallbackObjects;
      if (stats.fallbackReason.empty())
        stats.fallbackReason = reason;
    }
  }

  // When all remaining pending objects are strong singletons and blocked,
  // either (a) process a singleton whose empty escape pointers/calls are
  // provably unreachable from any pending object's loads or unresolved calls
  // (preserving strong updates and acyclic escape order), or (b) weaken only
  // the minimal scalar escape cycle / unknown-escaping singletons (including
  // implicit dependencies through currently-empty loads of pending singletons).
  void breakBlockedSingletons() {
    // Compute the set of VFG nodes that can still receive new objects from
    // loads of pending objects or unresolved indirect calls. Use O(1) dense
    // vectors and a single virtual hub for address-taken formals.
    const NodeID numNodes = static_cast<NodeID>(adj.size());
    const NodeID hubMay = numNodes;
    std::vector<bool> mayPopulate(numNodes + 1, false);
    std::vector<NodeID> mayWork;
    std::unordered_map<NodeID, NodeSet> loadSeedObjs;
    bool hubFormalsExpanded = false;

    auto seedMayNode = [&](NodeID node) {
      if (node && !mayPopulate[node]) {
        mayPopulate[node] = true;
        mayWork.push_back(node);
      }
    };

    for (ObjectID dep : pending) {
      flushPendingAccesses(dep);
      for (AccessID id : activeAccesses[dep]) {
        const auto &access = p.accesses_[id];
        if (!blockReachable[access.block] ||
            access.kind != Program::AccessKind::Load)
          continue;
        loadSeedObjs[access.value].insertBit(dep);
        seedMayNode(access.value);
      }
    }

    const NodeID numProg = static_cast<NodeID>(p.edges_.size());
    while (!mayWork.empty()) {
      const NodeID u = mayWork.back();
      mayWork.pop_back();
      if (u == hubMay) {
        if (!hubFormalsExpanded) {
          hubFormalsExpanded = true;
          for (FunctionID f : addressTakenFuncs) {
            for (NodeID formal : p.functions_[f].formalParams)
              seedMayNode(formal);
          }
        }
        continue;
      }
      for (NodeID v : adj[u])
        seedMayNode(v);
      if (u < numProg) {
        for (AccessID id : uses[u]) {
          const auto &access = p.accesses_[id];
          if (blockReachable[access.block] &&
              access.kind == Program::AccessKind::Load) {
            seedMayNode(access.value);
          }
        }
        if (!indirectCallUses[u].empty()) {
          bool anyReachSite = false;
          for (CallSiteID site : indirectCallUses[u]) {
            const auto &cs = p.callSites_[site];
            if (!blockReachable[cs.block])
              continue;
            anyReachSite = true;
            if (cs.result)
              seedMayNode(cs.result);
          }
          if (anyReachSite)
            seedMayNode(hubMay);
        }
      }
    }

    std::vector<std::uint64_t> objVisGen(p.objects_.size(), 0);
    std::uint64_t curVisGen = 0;

    auto isProvablyDeadEmptyOnly = [&](ObjectID obj) -> bool {
      if (inspectEscapeBlock(obj) != EscapeBlockKind::EmptyPointerOrCall)
        return false;
      const std::uint64_t vg = ++curVisGen;
      std::vector<ObjectID> work{obj};
      objVisGen[obj] = vg;
      while (!work.empty()) {
        const ObjectID cur = work.back();
        work.pop_back();
        for (ObjectID dep : initialEscapeObjects[cur]) {
          if (p.objects_[dep].memory && objVisGen[dep] != vg) {
            objVisGen[dep] = vg;
            work.push_back(dep);
          }
        }
        for (NodeID ptr : getEscapePointers(cur)) {
          const auto &targets = ptsOf(ptr);
          if (targets.empty()) {
            if (mayPopulate[ptr])
              return false;
            continue;
          }
          for (ObjectID target : targets) {
            if (p.objects_[target].memory && objVisGen[target] != vg) {
              objVisGen[target] = vg;
              work.push_back(target);
            }
          }
        }
        for (CallSiteID site : escapedIndirectCalls[cur]) {
          if (liveCallTargets[site].empty() && !liveCallExternal[site]) {
            if (mayPopulate[p.callSites_[site].calleePointer])
              return false;
          }
        }
      }
      return true;
    };

    // (a) Batch ALL pending singletons whose blocking empty pointers/calls are
    // provably permanently empty (unreachable from any pending object's loads
    // or unresolved calls). Because each has zero pending-object dependencies,
    // they are mutually independent and preserve strong updates.
    std::vector<ObjectID> deadBatch;
    for (ObjectID obj : pending) {
      if (isProvablyDeadEmptyOnly(obj))
        deadBatch.push_back(obj);
    }
    if (!deadBatch.empty()) {
      for (ObjectID obj : deadBatch) {
        pending.erase(obj);
        processed[obj] = true;
        previous[obj] = signature(obj);
      }
      for (ObjectID obj : deadBatch)
        computeIndirect(obj, previous[obj]);
      return;
    }

    // (b) Build the exact bipartite escape graph `{Objects} U {EscapePtrs}`
    // reachable from pending singletons, and find minimal sink SCCs containing
    // pending singletons in O(|V| + |E|) time (mathematically identical to the
    // sink SCCs of the transitive induced graph on `pending`).
    std::vector<ObjectID> nodes(pending.begin(), pending.end());
    const std::size_t n = nodes.size();
    std::unordered_map<ObjectID, std::size_t> objToIdx;
    objToIdx.reserve(n * 4 + 1);
    std::vector<ObjectID> objList;
    objList.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
      objToIdx[nodes[i]] = i;
      objList.push_back(nodes[i]);
    }

    // Discover all reachable objects and escape pointers, plus distinct empty
    // pointers / unresolved callee pointers in `mayPopulate`.
    std::vector<NodeID> ptrList;
    std::unordered_map<NodeID, std::size_t> ptrToIdx;
    std::vector<NodeID> neededEmptyNodes;
    std::vector<bool> isNeededEmpty(numNodes + 1, false);
    {
      std::size_t cursor = 0;
      auto internObj = [&](ObjectID o) {
        if (!p.objects_[o].memory || o == p.unknownObject())
          return;
        auto [it, inserted] = objToIdx.emplace(o, objList.size());
        if (inserted)
          objList.push_back(o);
      };
      while (cursor < objList.size()) {
        const ObjectID cur = objList[cursor++];
        for (ObjectID dep : initialEscapeObjects[cur])
          internObj(dep);
        for (NodeID ptr : getEscapePointers(cur)) {
          auto [pit, pinserted] = ptrToIdx.emplace(ptr, ptrList.size());
          if (pinserted) {
            ptrList.push_back(ptr);
            const auto &ptrPts = ptsOf(ptr);
            if (ptrPts.empty()) {
              if (mayPopulate[ptr] && !isNeededEmpty[ptr]) {
                isNeededEmpty[ptr] = true;
                neededEmptyNodes.push_back(ptr);
              }
            } else {
              for (ObjectID target : ptrPts)
                internObj(target);
            }
          }
        }
        for (CallSiteID site : escapedIndirectCalls[cur]) {
          if (liveCallTargets[site].empty() && !liveCallExternal[site]) {
            const NodeID cp = p.callSites_[site].calleePointer;
            if (mayPopulate[cp] && !isNeededEmpty[cp]) {
              isNeededEmpty[cp] = true;
              neededEmptyNodes.push_back(cp);
            }
          }
        }
      }
    }

    // Compute deduplicated `feederPool` (dense pending-singleton indices
    // 0..n-1) for all needed empty pointers in ONE single-pass Kosaraju-SCC
    // topological sweep over the backward-sliced `canReachNeeded` subgraph.
    // Hash-consing identical feeder sets avoids O(|VFG| * |Pending|) memory
    // spikes, and wiring each distinct `feederPool` slot as an intermediate
    // hub vertex in `bSucc`/`bPred` avoids O(|EmptyPtrs| * |Pending|) edges.
    std::vector<NodeSet> feederPool(1); // slot 0 = empty set
    std::unordered_map<NodeID, std::uint32_t> neededFeederId;
    if (!neededEmptyNodes.empty()) {
      std::vector<std::vector<NodeID>> revMayEdges(numNodes + 1);
      for (NodeID u = 1; u < numNodes; ++u) {
        if (!mayPopulate[u])
          continue;
        for (NodeID v : adj[u])
          if (mayPopulate[v])
            revMayEdges[v].push_back(u);
        if (u < numProg) {
          for (AccessID id : uses[u]) {
            const auto &access = p.accesses_[id];
            if (blockReachable[access.block] &&
                access.kind == Program::AccessKind::Load &&
                mayPopulate[access.value]) {
              revMayEdges[access.value].push_back(u);
            }
          }
          if (!indirectCallUses[u].empty()) {
            bool anyReachSite = false;
            for (CallSiteID site : indirectCallUses[u]) {
              const auto &cs = p.callSites_[site];
              if (!blockReachable[cs.block])
                continue;
              anyReachSite = true;
              if (cs.result && mayPopulate[cs.result])
                revMayEdges[cs.result].push_back(u);
            }
            if (anyReachSite && mayPopulate[hubMay])
              revMayEdges[hubMay].push_back(u);
          }
        }
      }
      if (mayPopulate[hubMay]) {
        for (FunctionID f : addressTakenFuncs) {
          for (NodeID formal : p.functions_[f].formalParams)
            if (formal && mayPopulate[formal])
              revMayEdges[formal].push_back(hubMay);
        }
      }

      std::vector<bool> canReachNeeded(numNodes + 1, false);
      std::vector<NodeID> bWork = neededEmptyNodes;
      for (NodeID u : neededEmptyNodes)
        canReachNeeded[u] = true;
      while (!bWork.empty()) {
        const NodeID u = bWork.back();
        bWork.pop_back();
        for (NodeID pred : revMayEdges[u]) {
          if (!canReachNeeded[pred]) {
            canReachNeeded[pred] = true;
            bWork.push_back(pred);
          }
        }
      }

      std::vector<NodeID> sliceNodes;
      std::vector<std::uint32_t> sliceIdx(
          numNodes + 1, std::numeric_limits<std::uint32_t>::max());
      for (NodeID u = 1; u <= numNodes; ++u) {
        if (canReachNeeded[u]) {
          sliceIdx[u] = static_cast<std::uint32_t>(sliceNodes.size());
          sliceNodes.push_back(u);
        }
      }
      const std::size_t nSlice = sliceNodes.size();
      std::vector<std::vector<std::uint32_t>> sliceSucc(nSlice),
          slicePred(nSlice);
      for (std::size_t i = 0; i < nSlice; ++i) {
        const NodeID v = sliceNodes[i];
        for (NodeID pred : revMayEdges[v]) {
          if (canReachNeeded[pred]) {
            const std::uint32_t pi = sliceIdx[pred];
            slicePred[i].push_back(pi);
            sliceSucc[pi].push_back(static_cast<std::uint32_t>(i));
          }
        }
      }
      std::vector<std::vector<NodeID>>().swap(revMayEdges);

      // Kosaraju SCC indices are in source-to-sink topological order.
      auto [sComp, numSComps] = computeSCCs(sliceSucc, slicePred);
      std::vector<std::vector<std::uint32_t>> sCompMembers(numSComps);
      for (std::size_t i = 0; i < nSlice; ++i)
        sCompMembers[sComp[i]].push_back(static_cast<std::uint32_t>(i));
      std::vector<std::vector<std::uint32_t>>().swap(sliceSucc);

      std::unordered_map<std::size_t, NodeSet> compSeeds;
      for (const auto &[seedNode, objs] : loadSeedObjs) {
        if (!canReachNeeded[seedNode])
          continue;
        const std::size_t sc = sComp[sliceIdx[seedNode]];
        auto &seedSet = compSeeds[sc];
        for (ObjectID dep : objs) {
          auto it = objToIdx.find(dep);
          if (it != objToIdx.end() && it->second < n)
            seedSet.insertBit(static_cast<std::uint32_t>(it->second));
        }
      }

      std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>
          feederIntern;
      auto internFeeder = [&](NodeSet &&s) -> std::uint32_t {
        if (s.empty())
          return 0;
        const std::uint64_t h = s.hash();
        auto &bucket = feederIntern[h];
        for (std::uint32_t id : bucket) {
          if (feederPool[id] == s)
            return id;
        }
        const std::uint32_t id = static_cast<std::uint32_t>(feederPool.size());
        feederPool.push_back(std::move(s));
        bucket.push_back(id);
        return id;
      };

      std::vector<std::uint32_t> compFeederId(numSComps, 0);
      std::vector<std::uint32_t> predIds;
      for (std::size_t c = 0; c < numSComps; ++c) {
        predIds.clear();
        for (std::uint32_t i : sCompMembers[c]) {
          for (std::uint32_t pi : slicePred[i]) {
            const std::size_t pc = sComp[pi];
            if (pc != c) {
              const std::uint32_t fid = compFeederId[pc];
              if (fid != 0)
                predIds.push_back(fid);
            }
          }
        }
        if (predIds.size() > 1) {
          std::sort(predIds.begin(), predIds.end());
          predIds.erase(std::unique(predIds.begin(), predIds.end()),
                        predIds.end());
        }
        auto seedIt = compSeeds.find(c);
        const bool hasSeed =
            (seedIt != compSeeds.end() && !seedIt->second.empty());
        if (predIds.empty() && !hasSeed) {
          compFeederId[c] = 0;
        } else if (predIds.size() == 1 && !hasSeed) {
          compFeederId[c] = predIds[0];
        } else {
          NodeSet merged;
          if (!predIds.empty()) {
            merged = feederPool[predIds[0]];
            for (std::size_t k = 1; k < predIds.size(); ++k)
              merged.unite(feederPool[predIds[k]]);
          }
          if (hasSeed)
            merged.unite(seedIt->second);
          compFeederId[c] = internFeeder(std::move(merged));
        }
      }
      for (NodeID u : neededEmptyNodes) {
        const std::uint32_t fid = compFeederId[sComp[sliceIdx[u]]];
        if (fid != 0)
          neededFeederId.emplace(u, fid);
      }
    }

    // Compact `feederPool` down to only slots actually referenced by
    // `neededFeederId` so `bSucc`/`bPred` only materialize live hub vertices.
    std::vector<std::uint32_t> activeFeederMap(feederPool.size(), 0);
    std::vector<std::uint32_t> activeFeederSlots;
    for (const auto &kv : neededFeederId) {
      if (activeFeederMap[kv.second] == 0) {
        activeFeederMap[kv.second] =
            static_cast<std::uint32_t>(activeFeederSlots.size() + 1);
        activeFeederSlots.push_back(kv.second);
      }
    }

    const std::size_t nObj = objList.size();
    const std::size_t feederBase = nObj + ptrList.size();
    const std::size_t totalV = feederBase + activeFeederSlots.size();
    std::vector<std::vector<std::size_t>> bSucc(totalV), bPred(totalV);
    auto addBEdge = [&](std::size_t u, std::size_t v) {
      if (u != v) {
        bSucc[u].push_back(v);
        bPred[v].push_back(u);
      }
    };
    for (std::size_t fi = 0; fi < activeFeederSlots.size(); ++fi) {
      const std::size_t hubV = feederBase + fi;
      for (std::uint32_t j : feederPool[activeFeederSlots[fi]])
        addBEdge(hubV, j);
    }
    for (std::size_t oi = 0; oi < nObj; ++oi) {
      const ObjectID cur = objList[oi];
      for (ObjectID dep : initialEscapeObjects[cur]) {
        auto it = objToIdx.find(dep);
        if (it != objToIdx.end())
          addBEdge(oi, it->second);
      }
      for (NodeID ptr : getEscapePointers(cur)) {
        auto pit = ptrToIdx.find(ptr);
        if (pit != ptrToIdx.end())
          addBEdge(oi, nObj + pit->second);
      }
      for (CallSiteID site : escapedIndirectCalls[cur]) {
        if (liveCallTargets[site].empty() && !liveCallExternal[site]) {
          const NodeID cp = p.callSites_[site].calleePointer;
          auto fit = neededFeederId.find(cp);
          if (fit != neededFeederId.end())
            addBEdge(oi, feederBase + (activeFeederMap[fit->second] - 1));
        }
      }
    }
    for (std::size_t pi = 0; pi < ptrList.size(); ++pi) {
      const NodeID ptr = ptrList[pi];
      const std::size_t v = nObj + pi;
      const auto &ptrPts = ptsOf(ptr);
      if (ptrPts.empty()) {
        auto fit = neededFeederId.find(ptr);
        if (fit != neededFeederId.end())
          addBEdge(v, feederBase + (activeFeederMap[fit->second] - 1));
      } else {
        for (ObjectID target : ptrPts) {
          auto it = objToIdx.find(target);
          if (it != objToIdx.end())
            addBEdge(v, it->second);
        }
      }
    }

    // Kosaraju SCC indices are in source-to-sink topological order.
    auto [comp, numComps] = computeSCCs(bSucc, bPred);
    std::vector<bool> compHasPending(numComps, false);
    for (std::size_t u = 0; u < n; ++u)
      compHasPending[comp[u]] = true;
    std::vector<std::vector<std::size_t>> compMembers(numComps);
    for (std::size_t u = 0; u < totalV; ++u)
      compMembers[comp[u]].push_back(u);
    std::vector<bool> compReachesOtherPending(numComps, false);
    for (std::size_t idx = numComps; idx > 0; --idx) {
      const std::size_t c = idx - 1;
      bool reaches = false;
      for (std::size_t u : compMembers[c]) {
        for (std::size_t v : bSucc[u]) {
          const std::size_t cv = comp[v];
          if (cv != c && (compHasPending[cv] || compReachesOtherPending[cv])) {
            reaches = true;
            break;
          }
        }
        if (reaches)
          break;
      }
      compReachesOtherPending[c] = reaches;
    }

    bool weakenedAny = false;
    for (std::size_t u = 0; u < n; ++u) {
      if (!compReachesOtherPending[comp[u]]) {
        weakenObjectLocally(
            nodes[u],
            "scalar escape cycle or unknown scalar escape dependency");
        weakenedAny = true;
      }
    }
    if (!weakenedAny) {
      for (ObjectID obj : nodes)
        weakenObjectLocally(
            obj, "scalar escape cycle or unresolved scalar escape dependency");
    }
  }

  //===--------------------------------------------------------------------===//
  // Algorithm 3: Main Solve Loop
  //===--------------------------------------------------------------------===//

  void run() {
    analyzed = false;
    stats = {};
    persistentCallTargets.assign(p.callSites_.size(), {});
    persistentCallExternal.assign(p.callSites_.size(), false);
    persistentCallUnknownHub.assign(p.callSites_.size(), false);
    reset(!config.enableStrongUpdates, /*preserveCallTargets=*/false);

    while (true) {
      computePointedToBy();

      bool strongRestartNeeded = false;
      if (resolveDynamicCalls(strongRestartNeeded)) {
        if (strongRestartNeeded && !weak) {
          ++stats.strongResets;
          reset(/*weakMode=*/false, /*preserveCallTargets=*/true);
          continue;
        }
        continue;
      }

      ensureUnknownSummary();
      if (hasPendingPropagation())
        continue;

      for (ObjectID object : dirtySigList) {
        if (!p.objects_[object].memory)
          continue;
        if (!processed[object])
          continue;
        Signature newSig = signature(object);
        const bool sigChanged = !(newSig == previous[object]);
        if (!sigChanged && !callGraphDirty[object])
          continue;

        if (canStrongUpdate(object) && emittedFlow[object]) {
          bool addedStrong = false;
          for (AccessID strong : newSig.strong) {
            if (!previous[object].strong.count(strong)) {
              addedStrong = true;
              break;
            }
          }
          if (addedStrong) {
            // Locally weaken only this object: all previously emitted edges
            // for `object` are a sound subset of its weak memory flow.
            weakenObjectLocally(
                object, "a new strong store invalidated the escape schedule");
            pending.insert(object);
            continue;
          }
        }
        pending.insert(object);
      }
      clearDirtySigs();
      std::fill(callGraphDirty.begin(), callGraphDirty.end(), false);
      ++callVisGen;

      if (pending.empty())
        break;

      auto processBatch = [&](const std::vector<ObjectID> &batch) {
        for (ObjectID obj : batch) {
          pending.erase(obj);
          processed[obj] = true;
          previous[obj] = signature(obj);
        }
        for (ObjectID obj : batch)
          computeIndirect(obj, previous[obj]);
      };

      if (weak) {
        std::vector<ObjectID> batch(pending.begin(), pending.end());
        pending.clear();
        processBatch(batch);
        continue;
      }

      // Priority 1: batch all currently unblocked non-singleton / weak objects
      // (mutually independent in escape order; avoids O(|Objects|^2) scans).
      std::vector<ObjectID> readyWeak;
      for (ObjectID object : pending) {
        if (!canStrongUpdate(object) && !escapeBlocked(object))
          readyWeak.push_back(object);
      }
      if (!readyWeak.empty()) {
        processBatch(readyWeak);
        continue;
      }

      // Priority 2: unblocked strong singletons (prefer objects feeding an
      // unresolved indirect call; otherwise batch mutually independent
      // unblocked singletons).
      ObjectID callFeeder = 0;
      std::vector<ObjectID> readySingletons;
      std::vector<ObjectID> deferredSingletons;
      for (ObjectID object : pending) {
        if (!canStrongUpdate(object) || escapeBlocked(object))
          continue;
        if (feedsUnresolvedCall(object)) {
          callFeeder = object;
          break;
        }
        if (!sharesFunctionWithUnresolvedCall(object)) {
          readySingletons.push_back(object);
        } else {
          deferredSingletons.push_back(object);
        }
      }
      if (callFeeder) {
        pending.erase(callFeeder);
        processed[callFeeder] = true;
        previous[callFeeder] = signature(callFeeder);
        computeIndirect(callFeeder, previous[callFeeder]);
        continue;
      }
      // While unresolved indirect calls exist, resolve pending weak objects
      // (which never perform strong updates) before emitting non-callFeeder
      // strong singletons, avoiding needless full-program strong restarts.
      if (anyUnresolvedIndirectCall) {
        std::vector<ObjectID> weakFirst;
        for (ObjectID object : pending)
          if (!canStrongUpdate(object))
            weakFirst.push_back(object);
        if (!weakFirst.empty()) {
          processBatch(weakFirst);
          continue;
        }
      }
      if (!readySingletons.empty()) {
        processBatch(readySingletons);
        continue;
      }
      if (!deferredSingletons.empty()) {
        processBatch(deferredSingletons);
        continue;
      }

      // Priority 3 (Algorithm 3 lines 25-29): batch ALL pending non-singleton
      // / weak objects together so multi-object non-scalar cycles do not
      // prematurely unblock dependent singletons.
      std::vector<ObjectID> weakBatch;
      for (ObjectID object : pending)
        if (!canStrongUpdate(object))
          weakBatch.push_back(object);
      if (!weakBatch.empty()) {
        processBatch(weakBatch);
        continue;
      }

      // Priority 4: only blocked strong singletons remain.
      breakBlockedSingletons();
    }

    for (const auto &sig : previous)
      finalStrong.insert(sig.strong.begin(), sig.strong.end());
    stats.strongUpdateSites = finalStrong.size();

    pted.clear();
    ptedBuilt = false;
    analyzed = true;
  }

  void requireAnalyzed() const {
    if (!analyzed)
      throw std::logic_error("ValueFlowPTA: analyze() must precede queries");
  }
};

//===----------------------------------------------------------------------===//
// Solver public methods
//===----------------------------------------------------------------------===//

Solver::Solver(const Program &program) : Solver(program, Config{}) {}
Solver::Solver(const Program &program, Config config)
    : impl_(std::make_unique<Impl>(program, config)) {}
Solver::~Solver() = default;

void Solver::analyze() { impl_->run(); }

const PointsToSet &Solver::pointsTo(NodeID node) const {
  impl_->requireAnalyzed();
  impl_->p.checkNode(node);
  return impl_->ptsOf(node);
}

const NodeSet &Solver::pointedToBy(ObjectID object) const {
  impl_->requireAnalyzed();
  if (!object || object > impl_->p.objectCount())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  impl_->ensurePted();
  return impl_->pted.at(object);
}

bool Solver::mayAlias(NodeID left, NodeID right) const {
  const auto &a = pointsTo(left);
  const auto &b = pointsTo(right);
  if (a.count(impl_->p.unknownObject()) || b.count(impl_->p.unknownObject()))
    return true;
  for (ObjectID object : a)
    if (object != impl_->p.nullObject() && b.count(object))
      return true;
  return false;
}

bool Solver::isStrongUpdate(AccessID access) const {
  impl_->requireAnalyzed();
  if (!access || access > impl_->p.accessCount())
    throw std::out_of_range("ValueFlowPTA: invalid access");
  return impl_->finalStrong.count(access) != 0;
}

const Solver::Statistics &Solver::statistics() const {
  impl_->requireAnalyzed();
  return impl_->stats;
}

std::size_t Solver::vfgNodeCount() const {
  impl_->requireAnalyzed();
  return impl_->adj.size() - 1;
}

bool Solver::isJoinNode(NodeID node) const {
  impl_->requireAnalyzed();
  if (!node || node >= impl_->adj.size())
    throw std::out_of_range("ValueFlowPTA: invalid VFG node");
  return node >= impl_->p.edges_.size();
}

const NodeSet &Solver::vfgSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  if (!node || node >= impl_->adj.size())
    throw std::out_of_range("ValueFlowPTA: invalid VFG node");
  impl_->ensureEdgesCache();
  return impl_->edgesCache[node];
}

NodeSet Solver::vfgIndirectSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  if (!node || node >= impl_->adj.size())
    throw std::out_of_range("ValueFlowPTA: invalid VFG node");
  impl_->ensureEdgesCache();
  NodeSet indirect = impl_->edgesCache[node];
  if (node < impl_->p.edges_.size()) {
    for (NodeID direct : impl_->p.edges_[node])
      indirect.erase(direct);
    auto it = impl_->extraDirectEdges.find(node);
    if (it != impl_->extraDirectEdges.end())
      indirect.subtract(it->second);
  }
  return indirect;
}

const std::set<FunctionID> &Solver::resolvedCallTargets(CallSiteID site) const {
  impl_->requireAnalyzed();
  impl_->p.checkCallSite(site);
  if (!impl_->siteCallsUnknownHub[site])
    return impl_->liveCallTargets[site];
  auto it = impl_->resolvedTargetsCache.find(site);
  if (it != impl_->resolvedTargetsCache.end())
    return it->second;
  std::set<FunctionID> merged = impl_->liveCallTargets[site];
  const auto &cs = impl_->p.callSites_[site];
  if (cs.calleePointer) {
    impl_->ptsOf(cs.calleePointer)
        .forEachIn(impl_->funcObjectSet, [&](ObjectID obj) {
          if (const FunctionID fn = impl_->p.functionOfObject_[obj])
            merged.insert(fn);
        });
  }
  return impl_->resolvedTargetsCache.emplace(site, std::move(merged))
      .first->second;
}

bool Solver::isFunctionReachable(FunctionID function) const {
  impl_->requireAnalyzed();
  impl_->p.checkFunction(function, /*allowImplicit=*/true);
  return impl_->funcReachable[function];
}

bool Solver::isBlockReachable(BlockID block) const {
  impl_->requireAnalyzed();
  impl_->p.checkBlock(block);
  return impl_->blockReachable[block];
}

} // namespace lotus::alias::vfg
