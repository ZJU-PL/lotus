#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowGraph.h"

#include <algorithm>
#include <deque>
#include <limits>
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

// Iterative Kosaraju SCC decomposition in source-to-sink topological order.
template <typename ID>
std::pair<std::vector<std::size_t>, std::size_t>
computeSCCs(const std::vector<std::vector<ID>> &successors,
            const std::vector<std::vector<ID>> &predecessors) {
  const std::size_t count = successors.size();
  std::vector<bool> seen(count, false);
  std::vector<std::size_t> postorder;
  postorder.reserve(count);
  for (std::size_t start = 0; start < count; ++start) {
    if (seen[start])
      continue;
    std::vector<std::pair<std::size_t, std::size_t>> dfs{{start, 0}};
    seen[start] = true;
    while (!dfs.empty()) {
      auto &frame = dfs.back();
      if (frame.second < successors[frame.first].size()) {
        const auto next = successors[frame.first][frame.second++];
        if (!seen[next]) {
          seen[next] = true;
          dfs.emplace_back(next, 0);
        }
      } else {
        postorder.push_back(frame.first);
        dfs.pop_back();
      }
    }
  }
  const auto invalid = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> component(count, invalid);
  std::size_t components = 0;
  for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
    if (component[*it] != invalid)
      continue;
    std::vector<std::size_t> work{*it};
    component[*it] = components;
    while (!work.empty()) {
      const auto node = work.back();
      work.pop_back();
      for (auto pred : predecessors[node]) {
        if (component[pred] == invalid) {
          component[pred] = components;
          work.push_back(pred);
        }
      }
    }
    ++components;
  }
  return {std::move(component), components};
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
  hash = hashMix(hash, set.size());
  for (NodeID id : set)
    hash = hashMix(hash, id);
  return hash;
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
      {source, initial, singleton, memory, true, 0, std::move(name)});
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
  appendUnique(edges_[from], to);
}

AccessID Program::addLoad(BlockID block, NodeID pointer, NodeID result) {
  checkBlock(block);
  checkNode(pointer);
  checkNode(result);
  if (sources_[result])
    throw std::invalid_argument(
        "ValueFlowPTA: load result is an object source");
  const AccessID id = nextID(accesses_.size());
  accesses_.push_back({AccessKind::Load, block, pointer, result, false});
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
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong});
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
  accesses_.push_back({AccessKind::Load, block, pointer, result, false});
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
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong});
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
  accesses_.push_back({AccessKind::Load, block, pointer, result, false});
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
  accesses_.push_back({AccessKind::Store, block, pointer, stored, allowStrong});
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
  appendUnique(callSites_[site].externalCallbacks, callback);
  callSites_[site].hasExternalTarget = true;
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
  std::vector<NodeSet> edges;
  std::vector<NodeSet> directEdges;
  std::vector<PointsToSet> pts;
  std::vector<NodeSet> pted;
  std::vector<NodeSet> reachSeeds;

  // Per-object tracking.
  NodeSet dirtySignatures;
  std::vector<bool> callGraphDirty;
  std::vector<NodeSet> activeAccesses;
  std::vector<NodeSet> escapePointers;
  std::vector<NodeSet> initialEscapeObjects;
  std::vector<std::vector<CallSiteID>> escapedIndirectCalls;
  std::vector<ObjectID> blockedWitness;
  std::vector<bool> effectiveSingleton;
  std::vector<FunctionID> effectiveOwner;
  std::vector<bool> forcedWeak;
  std::set<ObjectID> dirtyReach;
  std::set<ObjectID> pending;
  std::vector<bool> processed;
  std::vector<bool> emittedFlow;
  std::set<AccessID> finalStrong;

  // Node -> access / callsite use maps (populated lazily as functions become
  // reachable).
  std::vector<std::vector<AccessID>> uses;
  std::vector<NodeID> storedThrough;
  std::vector<ObjectID> initializedObject;
  std::vector<std::vector<CallSiteID>> indirectCallUses;
  std::vector<std::vector<CallSiteID>> indirectArgUses;
  std::vector<bool> isIndirectCalleeLoad;
  std::vector<bool> isCallBoundaryNode;
  std::vector<bool> objCallExposed;
  std::vector<NodeSet> reverseEscape;
  std::vector<NodeSet> ptrEscapedBy;

  // Generation-stamped per-function scratch arrays for O(|ActiveFuncs|)
  // computeIndirect without O(|Funcs|) allocations or full-module scans.
  std::uint64_t indirectGen = 0;
  std::vector<std::uint64_t> fnDirectModGen;
  std::vector<std::uint64_t> fnDirectRefGen;
  std::vector<std::uint64_t> fnTransModGen;
  std::vector<std::uint64_t> fnTransRefGen;
  std::vector<std::uint64_t> fnActiveGen;
  std::vector<std::uint64_t> fnSubgraphGen;

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
  std::vector<std::set<FunctionID>> callers;
  std::vector<std::set<CallSiteID>> callerSites;
  std::vector<bool> funcRecursive;
  std::vector<FunctionID> addressTakenFuncs;
  std::set<CallSiteID> pendingIndirectSites;
  std::vector<std::set<FunctionID>> persistentCallTargets;
  std::vector<bool> persistentCallExternal;

  struct Signature {
    NodeSet accesses;
    NodeSet strong;
    bool operator==(const Signature &other) const {
      return accesses == other.accesses && strong == other.strong;
    }
  };
  std::vector<Signature> previous;
  std::vector<Signature> current;

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

  // Per-object, per-function incremental reuse cache (paper Section 6, p. 9):
  // when an object is recomputed, functions whose local accesses and active
  // callsite summary did not change reuse their previous entry/exit nodes.
  struct CallFlowSummary {
    CallSiteID site = 0;
    bool hasIn = false;
    bool hasOut = false;
    bool outStrong = false;
    bool operator==(const CallFlowSummary &other) const {
      return site == other.site && hasIn == other.hasIn &&
             hasOut == other.hasOut && outStrong == other.outStrong;
    }
  };
  struct ObjectFuncCacheEntry {
    NodeSet accesses;
    NodeSet strong;
    std::vector<CallFlowSummary> calls;
    bool needExit = false;
    NodeID entryNode = 0;
    NodeID exitNode = 0;
    std::unordered_map<CallSiteID, NodeID> callInNodes;
    std::unordered_map<CallSiteID, NodeID> callOutNodes;
  };
  std::vector<std::unordered_map<FunctionID, ObjectFuncCacheEntry>>
      objectFuncCache;
  // Track which functions each processed object previously wired into, so
  // dynamic call-graph growth can mark affected processed objects dirty.
  std::vector<std::set<ObjectID>> funcAccessedByObjects;

  bool canStrongUpdate(ObjectID object) const {
    return !weak && effectiveSingleton[object] && !forcedWeak[object];
  }

  NodeID addJoin() {
    const NodeID node = nextID(edges.size());
    edges.emplace_back();
    directEdges.emplace_back();
    pts.emplace_back();
    uses.emplace_back();
    storedThrough.push_back(0);
    initializedObject.push_back(0);
    indirectCallUses.emplace_back();
    indirectArgUses.emplace_back();
    isIndirectCalleeLoad.push_back(false);
    isCallBoundaryNode.push_back(false);
    ptrEscapedBy.emplace_back();
    ++stats.valueFlowJoinNodes;
    return node;
  }

  void markCallVisibilityDirty(ObjectID root) {
    std::vector<ObjectID> work{root};
    NodeSet seen;
    seen.insert(root);
    while (!work.empty()) {
      const ObjectID cur = work.back();
      work.pop_back();
      objCallExposed[cur] = true;
      if (processed[cur]) {
        callGraphDirty[cur] = true;
        dirtySignatures.insert(cur);
      }
      for (ObjectID child : reverseEscape[cur]) {
        if (seen.insert(child).second)
          work.push_back(child);
      }
    }
  }

  void recordReverseEscape(ObjectID dep, ObjectID child) {
    if (!p.objects_[dep].memory)
      return;
    if (reverseEscape[dep].insert(child).second) {
      if (objCallExposed[dep])
        markCallVisibilityDirty(child);
    }
  }

  bool addVFGEdge(NodeID from, NodeID to, bool countIndirect) {
    if (!edges[from].insert(to).second)
      return false;
    if (countIndirect)
      ++stats.indirectEdges;
    else
      directEdges[from].insert(to);
    for (ObjectID object : pts[from]) {
      if (!pted[object].count(to)) {
        reachSeeds[object].insert(to);
        dirtyReach.insert(object);
      }
    }
    return true;
  }

  bool addIndirect(NodeID from, NodeID to) {
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

    fc.defBlockGen.assign(n, 0);
    fc.idfGen.assign(n, 0);
    fc.memoGen.assign(n, 0);
    fc.phiSparse.assign(n, 0);
    fc.lastStoreSparse.assign(n, 0);
    fc.memoExitDef.assign(n, 0);
    stats.expandedControlFlowNodes += n;

    // Register reachable accesses and callsites in this function.
    for (LocalBlock u = 1; u < n; ++u) {
      const BlockID b = fc.localToGlobal[u];
      std::uint32_t pos = 0;
      for (const auto &ev : p.blocks_[b].events) {
        if (ev.kind == Program::BlockEvent::Kind::Access) {
          const AccessID id = ev.id;
          accessEventPos[id] = pos++;
          const auto &access = p.accesses_[id];
          uses[access.pointer].push_back(id);
          if (access.kind == Program::AccessKind::Store) {
            storedThrough[access.value] = access.pointer;
            // Objects that already reached access.value now escape via pointer.
            for (ObjectID obj : pts[access.value]) {
              escapePointers[obj].insert(access.pointer);
              ptrEscapedBy[access.pointer].insert(obj);
              for (ObjectID target : pts[access.pointer])
                recordReverseEscape(target, obj);
            }
          }
          // Objects that already reached access.pointer now see this access.
          for (ObjectID obj : pts[access.pointer]) {
            activeAccesses[obj].insert(id);
            dirtySignatures.insert(obj);
            if (obj == p.unknownObject()) {
              for (ObjectID target = 1; target < p.objects_.size(); ++target)
                if (p.objects_[target].memory)
                  dirtySignatures.insert(target);
            }
          }
        } else {
          const CallSiteID site = ev.id;
          callEventPos[site] = pos++;
          funcCallSites[fn].push_back(site);
          const auto &cs = p.callSites_[site];
          if (cs.hasExternalTarget)
            liveCallExternal[site] = true;
          auto registerBoundaryNode = [&](NodeID node) {
            if (!node)
              return;
            isCallBoundaryNode[node] = true;
            for (ObjectID obj : pts[node]) {
              objCallExposed[obj] = true;
              if (obj == p.unknownObject()) {
                for (ObjectID target = 1; target < p.objects_.size(); ++target) {
                  if (p.objects_[target].memory && processed[target] &&
                      !p.objects_[target].isGlobal) {
                    callGraphDirty[target] = true;
                    dirtySignatures.insert(target);
                  }
                }
              } else {
                markCallVisibilityDirty(obj);
              }
            }
          };
          for (NodeID arg : cs.actualArgs)
            registerBoundaryNode(arg);
          registerBoundaryNode(cs.result);
          if (cs.calleePointer) {
            indirectCallUses[cs.calleePointer].push_back(site);
            // Mark loads producing calleePointer to prioritize resolving it.
            isIndirectCalleeLoad[cs.calleePointer] = true;
            if (!pts[cs.calleePointer].empty())
              pendingIndirectSites.insert(site);
            for (NodeID arg : cs.actualArgs) {
              if (!arg)
                continue;
              indirectArgUses[arg].push_back(site);
              for (ObjectID obj : pts[arg])
                appendUnique(escapedIndirectCalls[obj], site);
            }
          }
        }
      }
    }
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
    fc.weakFlowNode.assign(numComps, 0);
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
        if (persistentCallExternal[site])
          liveCallExternal[site] = true;
        for (FunctionID target : persistentCallTargets[site]) {
          if (connectCallEdgeInternal(site, target, /*dynamic=*/false,
                                      /*checkStrongKill=*/false))
            work.push_back(target);
        }
      }
    }
  }

  void updateRecursionStatus() {
    std::vector<std::vector<FunctionID>> cgSucc(p.functions_.size());
    std::vector<std::vector<FunctionID>> cgPred(p.functions_.size());
    std::vector<bool> selfCall(p.functions_.size(), false);
    for (CallSiteID site = 1; site < p.callSites_.size(); ++site) {
      const BlockID block = p.callSites_[site].block;
      if (!blockReachable[block])
        continue;
      const FunctionID caller = p.blocks_[block].function;
      for (FunctionID callee : liveCallTargets[site]) {
        if (caller == callee)
          selfCall[caller] = true;
        appendUnique(cgSucc[caller], callee);
        appendUnique(cgPred[callee], caller);
      }
    }
    auto [comp, numComps] = computeSCCs(cgSucc, cgPred);
    std::vector<std::size_t> compSize(numComps, 0);
    for (FunctionID f = 0; f < p.functions_.size(); ++f)
      ++compSize[comp[f]];

    for (FunctionID f = 1; f < p.functions_.size(); ++f) {
      const bool nowRec = selfCall[f] || compSize[comp[f]] > 1;
      if (nowRec && !funcRecursive[f]) {
        funcRecursive[f] = true;
        // Stack objects owned by a newly-recursive function can no longer
        // receive strong updates or stay scoped strictly to f.
        for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
          if (p.objects_[obj].ownerFunction == f) {
            if (effectiveSingleton[obj]) {
              effectiveSingleton[obj] = false;
              dirtySignatures.insert(obj);
            }
            if (effectiveOwner[obj] == f) {
              effectiveOwner[obj] = 0;
              callGraphDirty[obj] = true;
              dirtySignatures.insert(obj);
            }
          }
        }
      }
    }
  }

  bool connectCallEdgeInternal(CallSiteID site, FunctionID callee, bool dynamic,
                               bool checkStrongKill) {
    if (!liveCallTargets[site].insert(callee).second)
      return false;
    persistentCallTargets[site].insert(callee);
    const auto &cs = p.callSites_[site];
    const FunctionID caller = p.blocks_[cs.block].function;
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

    if (checkStrongKill) {
      // Any already-processed object accessed by callee (or its callees) needs
      // its interprocedural flow re-evaluated across site.
      std::vector<FunctionID> subWork{callee};
      std::set<FunctionID> subSeen;
      while (!subWork.empty()) {
        const FunctionID f = subWork.back();
        subWork.pop_back();
        if (!subSeen.insert(f).second)
          continue;
        for (ObjectID obj : funcAccessedByObjects[f]) {
          callGraphDirty[obj] = true;
          dirtySignatures.insert(obj);
        }
        for (CallSiteID subSite : funcCallSites[f])
          for (FunctionID next : liveCallTargets[subSite])
            subWork.push_back(next);
      }
      // Also mark objects accessed in caller if they were processed before
      // this indirect call target existed.
      for (ObjectID obj : funcAccessedByObjects[caller]) {
        callGraphDirty[obj] = true;
        dirtySignatures.insert(obj);
      }
    }
    return newlyReachable;
  }

  bool resolveDynamicCalls(bool &strongRestartNeeded) {
    bool connectedAny = false;
    while (!pendingIndirectSites.empty()) {
      const CallSiteID site = *pendingIndirectSites.begin();
      pendingIndirectSites.erase(pendingIndirectSites.begin());
      const auto &cs = p.callSites_[site];
      if (!blockReachable[cs.block] || !cs.calleePointer)
        continue;
      const FunctionID caller = p.blocks_[cs.block].function;
      const bool hadTargetsBefore =
          !liveCallTargets[site].empty() || liveCallExternal[site];
      std::vector<FunctionID> newTargets;
      for (ObjectID obj : pts[cs.calleePointer]) {
        if (obj == p.unknownObject()) {
          if (!liveCallExternal[site]) {
            liveCallExternal[site] = true;
            persistentCallExternal[site] = true;
            connectedAny = true;
            for (ObjectID accessed : funcAccessedByObjects[caller]) {
              callGraphDirty[accessed] = true;
              dirtySignatures.insert(accessed);
            }
          }
          for (FunctionID candidate : addressTakenFuncs)
            if (!liveCallTargets[site].count(candidate))
              newTargets.push_back(candidate);
        } else if (const FunctionID fn = p.functionOfObject_[obj]) {
          if (!liveCallTargets[site].count(fn))
            newTargets.push_back(fn);
        }
      }
      for (FunctionID callee : newTargets) {
        connectCallEdgeInternal(site, callee, /*dynamic=*/true,
                                /*checkStrongKill=*/true);
        connectedAny = true;
      }
      if (!newTargets.empty())
        computePointedToBy();
      if (!newTargets.empty() && !weak) {
        // Check if any already-emitted strong singleton gained a new strong
        // store from newly activated callees (regardless of hadTargetsBefore).
        for (ObjectID obj : dirtySignatures) {
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
        // If this callsite previously had no targets, also check whether a
        // newly connected callee can now materialize a strong callOutStore for
        // an already-emitted singleton in caller (or an ancestor).
        if (!strongRestartNeeded && !hadTargetsBefore) {
          std::set<FunctionID> subFuncs;
          std::vector<FunctionID> subWork(newTargets.begin(), newTargets.end());
          while (!subWork.empty()) {
            const FunctionID f = subWork.back();
            subWork.pop_back();
            if (!subFuncs.insert(f).second)
              continue;
            for (CallSiteID sub : funcCallSites[f])
              for (FunctionID next : liveCallTargets[sub])
                subWork.push_back(next);
          }
          std::set<FunctionID> ancFuncs;
          std::vector<FunctionID> ancWork{caller};
          while (!ancWork.empty()) {
            const FunctionID f = ancWork.back();
            ancWork.pop_back();
            if (!ancFuncs.insert(f).second)
              continue;
            for (FunctionID parent : callers[f])
              ancWork.push_back(parent);
          }
          for (FunctionID f : subFuncs) {
            for (ObjectID obj : funcAccessedByObjects[f]) {
              if (emittedFlow[obj] && canStrongUpdate(obj)) {
                strongRestartNeeded = true;
                break;
              }
            }
            if (strongRestartNeeded)
              break;
          }
          if (!strongRestartNeeded) {
            auto hasStoreInSubFuncs = [&](const NodeSet &accesses) {
              for (AccessID id : accesses) {
                if (p.accesses_[id].kind == Program::AccessKind::Store &&
                    subFuncs.count(p.blocks_[p.accesses_[id].block].function))
                  return true;
              }
              return false;
            };
            const bool unknownModInSub =
                hasStoreInSubFuncs(activeAccesses[p.unknownObject()]);
            for (FunctionID anc : ancFuncs) {
              for (ObjectID obj : funcAccessedByObjects[anc]) {
                if (!emittedFlow[obj] || !canStrongUpdate(obj))
                  continue;
                if (unknownModInSub ||
                    hasStoreInSubFuncs(activeAccesses[obj])) {
                  strongRestartNeeded = true;
                  break;
                }
              }
              if (strongRestartNeeded)
                break;
            }
          }
        }
      }
    }
    if (connectedAny) {
      updateRecursionStatus();
      updateUnresolvedIndirectCallFlags();
    }
    return connectedAny;
  }

  void updateUnresolvedIndirectCallFlags() {
    funcHasUnresolvedIndirectCall.assign(p.functions_.size(), false);
    for (CallSiteID site = 1; site < p.callSites_.size(); ++site) {
      const auto &cs = p.callSites_[site];
      if (!cs.calleePointer || !blockReachable[cs.block])
        continue;
      if (liveCallTargets[site].empty() && !liveCallExternal[site])
        funcHasUnresolvedIndirectCall[p.blocks_[cs.block].function] = true;
    }
  }

  //===--------------------------------------------------------------------===//
  // Initialization / Reset
  //===--------------------------------------------------------------------===//

  void reset(bool weakMode, bool preserveCallTargets) {
    weak = weakMode;
    flowTemplates.clear();
    objectFuncCache.assign(p.objects_.size(), {});
    funcAccessedByObjects.assign(p.functions_.size(), {});
    edges.assign(p.edges_.size(), {});
    directEdges.assign(p.edges_.size(), {});
    for (NodeID node = 1; node < p.edges_.size(); ++node) {
      edges[node].insert(p.edges_[node].begin(), p.edges_[node].end());
      directEdges[node].insert(p.edges_[node].begin(), p.edges_[node].end());
    }
    pts.assign(p.edges_.size(), {});
    pted.assign(p.objects_.size(), {});
    reachSeeds.assign(p.objects_.size(), {});
    dirtySignatures.clear();
    callGraphDirty.assign(p.objects_.size(), false);
    activeAccesses.assign(p.objects_.size(), {});
    escapePointers.assign(p.objects_.size(), {});
    initialEscapeObjects.assign(p.objects_.size(), {});
    escapedIndirectCalls.assign(p.objects_.size(), {});
    blockedWitness.assign(p.objects_.size(), 0);
    effectiveSingleton.assign(p.objects_.size(), false);
    effectiveOwner.assign(p.objects_.size(), 0);
    forcedWeak.assign(p.objects_.size(), false);
    for (ObjectID obj = 1; obj < p.objects_.size(); ++obj) {
      effectiveSingleton[obj] = p.objects_[obj].singleton;
      effectiveOwner[obj] = p.objects_[obj].ownerFunction;
    }
    current.assign(p.objects_.size(), {});
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
    objCallExposed.assign(p.objects_.size(), false);
    for (ObjectID obj = 1; obj < p.objects_.size(); ++obj)
      objCallExposed[obj] =
          p.objects_[obj].isGlobal || obj == p.unknownObject();
    reverseEscape.assign(p.objects_.size(), {});
    ptrEscapedBy.assign(p.edges_.size(), {});
    indirectGen = 0;
    fnDirectModGen.assign(p.functions_.size(), 0);
    fnDirectRefGen.assign(p.functions_.size(), 0);
    fnTransModGen.assign(p.functions_.size(), 0);
    fnTransRefGen.assign(p.functions_.size(), 0);
    fnActiveGen.assign(p.functions_.size(), 0);
    fnSubgraphGen.assign(p.functions_.size(), 0);
    dirtyReach.clear();
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
    callers.assign(p.functions_.size(), {});
    callerSites.assign(p.functions_.size(), {});
    funcRecursive.assign(p.functions_.size(), false);
    pendingIndirectSites.clear();
    if (!preserveCallTargets) {
      persistentCallTargets.assign(p.callSites_.size(), {});
      persistentCallExternal.assign(p.callSites_.size(), false);
    }

    addressTakenFuncs.clear();
    for (FunctionID f = 1; f < p.functions_.size(); ++f)
      if (p.functions_[f].addressTaken)
        addressTakenFuncs.push_back(f);

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
      dirtyReach.insert(object);
      reachSeeds[object].insert(p.objects_[object].address);
      dirtySignatures.insert(object);
      if (!p.objects_[object].memory)
        continue;
      pending.insert(object);
      const auto initKind = p.initializerKinds_[object];
      if (initKind == Program::InitializerKind::Empty)
        continue;
      const NodeID initializer =
          initKind == Program::InitializerKind::Explicit
              ? p.initializers_[object]
              : p.unknownValue();
      edges[initializer].insert(p.objects_[object].initialStore);
      initializedObject[p.objects_[object].initialStore] = object;
    }
  }

  //===--------------------------------------------------------------------===//
  // Algorithm 1: Incremental ComputePtedSet + Transitive ComputeEspSet
  //===--------------------------------------------------------------------===//

  void computePointedToBy() {
    while (!dirtyReach.empty()) {
      const ObjectID object = *dirtyReach.begin();
      dirtyReach.erase(dirtyReach.begin());
      ++stats.objectTraversals;
      NodeSet seeds;
      seeds.swap(reachSeeds[object]);
      seeds.subtract(pted[object]);
      if (seeds.empty())
        continue;
      pted[object].unite(seeds);
      std::vector<NodeID> work(seeds.begin(), seeds.end());
      while (!work.empty()) {
        const NodeID node = work.back();
        work.pop_back();
        pts[node].insert(object);
        if (initializedObject[node]) {
          if (initialEscapeObjects[object].insert(initializedObject[node]).second)
            recordReverseEscape(initializedObject[node], object);
        } else if (storedThrough[node]) {
          const NodeID ptr = storedThrough[node];
          if (escapePointers[object].insert(ptr).second) {
            ptrEscapedBy[ptr].insert(object);
            for (ObjectID target : pts[ptr])
              recordReverseEscape(target, object);
          }
        }
        if (!ptrEscapedBy[node].empty()) {
          for (ObjectID child : ptrEscapedBy[node])
            recordReverseEscape(object, child);
        }
        if (isCallBoundaryNode[node]) {
          objCallExposed[object] = true;
          if (object == p.unknownObject()) {
            for (ObjectID target = 1; target < p.objects_.size(); ++target) {
              if (p.objects_[target].memory && processed[target] &&
                  !p.objects_[target].isGlobal) {
                callGraphDirty[target] = true;
                dirtySignatures.insert(target);
              }
            }
          } else {
            markCallVisibilityDirty(object);
          }
        }
        if (!indirectCallUses[node].empty()) {
          for (CallSiteID site : indirectCallUses[node])
            pendingIndirectSites.insert(site);
        }
        if (!indirectArgUses[node].empty()) {
          for (CallSiteID site : indirectArgUses[node])
            appendUnique(escapedIndirectCalls[object], site);
        }
        if (!uses[node].empty()) {
          activeAccesses[object].insert(uses[node].begin(), uses[node].end());
          dirtySignatures.insert(object);
          for (ObjectID target : pts[node])
            if (effectiveSingleton[target])
              dirtySignatures.insert(target);
          if (object == p.unknownObject()) {
            for (ObjectID target = 1; target < p.objects_.size(); ++target)
              if (p.objects_[target].memory)
                dirtySignatures.insert(target);
          }
        }
        NodeSet unseen = edges[node];
        unseen.subtract(pted[object]);
        pted[object].unite(unseen);
        work.insert(work.end(), unseen.begin(), unseen.end());
      }
    }
  }

  Signature signature(ObjectID object) const {
    Signature result;
    result.accesses = activeAccesses[object];
    // Unknown is a wildcard address, not a disjoint allocation site.
    if (object != p.unknownObject())
      result.accesses.unite(activeAccesses[p.unknownObject()]);
    if (canStrongUpdate(object)) {
      for (AccessID id : result.accesses) {
        const auto &access = p.accesses_[id];
        if (access.allowStrong && access.kind == Program::AccessKind::Store &&
            pts[access.pointer].size() == 1 && pts[access.pointer].count(object))
          result.strong.insert(id);
      }
    }
    return result;
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
    auto setWitness = [&](ObjectID dep) { blockedWitness[object] = dep; };

    bool sawEmpty = false;
    bool sawUnknown = false;
    std::vector<ObjectID> work{object};
    NodeSet visited;
    visited.insert(object);
    while (!work.empty()) {
      const ObjectID cur = work.back();
      work.pop_back();
      for (ObjectID dep : initialEscapeObjects[cur]) {
        if (!p.objects_[dep].memory)
          continue;
        if (pending.count(dep)) {
          setWitness(dep);
          return EscapeBlockKind::PendingObject;
        }
        if (visited.insert(dep).second)
          work.push_back(dep);
      }
      for (NodeID ptr : escapePointers[cur]) {
        const auto &targets = pts[ptr];
        if (targets.empty()) {
          sawEmpty = true;
          continue;
        }
        if (targets.count(p.unknownObject())) {
          if (pending.count(p.unknownObject())) {
            setWitness(p.unknownObject());
            return EscapeBlockKind::PendingObject;
          }
          sawUnknown = true;
          continue;
        }
        for (ObjectID target : targets) {
          if (!p.objects_[target].memory)
            continue;
          if (pending.count(target)) {
            setWitness(target);
            return EscapeBlockKind::PendingObject;
          }
          if (visited.insert(target).second)
            work.push_back(target);
        }
      }
      for (CallSiteID site : escapedIndirectCalls[cur]) {
        if (liveCallTargets[site].empty() && !liveCallExternal[site])
          sawEmpty = true;
      }
    }
    if (sawUnknown) {
      setWitness(object);
      return EscapeBlockKind::UnknownEscape;
    }
    blockedWitness[object] = 0;
    return sawEmpty ? EscapeBlockKind::EmptyPointerOrCall
                    : EscapeBlockKind::Unblocked;
  }

  bool escapeBlocked(ObjectID object) {
    return inspectEscapeBlock(object) != EscapeBlockKind::Unblocked;
  }

  bool feedsUnresolvedCall(ObjectID object) const {
    for (AccessID id : current[object].accesses) {
      const auto &access = p.accesses_[id];
      if (access.kind == Program::AccessKind::Load &&
          isIndirectCalleeLoad[access.value])
        return true;
    }
    return false;
  }

  bool sharesFunctionWithUnresolvedCall(ObjectID object) const {
    for (AccessID id : current[object].accesses) {
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
        if (!events[k].isStore) {
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

    // Kosaraju component indices are in source-to-sink topological order.
    std::sort(activeComps.begin(), activeComps.end());
    const std::size_t rootComp = fc.comp[0];
    for (std::size_t c : activeComps) {
      fc.weakFlowNode[c] = 0;
      if (c == rootComp) {
        fc.weakFlowNode[c] = entryJoinNode;
        continue;
      }
      NodeSet inNodes;
      for (auto pred : fc.compPreds[c]) {
        if (fc.weakNeededGen[pred] == g && fc.weakFlowNode[pred])
          inNodes.insert(fc.weakFlowNode[pred]);
      }

      if (fc.compCyclic[c]) {
        std::vector<NodeID> loads;
        for (LocalBlock b : fc.compBlocks[c]) {
          auto it = blockRanges.find(b);
          if (it == blockRanges.end())
            continue;
          for (std::size_t k = it->second.first; k < it->second.second; ++k) {
            if (events[k].isStore)
              inNodes.insert(events[k].node);
            else
              loads.push_back(events[k].node);
          }
        }
        NodeID curFlow = 0;
        if (inNodes.size() == 1) {
          curFlow = *inNodes.begin();
        } else {
          curFlow = addJoin();
          for (NodeID in : inNodes)
            addIndirect(in, curFlow);
        }
        for (NodeID load : loads)
          addIndirect(curFlow, load);
        fc.weakFlowNode[c] = curFlow;
      } else {
        // Trivial acyclic single-block SCC: preserve exact intra-block order.
        NodeID curFlow = 0;
        if (inNodes.size() == 1) {
          curFlow = *inNodes.begin();
        } else {
          curFlow = addJoin();
          for (NodeID in : inNodes)
            addIndirect(in, curFlow);
        }
        const LocalBlock b = fc.compBlocks[c][0];
        auto it = blockRanges.find(b);
        if (it != blockRanges.end()) {
          std::vector<NodeID> pendingStores;
          auto flushStores = [&]() {
            if (pendingStores.empty())
              return;
            NodeID next = addJoin();
            if (curFlow)
              addIndirect(curFlow, next);
            for (NodeID s : pendingStores)
              addIndirect(s, next);
            curFlow = next;
            pendingStores.clear();
          };
          for (std::size_t k = it->second.first; k < it->second.second; ++k) {
            if (events[k].isStore) {
              pendingStores.push_back(events[k].node);
            } else {
              flushStores();
              addIndirect(curFlow, events[k].node);
            }
          }
          flushStores();
        }
        fc.weakFlowNode[c] = curFlow;
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
      if (!sparseEdgeSet[from].insert(to).second)
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

    // Condense non-killed SCCs of the sparse graph and factor into join nodes.
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
    auto [component, components] = computeSCCs(liveSuccs, livePreds);
    std::vector<NodeID> compNodes(components, 0);
    compNodes[component[0]] = entryJoinNode;
    for (std::size_t c = 0; c < components; ++c) {
      if (!compNodes[c])
        compNodes[c] = addJoin();
    }
    for (SparseNode u = 1; u < count; ++u) {
      if (sparseStoreValue[u])
        addIndirect(sparseStoreValue[u], compNodes[component[u]]);
      if (!sparseStrong[u]) {
        for (SparseNode pred : sparsePreds[u])
          if (component[pred] != component[u])
            addIndirect(compNodes[component[pred]], compNodes[component[u]]);
      }
    }
    for (const auto &[srcSparse, dstNode] : loadDefs)
      addIndirect(compNodes[component[srcSparse]], dstNode);
  }

  void computeLocalFlowForFunction(FunctionID fn, NodeID entryJoinNode,
                                   std::vector<LocalEvent> &events,
                                   bool hasAnyStrong) {
    ++stats.functionFlowComputations;
    std::sort(events.begin(), events.end(),
              [](const LocalEvent &a, const LocalEvent &b) {
                return std::tie(a.block, a.pos, a.phase) <
                       std::tie(b.block, b.pos, b.phase);
              });
    if (!hasAnyStrong)
      computeWeakLocalFlow(fn, entryJoinNode, events);
    else
      computeStrongLocalFlow(fn, entryJoinNode, events);
  }

  // Algorithm 2: PreprocessGlobalFlow + per-function ComputeLocalFlow.
  // Uses generation-stamped function markers so work is O(|ActiveFuncs(object)|)
  // rather than O(|Functions|), and scopes interprocedural call-in/call-out
  // instrumentation to callsites that can actually access `object` (matching
  // Section 5.2.1 while soundly handling globals, nested carriers, and returns).
  void computeIndirect(ObjectID object, const Signature &sig) {
    ++stats.indirectFlowComputations;
    stats.projectedEvents += sig.accesses.size();
    if (sig.accesses.empty())
      return;

    const std::uint64_t g = ++indirectGen;

    // Partition accesses by function and detect if any load exists at all.
    std::unordered_map<FunctionID, NodeSet> fnAccesses;
    std::unordered_map<FunctionID, NodeSet> fnStrong;
    std::vector<FunctionID> directModFuncs;
    std::vector<FunctionID> directRefFuncs;

    for (AccessID id : sig.accesses) {
      const auto &access = p.accesses_[id];
      if (!blockReachable[access.block])
        continue;
      const FunctionID fn = p.blocks_[access.block].function;
      fnAccesses[fn].insert(id);
      if (sig.strong.count(id))
        fnStrong[fn].insert(id);
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
    if (directRefFuncs.empty())
      return;
    emittedFlow[object] = true;

    // Compute transitive escape carrier set for sound callsite scoping.
    bool globalVisible = p.objects_[object].isGlobal ||
                         object == p.unknownObject();
    NodeSet carrierObjects;
    carrierObjects.insert(object);
    if (!globalVisible) {
      std::vector<ObjectID> escWork{object};
      while (!escWork.empty() && !globalVisible) {
        const ObjectID cur = escWork.back();
        escWork.pop_back();
        for (ObjectID dep : initialEscapeObjects[cur]) {
          if (!p.objects_[dep].memory)
            continue;
          if (p.objects_[dep].isGlobal || dep == p.unknownObject()) {
            globalVisible = true;
            break;
          }
          if (carrierObjects.insert(dep).second)
            escWork.push_back(dep);
        }
        if (globalVisible)
          break;
        for (NodeID ptr : escapePointers[cur]) {
          const auto &targets = pts[ptr];
          if (targets.empty() || targets.count(p.unknownObject())) {
            globalVisible = true;
            break;
          }
          for (ObjectID target : targets) {
            if (!p.objects_[target].memory)
              continue;
            if (p.objects_[target].isGlobal) {
              globalVisible = true;
              break;
            }
            if (carrierObjects.insert(target).second)
              escWork.push_back(target);
          }
          if (globalVisible)
            break;
        }
      }
    }

    auto nodeIntersectsCarriers = [&](NodeID n) -> bool {
      if (!n)
        return false;
      const auto &pSet = pts[n];
      if (pSet.count(p.unknownObject()))
        return true;
      for (ObjectID c : carrierObjects)
        if (pSet.count(c))
          return true;
      return false;
    };

    auto callSiteCanCarryObject = [&](CallSiteID site) -> bool {
      if (globalVisible || liveCallExternal[site])
        return true;
      const auto &cs = p.callSites_[site];
      for (ObjectID c : carrierObjects) {
        if (const FunctionID owner = p.objects_[c].ownerFunction) {
          if (liveCallTargets[site].count(owner))
            return true;
        }
      }
      for (NodeID arg : cs.actualArgs)
        if (nodeIntersectsCarriers(arg))
          return true;
      if (nodeIntersectsCarriers(cs.result))
        return true;
      return false;
    };

    // Determine whether a local stack object stays strictly within its owner's
    // call-graph subgraph. Fast path O(1) when all accesses are inside owner.
    FunctionID scopeRoot = 0;
    if (const FunctionID owner = effectiveOwner[object]) {
      if (funcReachable[owner] && !funcRecursive[owner]) {
        if (fnAccesses.size() == 1 && fnAccesses.count(owner)) {
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
            }
          }
          bool allInside = true;
          for (const auto &entry : fnAccesses) {
            if (fnSubgraphGen[entry.first] != g) {
              allInside = false;
              break;
            }
          }
          if (allInside)
            scopeRoot = owner;
        }
      }
    }

    // Backward call-graph propagation for TransMod and TransRef, traversing
    // only callsites that can actually carry `object` and collecting
    // `activeFuncs` in O(|ActiveFuncs|).
    std::vector<FunctionID> activeFuncs;
    auto markActive = [&](FunctionID f) {
      if (fnActiveGen[f] != g) {
        fnActiveGen[f] = g;
        activeFuncs.push_back(f);
        funcAccessedByObjects[f].insert(object);
      }
    };
    auto propagateUp = [&](const std::vector<FunctionID> &seeds,
                           std::vector<std::uint64_t> &markGen) {
      std::vector<FunctionID> work;
      for (FunctionID f : seeds) {
        if (markGen[f] != g) {
          markGen[f] = g;
          markActive(f);
          work.push_back(f);
        }
      }
      while (!work.empty()) {
        const FunctionID f = work.back();
        work.pop_back();
        if (scopeRoot && f == scopeRoot)
          continue;
        for (CallSiteID site : callerSites[f]) {
          const BlockID blk = p.callSites_[site].block;
          if (!blockReachable[blk] || !callSiteCanCarryObject(site))
            continue;
          const FunctionID caller = p.blocks_[blk].function;
          if (funcReachable[caller] && markGen[caller] != g) {
            markGen[caller] = g;
            markActive(caller);
            work.push_back(caller);
          }
        }
      }
    };
    propagateUp(directModFuncs, fnTransModGen);
    propagateUp(directRefFuncs, fnTransRefGen);

    auto isTransMod = [&](FunctionID f) { return fnTransModGen[f] == g; };
    auto isTransRef = [&](FunctionID f) { return fnTransRefGen[f] == g; };

    // PreprocessGlobalFlow: prepare per-function call summaries and auxiliary
    // entry/exit/callIn/callOut nodes, reusing unchanged functions per object.
    const bool objCanStrong = canStrongUpdate(object);
    std::unordered_map<FunctionID, NodeID> entryNodeOf;
    std::unordered_map<FunctionID, NodeID> exitNodeOf;
    std::unordered_map<FunctionID, std::vector<CallFlowSummary>> callSummaries;
    std::unordered_map<FunctionID, bool> reusedFunc;

    for (FunctionID f : activeFuncs) {
      auto &summaries = callSummaries[f];
      for (CallSiteID site : funcCallSites[f]) {
        if (!callSiteCanCarryObject(site))
          continue;
        const auto &targets = liveCallTargets[site];
        bool anyAccess = false;
        bool anyMod = false;
        bool allMod = !targets.empty() && !liveCallExternal[site] &&
                      !p.callSites_[site].isInvoke;
        for (FunctionID callee : targets) {
          if (isTransMod(callee) || isTransRef(callee))
            anyAccess = true;
          if (isTransMod(callee))
            anyMod = true;
          else
            allMod = false;
        }
        if (!anyAccess)
          continue;
        const bool hasOut = anyMod && p.callSites_[site].mayReturn;
        const bool outStrong = hasOut && objCanStrong && allMod;
        summaries.push_back({site, true, hasOut, outStrong});
      }
      bool hasRelevantCaller = false;
      if (isTransMod(f) && funcCFGs[f].exitLocal != InvalidLocal &&
          !(scopeRoot && f == scopeRoot)) {
        for (CallSiteID site : callerSites[f]) {
          if (blockReachable[p.callSites_[site].block] &&
              callSiteCanCarryObject(site)) {
            hasRelevantCaller = true;
            break;
          }
        }
      }
      const bool needExit = hasRelevantCaller;

      // Check per-object, per-function reuse cache (Section 6, p. 9).
      auto &objCache = objectFuncCache[object];
      auto cacheIt = objCache.find(f);
      const NodeSet &acc = fnAccesses[f];
      const NodeSet &str = fnStrong[f];
      if (cacheIt != objCache.end() && cacheIt->second.accesses == acc &&
          cacheIt->second.strong == str && cacheIt->second.calls == summaries &&
          cacheIt->second.needExit == needExit) {
        reusedFunc[f] = true;
        entryNodeOf[f] = cacheIt->second.entryNode;
        exitNodeOf[f] = cacheIt->second.exitNode;
        ++stats.functionFlowReuseHits;
        continue;
      }

      reusedFunc[f] = false;

      // Cross-object template cache for callsite-free, exit-free functions.
      if (summaries.empty() && !needExit) {
        LocalTemplateKey key{f, acc, str};
        auto tmplIt = flowTemplates.find(key);
        if (tmplIt != flowTemplates.end()) {
          ++stats.indirectFlowCacheHits;
          entryNodeOf[f] = tmplIt->second;
          exitNodeOf[f] = 0;
          ObjectFuncCacheEntry entry;
          entry.accesses = acc;
          entry.strong = str;
          entry.needExit = false;
          entry.entryNode = tmplIt->second;
          entry.exitNode = 0;
          objCache[f] = std::move(entry);
          reusedFunc[f] = true;
          continue;
        }
      }

      entryNodeOf[f] = addJoin();
      exitNodeOf[f] = needExit ? addJoin() : 0;
    }

    // Wire initialStore into entry functions, scopeRoot, and any active
    // function not reachable from {entryFunctions_ U {scopeRoot}} along active
    // carrying call edges (e.g., a non-global calloc object dereferenced inside
    // a callee whose callers do not carry the object).
    std::vector<FunctionID> initReachWork;
    NodeSet initReached;
    for (FunctionID f : activeFuncs) {
      if (p.entryFunctions_.count(f) || (scopeRoot && f == scopeRoot)) {
        if (initReached.insert(f).second)
          initReachWork.push_back(f);
      }
    }
    while (!initReachWork.empty()) {
      const FunctionID f = initReachWork.back();
      initReachWork.pop_back();
      for (const auto &cs : callSummaries[f]) {
        for (FunctionID callee : liveCallTargets[cs.site]) {
          if (fnActiveGen[callee] == g && initReached.insert(callee).second)
            initReachWork.push_back(callee);
        }
      }
    }

    for (FunctionID f : activeFuncs) {
      if (p.entryFunctions_.count(f) || (scopeRoot && f == scopeRoot) ||
          !initReached.count(f)) {
        addIndirect(p.objects_[object].initialStore, entryNodeOf[f]);
      }

      auto &summaries = callSummaries[f];
      auto &objCache = objectFuncCache[object];

      if (reusedFunc[f]) {
        // Still ensure interprocedural call edges exist if new targets arrived.
        auto &cached = objCache[f];
        for (const auto &cs : summaries) {
          if (cs.hasIn) {
            const NodeID inNode = cached.callInNodes.at(cs.site);
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee) || isTransRef(callee)) {
                if (addIndirect(inNode, entryNodeOf.at(callee)))
                  ++stats.interproceduralAuxEdges;
              }
            }
          }
          if (cs.hasOut) {
            const NodeID outNode = cached.callOutNodes.at(cs.site);
            for (FunctionID callee : liveCallTargets[cs.site]) {
              if (isTransMod(callee) && exitNodeOf.count(callee) &&
                  exitNodeOf.at(callee)) {
                if (addIndirect(exitNodeOf.at(callee), outNode))
                  ++stats.interproceduralAuxEdges;
              }
            }
          }
        }
        continue;
      }

      // Build local event list for function f.
      std::vector<LocalEvent> events;
      const NodeSet &acc = fnAccesses[f];
      const NodeSet &str = fnStrong[f];
      bool hasAnyStrong = !str.empty();
      for (AccessID id : acc) {
        const auto &access = p.accesses_[id];
        LocalEvent ev;
        ev.block = blockToLocal[access.block];
        ev.pos = accessEventPos[id];
        ev.phase = 0;
        ev.isStore = (access.kind == Program::AccessKind::Store);
        ev.isStrong = ev.isStore && (str.count(id) != 0);
        ev.node = access.value;
        events.push_back(ev);
      }

      ObjectFuncCacheEntry cacheEntry;
      cacheEntry.accesses = acc;
      cacheEntry.strong = str;
      cacheEntry.calls = summaries;
      cacheEntry.needExit = (exitNodeOf[f] != 0);
      cacheEntry.entryNode = entryNodeOf[f];
      cacheEntry.exitNode = exitNodeOf[f];

      for (const auto &cs : summaries) {
        const LocalBlock callBlock = blockToLocal[p.callSites_[cs.site].block];
        const std::uint32_t pos = callEventPos[cs.site];
        if (cs.hasIn) {
          const NodeID inNode = addJoin();
          cacheEntry.callInNodes[cs.site] = inNode;
          events.push_back({callBlock, pos, 0, false, false, inNode, 0});
          for (FunctionID callee : liveCallTargets[cs.site]) {
            if (isTransMod(callee) || isTransRef(callee)) {
              if (addIndirect(inNode, entryNodeOf.at(callee)))
                ++stats.interproceduralAuxEdges;
            }
          }
        }
        if (cs.hasOut) {
          const NodeID outNode = addJoin();
          cacheEntry.callOutNodes[cs.site] = outNode;
          if (cs.outStrong)
            hasAnyStrong = true;
          events.push_back({callBlock, pos, 1, true, cs.outStrong, outNode, 0});
          for (FunctionID callee : liveCallTargets[cs.site]) {
            if (isTransMod(callee) && exitNodeOf.count(callee) &&
                exitNodeOf.at(callee)) {
              if (addIndirect(exitNodeOf.at(callee), outNode))
                ++stats.interproceduralAuxEdges;
            }
          }
        }
      }

      if (exitNodeOf[f]) {
        events.push_back({funcCFGs[f].exitLocal,
                          std::numeric_limits<std::uint32_t>::max(), 2, false,
                          false, exitNodeOf[f], 0});
      }

      computeLocalFlowForFunction(f, entryNodeOf[f], events, hasAnyStrong);

      if (summaries.empty() && !cacheEntry.needExit)
        flowTemplates.emplace(LocalTemplateKey{f, acc, str}, entryNodeOf[f]);
      objCache[f] = std::move(cacheEntry);
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
    current[object] = signature(object);
  }

  // When all remaining pending objects are strong singletons and blocked,
  // either (a) process a singleton blocked solely by a permanently empty
  // pointer/call without weakening, or (b) weaken only the minimal scalar
  // escape cycle / unknown-escaping singletons.
  void breakBlockedSingletons() {
    // (a) Check for any pending singleton blocked solely by an empty pointer or
    // unresolved empty indirect call (no pending object dependency and no
    // unknown escape). Processing it preserves its strong updates.
    for (ObjectID obj : pending) {
      if (inspectEscapeBlock(obj) == EscapeBlockKind::EmptyPointerOrCall) {
        pending.erase(obj);
        processed[obj] = true;
        previous[obj] = current[obj];
        computeIndirect(obj, current[obj]);
        return;
      }
    }

    // (b) Build the escape dependency graph induced on pending singletons:
    // edge u -> v means u depends on v (v ✂ u, so v must be processed before u).
    std::vector<ObjectID> nodes(pending.begin(), pending.end());
    std::unordered_map<ObjectID, std::size_t> idxOf;
    for (std::size_t i = 0; i < nodes.size(); ++i)
      idxOf[nodes[i]] = i;

    const std::size_t n = nodes.size();
    std::vector<std::vector<std::size_t>> succs(n), preds(n);

    for (std::size_t i = 0; i < n; ++i) {
      const ObjectID obj = nodes[i];
      std::vector<ObjectID> work{obj};
      NodeSet visited;
      visited.insert(obj);
      while (!work.empty()) {
        const ObjectID cur = work.back();
        work.pop_back();
        auto checkDep = [&](ObjectID dep) {
          if (!p.objects_[dep].memory || dep == p.unknownObject())
            return;
          auto it = idxOf.find(dep);
          if (it != idxOf.end()) {
            if (it->second != i) {
              appendUnique(succs[i], it->second);
              appendUnique(preds[it->second], i);
            }
          } else if (visited.insert(dep).second) {
            work.push_back(dep);
          }
        };
        for (ObjectID dep : initialEscapeObjects[cur])
          checkDep(dep);
        for (NodeID ptr : escapePointers[cur]) {
          for (ObjectID target : pts[ptr])
            checkDep(target);
        }
      }
    }

    // Sink SCCs in `succs` (no outgoing edges to other SCCs) are the minimal
    // cycles / unknown-escaping objects blocking everything else.
    auto [comp, numComps] = computeSCCs(succs, preds);
    std::vector<bool> hasOutEdge(numComps, false);
    for (std::size_t u = 0; u < n; ++u)
      for (std::size_t v : succs[u])
        if (comp[u] != comp[v])
          hasOutEdge[comp[u]] = true;

    bool weakenedAny = false;
    for (std::size_t u = 0; u < n; ++u) {
      if (!hasOutEdge[comp[u]]) {
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

      for (ObjectID object : dirtySignatures) {
        if (!p.objects_[object].memory)
          continue;
        Signature newSig = signature(object);
        if (!processed[object]) {
          current[object] = std::move(newSig);
          continue;
        }
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
        current[object] = std::move(newSig);
        pending.insert(object);
      }
      dirtySignatures.clear();
      std::fill(callGraphDirty.begin(), callGraphDirty.end(), false);

      if (pending.empty())
        break;

      if (weak) {
        const ObjectID chosen = *pending.begin();
        pending.erase(chosen);
        processed[chosen] = true;
        previous[chosen] = current[chosen];
        computeIndirect(chosen, current[chosen]);
        continue;
      }

      // Priority 1: unblocked non-singleton / weak object.
      ObjectID chosen = 0;
      for (ObjectID object : pending) {
        if (!canStrongUpdate(object) && !escapeBlocked(object)) {
          chosen = object;
          break;
        }
      }

      // Priority 2: unblocked strong singleton (prefer objects feeding an
      // unresolved indirect call, then objects not sharing a function with one).
      if (!chosen) {
        ObjectID fallbackUnblocked = 0;
        ObjectID deferredUnblocked = 0;
        for (ObjectID object : pending) {
          if (!canStrongUpdate(object) || escapeBlocked(object))
            continue;
          if (feedsUnresolvedCall(object)) {
            chosen = object;
            break;
          }
          if (!sharesFunctionWithUnresolvedCall(object)) {
            if (!fallbackUnblocked)
              fallbackUnblocked = object;
          } else if (!deferredUnblocked) {
            deferredUnblocked = object;
          }
        }
        if (!chosen)
          chosen = fallbackUnblocked ? fallbackUnblocked : deferredUnblocked;
      }

      if (chosen) {
        pending.erase(chosen);
        processed[chosen] = true;
        previous[chosen] = current[chosen];
        computeIndirect(chosen, current[chosen]);
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
        for (ObjectID object : weakBatch) {
          pending.erase(object);
          processed[object] = true;
          previous[object] = current[object];
        }
        for (ObjectID object : weakBatch)
          computeIndirect(object, current[object]);
        continue;
      }

      // Priority 4: only blocked strong singletons remain.
      breakBlockedSingletons();
    }

    for (const auto &sig : previous)
      finalStrong.insert(sig.strong.begin(), sig.strong.end());
    stats.strongUpdateSites = finalStrong.size();

    // Strip internal join nodes from public pointedToBy() sets.
    for (NodeSet &nodes : pted) {
      NodeSet publicNodes;
      for (NodeID node : nodes) {
        if (node >= p.edges_.size())
          break;
        publicNodes.insert(node);
      }
      nodes = std::move(publicNodes);
    }
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
  return impl_->pts.at(node);
}

const NodeSet &Solver::pointedToBy(ObjectID object) const {
  impl_->requireAnalyzed();
  if (!object || object > impl_->p.objectCount())
    throw std::out_of_range("ValueFlowPTA: invalid object");
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
  return impl_->edges.size() - 1;
}

bool Solver::isJoinNode(NodeID node) const {
  impl_->requireAnalyzed();
  if (!node || node >= impl_->edges.size())
    throw std::out_of_range("ValueFlowPTA: invalid VFG node");
  return node >= impl_->p.edges_.size();
}

const NodeSet &Solver::vfgSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  if (!node || node >= impl_->edges.size())
    throw std::out_of_range("ValueFlowPTA: invalid VFG node");
  return impl_->edges[node];
}

NodeSet Solver::vfgIndirectSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  if (!node || node >= impl_->edges.size())
    throw std::out_of_range("ValueFlowPTA: invalid VFG node");
  NodeSet indirect = impl_->edges[node];
  indirect.subtract(impl_->directEdges[node]);
  return indirect;
}

const std::set<FunctionID> &Solver::resolvedCallTargets(CallSiteID site) const {
  impl_->requireAnalyzed();
  impl_->p.checkCallSite(site);
  return impl_->liveCallTargets[site];
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
