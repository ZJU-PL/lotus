#include "Alias/InclusionBased/FlowSensitive/Versioned/VersionedFlowSensitivePTA.h"

#include "Alias/InclusionBased/FlowSensitive/Support/SVFGOrder.h"
#include "IR/ICFG/CallGraph.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <thread>

#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace lotus::analysis;

namespace lotus::alias {

namespace {
constexpr VersionedFlowSensitivePTA::NodeID InitialMeldToken = 1;

// Collect the edges incident to the actual nodes of \p callSite and the formal
// nodes of \p callee.
void collectCallsiteCalleeEdges(
    const SVFG &graph, const llvm::CallBase *callSite,
    const llvm::Function *callee,
    std::unordered_map<const SVFGEdge *, SVFGNodeBS> &edges) {
  auto collect = [&](const SVFGNodeSet &nodes) {
    for (const SVFGNode *node : nodes) {
      if (!node)
        continue;
      for (const SVFGEdge *edge : node->getInEdges())
        if (edge)
          edges.emplace(edge, edge->getPointsTo());
      for (const SVFGEdge *edge : node->getOutEdges())
        if (edge)
          edges.emplace(edge, edge->getPointsTo());
    }
  };
  collect(graph.getActualParms(callSite));
  collect(graph.getActualIns(callSite));
  collect(graph.getActualRets(callSite));
  collect(graph.getActualOuts(callSite));
  collect(graph.getFormalParms(callee));
  collect(graph.getFormalIns(callee));
  collect(graph.getFormalRets(callee));
  collect(graph.getFormalOuts(callee));
}
} // namespace

VersionedFlowSensitivePTA::VersionedFlowSensitivePTA(const SVFG &graph)
    : VersionedFlowSensitivePTA(graph, Config{}) {}

VersionedFlowSensitivePTA::VersionedFlowSensitivePTA(const SVFG &graph,
                                                     Config config)
    : graph_(&graph), config_(std::move(config)) {}

bool VersionedFlowSensitivePTA::inScope(const SVFGNode *node) const {
  return node && (!config_.scope || config_.scope->contains(node));
}

void VersionedFlowSensitivePTA::initializeRecursiveFunctions() {
  recursiveFunctions_.clear();
  if (const LTCallGraph *callGraph = graph_->getRefinedCallGraph()) {
    std::unordered_map<const Function *, int> index;
    std::unordered_map<const Function *, int> lowlink;
    std::unordered_set<const Function *> onStack;
    std::vector<const Function *> stack;
    int nextIndex = 0;
    std::function<void(const Function *)> visit =
        [&](const Function *function) {
          index[function] = lowlink[function] = nextIndex++;
          stack.push_back(function);
          onStack.insert(function);
          const LTCallGraphNode *node = (*callGraph)[function];
          for (const auto &edge : *node) {
            const Function *callee =
                edge.second ? edge.second->getFunction() : nullptr;
            if (!callee || callee->isDeclaration())
              continue;
            if (index.find(callee) == index.end()) {
              visit(callee);
              lowlink[function] = std::min(lowlink[function], lowlink[callee]);
            } else if (onStack.count(callee) != 0) {
              lowlink[function] = std::min(lowlink[function], index[callee]);
            }
          }
          if (lowlink[function] != index[function])
            return;
          std::vector<const Function *> component;
          do {
            const Function *member = stack.back();
            stack.pop_back();
            onStack.erase(member);
            component.push_back(member);
            if (member == function)
              break;
          } while (!stack.empty());
          bool recursive = component.size() > 1;
          if (!recursive && !component.empty()) {
            const LTCallGraphNode *single = (*callGraph)[component.front()];
            for (const auto &edge : *single)
              recursive |= edge.second == single;
          }
          if (recursive)
            recursiveFunctions_.insert(component.begin(), component.end());
        };
    for (const auto &entry : *callGraph)
      if (entry.first && !entry.first->isDeclaration() &&
          index.find(entry.first) == index.end())
        visit(entry.first);
    return;
  }

  Module *module = nullptr;
  for (const auto &[nodeID, node] : *graph_)
    if (const Function *function = node->getFunction()) {
      module = const_cast<Module *>(function->getParent());
      break;
    }
  if (!module)
    return;
  CallGraph callGraph(*module);
  for (scc_iterator<CallGraph *> it = scc_begin(&callGraph),
                                 end = scc_end(&callGraph);
       it != end; ++it) {
    const std::vector<CallGraphNode *> &component = *it;
    bool recursive = component.size() > 1;
    if (!recursive && component.size() == 1)
      for (const auto &edge : *component.front())
        recursive |= edge.second == component.front();
    if (recursive)
      for (CallGraphNode *node : component)
        if (node && node->getFunction())
          recursiveFunctions_.insert(node->getFunction());
  }
}

bool VersionedFlowSensitivePTA::recursionChangeAffectsStrongUpdates(
    const std::unordered_set<const llvm::Function *> &previous) const {
  if (recursiveFunctions_ == previous)
    return false;
  // A store in a recursive function is a weak update. If no strong-update
  // site sits in a function whose recursion status changed, the existing
  // labels remain valid and a whole-graph relabel is unnecessary.
  for (NodeID site : strongUpdateSites_) {
    const SVFGNode *node = graph_->getNode(site);
    const Function *function = node ? node->getFunction() : nullptr;
    if (!function)
      continue;
    if (previous.count(function) != recursiveFunctions_.count(function))
      return true;
  }
  return false;
}

void VersionedFlowSensitivePTA::demoteStrongUpdatesToWeak(
    const std::unordered_set<const llvm::Function *> &previous) {
  std::vector<NodeID> toDemote;
  for (NodeID site : strongUpdateSites_) {
    const SVFGNode *node = graph_->getNode(site);
    const Function *function = node ? node->getFunction() : nullptr;
    if (function && recursiveFunctions_.count(function) != 0 &&
        previous.count(function) == 0) {
      toDemote.push_back(site);
    }
  }
  for (NodeID site : toDemote) {
    strongUpdateSites_.erase(site);
    weakUpdateSites_.insert(site);
    ++stats_.incrementalDemotions;
    const auto *store = dyn_cast_or_null<StoreSVFGNode>(graph_->getNode(site));
    if (!store)
      continue;
    const Value *ptrOp = accessPointer(store->getInstruction());
    const PointsToSet targets =
        accessTargets(ptrOp, store->getMemoryPointsTo());
    for (ObjectID object : targets) {
      const Version consumed = getConsume(site, object);
      const Version yielded = getYield(site, object);
      if (consumed != InvalidVersion && yielded != InvalidVersion) {
        const PointsToSet incoming = versionedPointsTo(object, consumed);
        PointsToSet &destination = versionedPointsTo_[{object, yielded}];
        const std::size_t oldSize = destination.size();
        destination.insert(incoming.begin(), incoming.end());
        if (destination.size() != oldSize) {
          propagateVersion(object, yielded);
          enqueueDependentStatements(object, yielded);
        }
      }
    }
  }
}

bool VersionedFlowSensitivePTA::functionHasKnownCaller(
    const Function *function) const {
  if (!function)
    return false;
  if (const LTCallGraph *callGraph = graph_->getRefinedCallGraph())
    for (const auto &entry : *callGraph) {
      if (!entry.second)
        continue;
      for (const auto &edge : *entry.second)
        if (edge.second && edge.second->getFunction() == function)
          return true;
    }
  return false;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::expandIndirectObjects(
    const PointsToSet &objects) const {
  PointsToSet expanded = objects;
  for (ObjectID object : objects) {
    if (!graph_->isFieldInsensitiveObject(object))
      continue;
    const PointsToSet fields = graph_->getFieldObjects(object);
    expanded.insert(fields.begin(), fields.end());
  }
  return mapObjects(expanded);
}

VersionedFlowSensitivePTA::ObjectID
VersionedFlowSensitivePTA::mapObject(ObjectID object) const {
  return objectClustering_.empty() ? object : objectClustering_.map(object);
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::mapObjects(const PointsToSet &objects) const {
  return objectClustering_.empty() ? objects
                                   : objectClustering_.mapAll(objects);
}

void VersionedFlowSensitivePTA::buildObjectClustering(
    const PointsToSet &candidates) {
  objectClustering_ = {};
  stats_.clusteredObjects = 0;
  stats_.mergedObjectClusters = 0;
  switch (config_.clusterObjects) {
  case Config::ClusterMethod::None:
    return;
  case Config::ClusterMethod::Type:
    objectClustering_ = flow_sensitive::clusterObjectsByType(*graph_);
    break;
  case Config::ClusterMethod::Footprint: {
    // The versioned solver already computes this partition for label sharing
    // (objects whose access footprints are identical).  Promote it into the
    // object map so memory facts are keyed by one representative. This
    // coarsens the analysis, unlike SVF's identity-preserving ID clustering.
    //
    // Group first, merge afterwards: while footprints are being computed the
    // partition is still empty, so storeMayTarget()/intrinsicMayDefine() see
    // the raw object IDs rather than a half-built partition.
    std::vector<ObjectID> sorted(candidates.begin(), candidates.end());
    std::sort(sorted.begin(), sorted.end());
    std::map<std::vector<FootprintEntry>, ObjectID> owners;
    std::vector<std::pair<ObjectID, ObjectID>> merges;
    for (ObjectID object : sorted) {
      if (object == 0 || graph_->isUnknownObject(object) ||
          graph_->isConstantObject(object))
        continue;
      std::vector<FootprintEntry> footprint = versionFootprint(object);
      const auto owner = owners.find(footprint);
      if (owner == owners.end())
        owners.emplace(std::move(footprint), object);
      else
        merges.emplace_back(object, owner->second);
    }
    for (const auto &[object, representative] : merges)
      objectClustering_.merge(object, representative);
    break;
  }
  }
  stats_.clusteredObjects = objectClustering_.mergedObjects();
  stats_.mergedObjectClusters = objectClustering_.mergedClusters();
}

bool VersionedFlowSensitivePTA::edgeCarriesObject(const SVFGEdge &edge,
                                                  ObjectID object) const {
  if (!isIndirectVFGEdge(edge.getEdgeKind()) || edge.getPointsTo().empty())
    return false;
  for (ObjectID guarded : edge.getPointsTo()) {
    if (graph_->isUnknownObject(guarded))
      return true;
    if (guarded == object)
      return true;
    if (graph_->isFieldInsensitiveObject(guarded) &&
        graph_->getFieldObjects(guarded).count(object) != 0)
      return true;
  }
  return false;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::storeTargetObjects(
    const StoreSVFGNode &store) const {
  const Value *pointer = accessPointer(store.getInstruction());
  PointsToSet targets;
  if (config_.preAnalysisTargets && pointer) {
    // MemorySSA regions can use a coarser/different object partition. An
    // intersection can silently remove a feasible auxiliary-analysis target
    // and leave it without a yielded version at this store.
    targets = config_.preAnalysisTargets(pointer);
  }
  if (targets.empty())
    targets = store.getMemoryPointsTo();
  if (targets.empty() && pointer)
    targets = graph_->getObjectIds(pointer);
  return expandIndirectObjects(targets);
}

bool VersionedFlowSensitivePTA::storeMayTarget(const StoreSVFGNode &store,
                                               ObjectID object) const {
  const PointsToSet targets = storeTargetObjects(store);
  for (ObjectID target : targets)
    if (target == object || graph_->isUnknownObject(target))
      return true;
  return false;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::objectsWithFields(const Value *pointer) const {
  PointsToSet objects = pointerTargets(pointer);
  if (objects.empty() && pointer) {
    const PointsToSet &known = graph_->getObjectIds(pointer);
    objects.insert(known.begin(), known.end());
    if (ObjectID object = graph_->getObjectId(pointer))
      objects.insert(object);
  }
  PointsToSet expanded = objects;
  for (ObjectID object : objects) {
    const PointsToSet fields = graph_->getFieldObjects(object);
    expanded.insert(fields.begin(), fields.end());
  }
  return mapObjects(expanded);
}

bool VersionedFlowSensitivePTA::intrinsicMayDefine(
    const ActualOutSVFGNode &actualOut, ObjectID object) const {
  const auto *intrinsic =
      dyn_cast_or_null<IntrinsicInst>(actualOut.getCallSite());
  if (!intrinsic || intrinsic->arg_size() == 0)
    return false;
  switch (intrinsic->getIntrinsicID()) {
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
    break;
  default:
    return false;
  }
  const PointsToSet destinations =
      objectsWithFields(intrinsic->getArgOperand(0));
  return destinations.count(object) != 0 ||
         std::any_of(destinations.begin(), destinations.end(),
                     [&](ObjectID destination) {
                       return graph_->isUnknownObject(destination);
                     });
}

bool VersionedFlowSensitivePTA::memoryPhiNeedsInitial(
    const MSSAPhiSVFGNode &phi, ObjectID object) const {
  for (const SVFGEdge *edge : phi.getInEdges()) {
    const SVFGNode *source = edge ? edge->getSrcNode() : nullptr;
    if (isa_and_nonnull<EntryChiSVFGNode>(source))
      return true;
    if (const auto *formalIn = dyn_cast_or_null<FormalInSVFGNode>(source))
      if (!(isDeltaNode(source->getId()) &&
            deltaCarriesObject(*source, object)) &&
          !functionHasKnownCaller(formalIn->getFunction()))
        return true;
  }
  const ICFGNode *icfgNode = phi.getICFGNode();
  const BasicBlock *block = icfgNode ? icfgNode->getBasicBlock() : nullptr;
  if (!block)
    return false;
  const unsigned predecessorCount =
      static_cast<unsigned>(std::distance(pred_begin(block), pred_end(block)));
  return phi.getOpVerNum() < predecessorCount;
}

bool VersionedFlowSensitivePTA::isDeltaNode(NodeID node) const {
  return deltaNodes_.count(node) != 0;
}

bool VersionedFlowSensitivePTA::isDeltaSource(NodeID node) const {
  return deltaSourceNodes_.count(node) != 0;
}

bool VersionedFlowSensitivePTA::deltaCarriesObject(const SVFGNode &node,
                                                   ObjectID object) const {
  const auto *memoryNode = dyn_cast<MSSASVFGNode>(&node);
  if (!memoryNode)
    return false;
  const PointsToSet regions =
      expandIndirectObjects(memoryNode->getDefSVFVars());
  return regions.count(object) != 0 ||
         std::any_of(regions.begin(), regions.end(), [&](ObjectID region) {
           return graph_->isUnknownObject(region);
         });
}

void VersionedFlowSensitivePTA::buildDeltaMaps() {
  // SVF's buildDeltaMaps().  A delta node may gain new incoming indirect
  // edges while the solver is running (on-the-fly call-graph construction):
  //   * a function entry may gain edges from newly discovered indirect
  //     callsites, and
  //   * a callsite return of an indirect call may gain edges from newly
  //     discovered callees.
  // Because prelabelling gives these nodes a dedicated version, materialising
  // such an edge only ever adds a reliance; it never requires relabelling.
  deltaNodes_.clear();
  deltaSourceNodes_.clear();
  std::unordered_set<const CallBase *> deltaCallSites;
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    bool delta = false;
    if (const Function *callee = graph_->isFunEntrySVFGNode(node)) {
      const auto &callSites = graph_->getIndCallSitesInvokingCallee(callee);
      delta = !callSites.empty();
      deltaCallSites.insert(callSites.begin(), callSites.end());
    } else if (const CallBase *callSite = graph_->isCallSiteRetSVFGNode(node)) {
      delta = callSite->getCalledFunction() == nullptr;
      if (delta)
        deltaCallSites.insert(callSite);
    }
    if (delta)
      deltaNodes_.insert(nodeID);
  }

  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    // Actual-in nodes of a delta callsite feed the formal-ins of a newly
    // discovered callee; every formal-out feeds the actual-outs of a newly
    // discovered callsite.  SVF over-approximates this by marking every node
    // whose ICFG node is a delta call block node.
    if (const auto *actualIn = dyn_cast<ActualInSVFGNode>(node))
      if (deltaCallSites.count(actualIn->getCallSite()) != 0)
        deltaSourceNodes_.insert(nodeID);
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      if (deltaCallSites.count(actualOut->getCallSite()) != 0)
        deltaSourceNodes_.insert(nodeID);
    if (isa<FormalOutSVFGNode>(node))
      deltaSourceNodes_.insert(nodeID);
  }
  stats_.deltaNodes = deltaNodes_.size();
  stats_.deltaSourceNodes = deltaSourceNodes_.size();
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::relevantObjects() const {
  PointsToSet objects;
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node)) {
      PointsToSet targets = storeTargetObjects(*store);
      objects.insert(targets.begin(), targets.end());
    }
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node)) {
      const auto *intrinsic =
          dyn_cast_or_null<IntrinsicInst>(actualOut->getCallSite());
      if (intrinsic && intrinsic->arg_size() != 0 &&
          (intrinsic->getIntrinsicID() == Intrinsic::memcpy ||
           intrinsic->getIntrinsicID() == Intrinsic::memmove ||
           intrinsic->getIntrinsicID() == Intrinsic::memset)) {
        // Intrinsics can be the first definition of a local object. Those
        // destinations have no Store node or global initializer to prelabel
        // them, but still need consume/yield versions for their written data.
        const PointsToSet destinations =
            objectsWithFields(intrinsic->getArgOperand(0));
        objects.insert(destinations.begin(), destinations.end());
        const PointsToSet definitions =
            expandIndirectObjects(actualOut->getDefSVFVars());
        objects.insert(definitions.begin(), definitions.end());
      }
    }
    if (!isDeltaNode(nodeID))
      continue;
    if (const auto *mssa = dyn_cast<MSSASVFGNode>(node)) {
      const PointsToSet definitions =
          expandIndirectObjects(mssa->getDefSVFVars());
      objects.insert(definitions.begin(), definitions.end());
    }
  }

  // Global initialisers write memory without a store node, so their storage
  // objects need a version for initializeGlobalMemory() to fill.
  const Module *module = nullptr;
  for (const auto &[nodeID, node] : *graph_)
    if (const Function *function = node->getFunction()) {
      module = function->getParent();
      break;
    }
  if (module)
    for (const GlobalVariable &global : module->globals()) {
      if (!global.hasInitializer())
        continue;
      PointsToSet bases;
      if (ObjectID object = graph_->getObjectId(&global))
        bases.insert(object);
      if (bases.empty()) {
        const PointsToSet &known = graph_->getObjectIds(&global);
        bases.insert(known.begin(), known.end());
      }
      for (ObjectID base : bases) {
        objects.insert(base);
        const PointsToSet fields = graph_->getFieldObjects(base);
        objects.insert(fields.begin(), fields.end());
      }
    }
  return objects;
}

bool VersionedFlowSensitivePTA::addReliance(ObjectID object, Version source,
                                            Version destination) {
  if (source == InvalidVersion || destination == InvalidVersion ||
      source == destination)
    return false;
  auto &destinations = versionReliance_[mapObject(object)][source];
  if (std::find(destinations.begin(), destinations.end(), destination) ==
      destinations.end()) {
    destinations.push_back(destination);
    return true;
  }
  return false;
}

void VersionedFlowSensitivePTA::buildObjectIndex() {
  objectIndex_.clear();
  allNodes_.clear();
  statementNodes_.clear();
  storeNodes_.clear();
  actualOutNodes_.clear();
  memoryPhiNodes_.clear();
  wildcardEdges_.clear();

  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    allNodes_.push_back(node);

    if (isa<StoreSVFGNode>(node))
      storeNodes_.push_back(dyn_cast<StoreSVFGNode>(node));
    if (isa<ActualOutSVFGNode>(node))
      actualOutNodes_.push_back(dyn_cast<ActualOutSVFGNode>(node));
    if (isa<MSSAPhiSVFGNode>(node))
      memoryPhiNodes_.push_back(dyn_cast<MSSAPhiSVFGNode>(node));
    if (isa<LoadSVFGNode, StoreSVFGNode, ActualOutSVFGNode>(node))
      statementNodes_.push_back(node);

    // Delta nodes are prelabelled seeds: their def set tells which objects
    // gain a dedicated version at this location (SVF: prelabel()).
    // (computeObjectLabels() seeds them from deltaNodes_ directly.)

    for (const SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || !inScope(edge->getDstNode()))
        continue;
      if (!isIndirectVFGEdge(edge->getEdgeKind()) ||
          edge->getPointsTo().empty())
        continue;
      // A wildcard guard makes the edge carry every object
      // (edgeCarriesObject() returns true for all of them), so it cannot be
      // indexed precisely.
      bool wildcard = false;
      for (ObjectID guarded : edge->getPointsTo())
        if (graph_->isUnknownObject(guarded))
          wildcard = true;
      if (wildcard) {
        wildcardEdges_.push_back(edge);
        continue;
      }
      for (ObjectID object : expandIndirectObjects(edge->getPointsTo()))
        objectIndex_[object].push_back(edge);
    }
  }
}

VersionedFlowSensitivePTA::ObjectID
VersionedFlowSensitivePTA::fieldInsensitiveRepresentative(ObjectID base) {
  if (base == 0)
    return 0;
  auto cached = fieldInsensitiveObjects_.find(base);
  if (cached != fieldInsensitiveObjects_.end())
    return cached->second;
  // Object metadata is the only graph state mutated here; the on-the-fly
  // call-graph path already mutates the graph in the same way.
  auto &mutableGraph = const_cast<SVFG &>(*graph_);
  const ObjectID existing = mutableGraph.getFieldInsensitiveObject(base);
  const ObjectID representative =
      mutableGraph.getOrCreateFieldInsensitiveObject(base);
  if (existing == 0 && representative != 0)
    ++stats_.fieldInsensitiveCollapses;
  fieldInsensitiveObjects_[base] = representative;
  return representative;
}

void VersionedFlowSensitivePTA::computeObjectLabels(
    ObjectID object, ObjectLabels &labels) const {
  const auto indexIt = objectIndex_.find(object);
  const std::vector<const SVFGEdge *> *indexedEdges =
      indexIt == objectIndex_.end() ? nullptr : &indexIt->second;

  // Adjacency over the edges which carry this object, built once instead of
  // rescanning every out-edge of every node for each object.
  std::unordered_map<NodeID, std::vector<const SVFGEdge *>> adjacency;
  std::unordered_set<NodeID> hasIncoming;
  auto addEdge = [&](const SVFGEdge *edge) {
    adjacency[edge->getSrcNode()->getId()].push_back(edge);
    hasIncoming.insert(edge->getDstNode()->getId());
  };
  if (indexedEdges)
    for (const SVFGEdge *edge : *indexedEdges)
      addEdge(edge);
  for (const SVFGEdge *edge : wildcardEdges_)
    addEdge(edge);

  // 1. Identify starting nodes and boundary nodes
  std::vector<const SVFGNode *> startingNodes;
  std::unordered_set<NodeID> isStarting;
  auto addStartingNode = [&](const SVFGNode *node) {
    if (node && isStarting.insert(node->getId()).second)
      startingNodes.push_back(node);
  };

  auto isBoundaryNode = [&](NodeID nodeID) -> bool {
    const SVFGNode *node = graph_->getNode(nodeID);
    if (!node)
      return false;
    if (isDeltaNode(nodeID) && deltaCarriesObject(*node, object))
      return true;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node))
      return storeMayTarget(*store, object);
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      return intrinsicMayDefine(*actualOut, object);
    return false;
  };

  for (const SVFGNode *node : allNodes_) {
    const NodeID nodeID = node->getId();
    const auto *memoryPhi = dyn_cast<MSSAPhiSVFGNode>(node);
    const bool deltaNode =
        isDeltaNode(nodeID) && deltaCarriesObject(*node, object);
    if (deltaNode) {
      addStartingNode(node);
    } else if (!hasIncoming.count(nodeID) ||
               (memoryPhi && memoryPhiNeedsInitial(*memoryPhi, object))) {
      addStartingNode(node);
    }
    bool defines = false;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node))
      defines = storeMayTarget(*store, object);
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      defines |= intrinsicMayDefine(*actualOut, object);
    if (defines)
      addStartingNode(node);
  }

  // 2. Tarjan SCC on the overlay graph
  std::unordered_map<NodeID, int> partOf;
  std::unordered_map<NodeID, int> index;
  std::unordered_map<NodeID, int> lowlink;
  std::unordered_set<NodeID> onStack;
  std::vector<NodeID> stack;
  int currentIndex = 0;
  int currentSCC = 0;

  struct Frame {
    NodeID u;
    std::size_t edgeIdx;
  };
  std::vector<Frame> callStack;

  for (const SVFGNode *startNode : startingNodes) {
    NodeID root = startNode->getId();
    if (index.count(root) != 0)
      continue;

    index[root] = lowlink[root] = currentIndex++;
    stack.push_back(root);
    onStack.insert(root);
    callStack.push_back({root, 0});

    while (!callStack.empty()) {
      Frame &top = callStack.back();
      NodeID u = top.u;
      auto adjIt = adjacency.find(u);
      const auto *edges = (adjIt != adjacency.end()) ? &adjIt->second : nullptr;

      if (edges && top.edgeIdx < edges->size()) {
        const SVFGEdge *edge = (*edges)[top.edgeIdx++];
        const SVFGNode *dstNode = edge->getDstNode();
        NodeID v = dstNode->getId();

        if (isBoundaryNode(v))
          continue;

        if (index.count(v) == 0) {
          index[v] = lowlink[v] = currentIndex++;
          stack.push_back(v);
          onStack.insert(v);
          callStack.push_back({v, 0});
        } else if (onStack.count(v) != 0) {
          lowlink[u] = std::min(lowlink[u], index[v]);
        }
      } else {
        if (lowlink[u] == index[u]) {
          while (true) {
            NodeID w = stack.back();
            stack.pop_back();
            onStack.erase(w);
            partOf[w] = currentSCC;
            if (w == u)
              break;
          }
          ++currentSCC;
        }
        callStack.pop_back();
        if (!callStack.empty()) {
          NodeID p = callStack.back().u;
          lowlink[p] = std::min(lowlink[p], lowlink[u]);
        }
      }
    }
  }

  // 3. Topological sort of reached nodes
  std::vector<NodeID> todoList;
  todoList.reserve(partOf.size());
  for (const auto &[nodeID, scc] : partOf)
    todoList.push_back(nodeID);

  std::sort(todoList.begin(), todoList.end(),
            [&](NodeID a, NodeID b) { return partOf[a] > partOf[b]; });

  // 4. Initialize meld sets
  std::vector<MeldSet> sccConsumedMeld(currentSCC);
  std::unordered_map<NodeID, MeldSet> storesYieldedMeld;

  for (NodeID n : todoList) {
    const SVFGNode *node = graph_->getNode(n);
    if (!node)
      continue;
    const auto *memoryPhi = dyn_cast<MSSAPhiSVFGNode>(node);
    const bool deltaNode = isDeltaNode(n) && deltaCarriesObject(*node, object);
    if (deltaNode) {
      sccConsumedMeld[partOf[n]].insert(n + 2);
    } else if (!hasIncoming.count(n) ||
               (memoryPhi && memoryPhiNeedsInitial(*memoryPhi, object))) {
      sccConsumedMeld[partOf[n]].insert(InitialMeldToken);
    }
    bool defines = false;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node))
      defines = storeMayTarget(*store, object);
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      defines |= intrinsicMayDefine(*actualOut, object);
    if (defines)
      storesYieldedMeld[n].insert(n + 2);
  }

  // 5. Single forward topological pass
  for (NodeID n : todoList) {
    const SVFGNode *node = graph_->getNode(n);
    if (!node)
      continue;
    bool definesNewVersion = false;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node))
      definesNewVersion = storeMayTarget(*store, object);
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      definesNewVersion |= intrinsicMayDefine(*actualOut, object);

    const MeldSet &nYield = definesNewVersion ? storesYieldedMeld[n]
                                              : sccConsumedMeld[partOf[n]];
    if (nYield.empty())
      continue;

    auto adjIt = adjacency.find(n);
    if (adjIt == adjacency.end())
      continue;

    for (const SVFGEdge *edge : adjIt->second) {
      const SVFGNode *dst = edge->getDstNode();
      NodeID dstID = dst->getId();
      if (isDeltaNode(dstID) && deltaCarriesObject(*dst, object))
        continue;
      auto dstPartOfIt = partOf.find(dstID);
      if (dstPartOfIt == partOf.end())
        continue;
      int dstSCC = dstPartOfIt->second;
      if (dstSCC != partOf[n] || definesNewVersion) {
        sccConsumedMeld[dstSCC].insert(nYield.begin(), nYield.end());
      }
    }
  }

  std::unordered_map<NodeID, MeldSet> consumedMeld;
  std::unordered_map<NodeID, MeldSet> yieldedMeld;

  for (NodeID n : todoList) {
    const SVFGNode *node = graph_->getNode(n);
    if (!node)
      continue;
    bool definesNewVersion = false;
    if (const auto *store = dyn_cast<StoreSVFGNode>(node))
      definesNewVersion = storeMayTarget(*store, object);
    if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node))
      definesNewVersion |= intrinsicMayDefine(*actualOut, object);

    if (!sccConsumedMeld[partOf[n]].empty())
      consumedMeld[n] = sccConsumedMeld[partOf[n]];
    if (definesNewVersion)
      yieldedMeld[n] = storesYieldedMeld[n];
    else if (!sccConsumedMeld[partOf[n]].empty())
      yieldedMeld[n] = sccConsumedMeld[partOf[n]];
  }

  MeldSet initialMeld;
  initialMeld.insert(InitialMeldToken);
  labels.initialVersion = labels.intern(initialMeld);
  for (const SVFGNode *node : allNodes_) {
    const NodeID nodeID = node->getId();
    if (!consumedMeld[nodeID].empty()) {
      const Version consumed = labels.intern(consumedMeld[nodeID]);
      labels.consume[nodeID] = consumed;
      const auto *memoryPhi = dyn_cast<MSSAPhiSVFGNode>(node);
      if (memoryPhi && memoryPhiNeedsInitial(*memoryPhi, object) &&
          consumedMeld[nodeID].count(InitialMeldToken) != 0)
        labels.addReliance(labels.initialVersion, consumed);
    }
    if (!yieldedMeld[nodeID].empty())
      labels.yield[nodeID] = labels.intern(yieldedMeld[nodeID]);
  }

  // Yield->consume reliances for every edge which carries this object.
  auto addEdgeReliance = [&](const SVFGEdge *edge) {
    labels.addReliance(labels.yieldAt(edge->getSrcNode()->getId()),
                       labels.consumeAt(edge->getDstNode()->getId()));
  };
  if (indexedEdges)
    for (const SVFGEdge *edge : *indexedEdges)
      addEdgeReliance(edge);
  for (const SVFGEdge *edge : wildcardEdges_)
    addEdgeReliance(edge);

  for (const SVFGNode *node : statementNodes_) {
    const NodeID nodeID = node->getId();
    const Version consumed = labels.consumeAt(nodeID);
    if (consumed == InvalidVersion)
      continue;
    auto &statements = labels.statements[consumed];
    if (std::find(statements.begin(), statements.end(), nodeID) ==
        statements.end())
      statements.push_back(nodeID);
  }

  // Intrinsic actual-outs read memory which is not in their def set (the
  // memcpy source and the destination as it was before the call), so they
  // must be re-evaluated whenever any version of a touched object changes.
  for (const SVFGNode *node : actualOutNodes_) {
    const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node);
    if (!actualOut)
      continue;
    const auto *intrinsic =
        dyn_cast_or_null<IntrinsicInst>(actualOut->getCallSite());
    if (!intrinsic || intrinsic->arg_size() < 2)
      continue;
    const Intrinsic::ID intrinsicId = intrinsic->getIntrinsicID();
    if (intrinsicId != Intrinsic::memcpy && intrinsicId != Intrinsic::memmove &&
        intrinsicId != Intrinsic::memset)
      continue;
    bool touchesObject = false;
    for (unsigned operand = 0; operand < 2; ++operand)
      if (objectsWithFields(intrinsic->getArgOperand(operand)).count(object) !=
          0)
        touchesObject = true;
    if (touchesObject)
      labels.intrinsicDependents.insert(node->getId());
  }
}

void VersionedFlowSensitivePTA::mergeObjectLabels(ObjectID object,
                                                  ObjectLabels &&labels) {
  initialVersion_[object] = labels.initialVersion;
  versionMelds_[object] = std::move(labels.melds);
  for (const auto &[location, version] : labels.consume)
    consume_[location][object] = version;
  for (const auto &[location, version] : labels.yield)
    yield_[location][object] = version;
  if (!labels.reliance.empty())
    versionReliance_[object] = std::move(labels.reliance);
  if (!labels.statements.empty())
    statementReliance_[object] = std::move(labels.statements);
  if (!labels.intrinsicDependents.empty())
    intrinsicDependents_[object] = std::move(labels.intrinsicDependents);
}

void VersionedFlowSensitivePTA::labelObject(ObjectID object) {
  ObjectLabels labels;
  computeObjectLabels(object, labels);
  mergeObjectLabels(object, std::move(labels));
}

std::vector<VersionedFlowSensitivePTA::FootprintEntry>
VersionedFlowSensitivePTA::versionFootprint(ObjectID object) const {
  std::vector<FootprintEntry> footprint;
  const auto indexIt = objectIndex_.find(object);
  if (indexIt != objectIndex_.end())
    for (const SVFGEdge *edge : indexIt->second)
      footprint.push_back(
          {4, edge->getSrcNode()->getId(), edge->getDstNode()->getId()});
  for (const SVFGEdge *edge : wildcardEdges_)
    footprint.push_back(
        {4, edge->getSrcNode()->getId(), edge->getDstNode()->getId()});
  for (const StoreSVFGNode *store : storeNodes_)
    if (storeMayTarget(*store, object))
      footprint.push_back({1, store->getId(), store->getId()});
  for (const ActualOutSVFGNode *actualOut : actualOutNodes_)
    if (intrinsicMayDefine(*actualOut, object))
      footprint.push_back({2, actualOut->getId(), actualOut->getId()});
  for (const MSSAPhiSVFGNode *memoryPhi : memoryPhiNodes_)
    if (memoryPhiNeedsInitial(*memoryPhi, object))
      footprint.push_back({3, memoryPhi->getId(), memoryPhi->getId()});
  for (NodeID nodeID : deltaNodes_) {
    const SVFGNode *node = graph_->getNode(nodeID);
    if (node && deltaCarriesObject(*node, object))
      footprint.push_back({5, nodeID, nodeID});
  }
  std::sort(footprint.begin(), footprint.end());
  return footprint;
}

void VersionedFlowSensitivePTA::copyVersionLabels(ObjectID canonical,
                                                  ObjectID object) {
  std::vector<std::pair<NodeID, Version>> consumed;
  std::vector<std::pair<NodeID, Version>> yielded;
  for (const auto &[location, objects] : consume_) {
    auto it = objects.find(canonical);
    if (it != objects.end())
      consumed.emplace_back(location, it->second);
  }
  for (const auto &[location, objects] : yield_) {
    auto it = objects.find(canonical);
    if (it != objects.end())
      yielded.emplace_back(location, it->second);
  }
  for (const auto &[location, version] : consumed)
    consume_[location][object] = version;
  for (const auto &[location, version] : yielded)
    yield_[location][object] = version;
  initialVersion_[object] = initialVersion_[canonical];
  versionMelds_[object] = versionMelds_[canonical];
  versionReliance_[object] = versionReliance_[canonical];
  statementReliance_[object] = statementReliance_[canonical];
  intrinsicDependents_[object] = intrinsicDependents_[canonical];
}

void VersionedFlowSensitivePTA::buildVersionProcessingOrder() {
  versionProcessingOrder_.clear();
  versionProcessingOrder_.reserve(versionMelds_.size());
  for (const auto &[object, melds] : versionMelds_)
    versionProcessingOrder_.push_back(object);
  // Precompute each object's occurrence weight once; computing it inside the
  // comparator makes ordering O(n log n * |consume_|).
  std::unordered_map<ObjectID, std::size_t> weights;
  weights.reserve(versionMelds_.size());
  for (const auto &[object, melds] : versionMelds_)
    weights.emplace(object, std::size_t{1});
  for (const auto &[object, reliance] : versionReliance_) {
    auto weightIt = weights.find(object);
    if (weightIt == weights.end())
      continue;
    for (const auto &entry : reliance)
      weightIt->second += 1 + entry.second.size();
  }
  for (const auto &[location, objects] : consume_)
    for (const auto &entry : objects) {
      auto weightIt = weights.find(entry.first);
      if (weightIt != weights.end())
        ++weightIt->second;
    }
  auto occurrenceWeight = [&](ObjectID object) {
    auto weightIt = weights.find(object);
    return weightIt == weights.end() ? std::size_t{0} : weightIt->second;
  };
  if (config_.orderVersionedObjectsByWeight)
    std::stable_sort(versionProcessingOrder_.begin(),
                     versionProcessingOrder_.end(),
                     [&](ObjectID lhs, ObjectID rhs) {
                       const std::size_t leftWeight = occurrenceWeight(lhs);
                       const std::size_t rightWeight = occurrenceWeight(rhs);
                       return leftWeight != rightWeight
                                  ? leftWeight > rightWeight
                                  : lhs < rhs;
                     });
  else
    std::sort(versionProcessingOrder_.begin(), versionProcessingOrder_.end());
}

void VersionedFlowSensitivePTA::buildVersionLabels() {
  consume_.clear();
  yield_.clear();
  versionReliance_.clear();
  statementReliance_.clear();
  initialVersion_.clear();
  versionMelds_.clear();
  equivalentObject_.clear();
  labelledObjects_.clear();
  intrinsicDependents_.clear();
  versionPropagationQueue_ = {};
  queuedVersionPropagations_.clear();
  stats_.equivalentObjects = 0;
  buildDeltaMaps();
  buildObjectIndex();
  // Compute the partition from the unmapped object set so a relabel after a
  // topology change never compounds a previous partition.
  objectClustering_ = {};
  const PointsToSet candidates = relevantObjects();
  buildObjectClustering(candidates);
  if (!objectClustering_.empty()) {
    // The first index was built with raw object IDs and is what clustering
    // needed; labelling must see representatives, and an edge whose guard names
    // a merged member must be indexed under that member's representative.
    buildObjectIndex();
  }
  const PointsToSet objects = mapObjects(candidates);
  // Objects with identical footprints share version labels. Compute one
  // representative per footprint, label the representatives concurrently,
  // then merge them in object order so the result does not depend on timing.
  std::vector<ObjectID> representatives;
  std::vector<std::pair<ObjectID, ObjectID>> equivalents;
  std::map<std::vector<FootprintEntry>, ObjectID> footprintOwners;
  representatives.reserve(objects.size());
  for (ObjectID object : objects) {
    labelledObjects_.insert(object);
    std::vector<FootprintEntry> footprint = versionFootprint(object);
    auto owner = footprintOwners.find(footprint);
    if (owner == footprintOwners.end()) {
      equivalentObject_[object] = object;
      footprintOwners.emplace(std::move(footprint), object);
      representatives.push_back(object);
    } else {
      equivalentObject_[object] = owner->second;
      equivalents.emplace_back(object, owner->second);
    }
  }

  std::vector<ObjectLabels> labels(representatives.size());
  auto computeLabels = [&](std::size_t index) {
    computeObjectLabels(representatives[index], labels[index]);
  };
  std::size_t threadCount = config_.versioningThreads;
  if (threadCount == 0)
    threadCount = std::max(1u, std::thread::hardware_concurrency());
  threadCount = std::min(threadCount, representatives.size());
  if (threadCount <= 1) {
    for (std::size_t index = 0; index < representatives.size(); ++index)
      computeLabels(index);
  } else {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t worker = 0; worker < threadCount; ++worker)
      workers.emplace_back([&]() {
        for (std::size_t index = next.fetch_add(1);
             index < representatives.size(); index = next.fetch_add(1)) {
          computeLabels(index);
        }
      });
    for (std::thread &worker : workers)
      worker.join();
  }

  for (std::size_t index = 0; index < representatives.size(); ++index)
    mergeObjectLabels(representatives[index], std::move(labels[index]));
  for (const auto &[object, representative] : equivalents)
    copyVersionLabels(representative, object);

  stats_.equivalentObjects = equivalents.size();
  stats_.versionedObjects = objects.size();
  stats_.versions = 0;
  for (const auto &[object, melds] : versionMelds_)
    stats_.versions += melds.size();
  buildVersionProcessingOrder();
  ++stats_.relabelings;
}

bool VersionedFlowSensitivePTA::labelObjectIncremental(ObjectID object) {
  // SVF's prelabelling covers every object which the pre-analysis sees, so a
  // missing label simply means "this object carries no facts here" and the
  // delta edge is skipped.  Graphs built without a pre-analysis (unit tests,
  // hand-built SVFGs) can still surface an object on a newly connected edge
  // that was never part of relevantObjects(); label just that object instead
  // of rebuilding the whole graph.
  if (object == 0 || !labelledObjects_.insert(mapObject(object)).second)
    return false;
  object = mapObject(object);
  equivalentObject_[object] = object;
  // The graph changed since the last labelling pass (this is the on-the-fly
  // path), so refresh the object->edge index before labelling.
  buildObjectIndex();
  labelObject(object);
  ++stats_.deltaRelabelings;
  buildVersionProcessingOrder();
  return true;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::getConsume(NodeID location, ObjectID object) const {
  auto locationIt = consume_.find(location);
  if (locationIt == consume_.end())
    return InvalidVersion;
  auto objectIt = locationIt->second.find(mapObject(object));
  return objectIt == locationIt->second.end() ? InvalidVersion
                                              : objectIt->second;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::getYield(NodeID location, ObjectID object) const {
  auto locationIt = yield_.find(location);
  if (locationIt == yield_.end())
    return InvalidVersion;
  auto objectIt = locationIt->second.find(mapObject(object));
  return objectIt == locationIt->second.end() ? InvalidVersion
                                              : objectIt->second;
}

VersionedFlowSensitivePTA::Version
VersionedFlowSensitivePTA::initialVersion(ObjectID object) const {
  auto it = initialVersion_.find(mapObject(object));
  return it == initialVersion_.end() ? InvalidVersion : it->second;
}

const VersionedFlowSensitivePTA::PointsToSet &
VersionedFlowSensitivePTA::versionedPointsTo(ObjectID object,
                                             Version version) const {
  static const PointsToSet empty;
  auto it = versionedPointsTo_.find({mapObject(object), version});
  return it == versionedPointsTo_.end() ? empty : it->second;
}

const std::vector<VersionedFlowSensitivePTA::Version> &
VersionedFlowSensitivePTA::getReliantVersions(ObjectID object,
                                              Version version) const {
  static const std::vector<Version> empty;
  auto objectIt = versionReliance_.find(mapObject(object));
  if (objectIt == versionReliance_.end())
    return empty;
  auto versionIt = objectIt->second.find(version);
  return versionIt == objectIt->second.end() ? empty : versionIt->second;
}

VersionedFlowSensitivePTA::ObjectID
VersionedFlowSensitivePTA::canonicalVersionObject(ObjectID object) const {
  auto it = equivalentObject_.find(object);
  return it == equivalentObject_.end() ? object : it->second;
}

VersionedFlowSensitivePTA::ObjectID
VersionedFlowSensitivePTA::canonicalObject(ObjectID object) const {
  const auto it = loadedCanonicalObjects_.find(object);
  if (it != loadedCanonicalObjects_.end())
    return it->second;
  return graph_ ? graph_->canonicalObjectId(object) : object;
}

const std::vector<VersionedFlowSensitivePTA::NodeID> &
VersionedFlowSensitivePTA::getDependentStatements(ObjectID object,
                                                  Version version) const {
  static const std::vector<NodeID> empty;
  auto objectIt = statementReliance_.find(mapObject(object));
  if (objectIt == statementReliance_.end())
    return empty;
  auto versionIt = objectIt->second.find(version);
  return versionIt == objectIt->second.end() ? empty : versionIt->second;
}

bool VersionedFlowSensitivePTA::writeVersionedAnalysisResultToFile(
    const std::string &filename) const {
  std::ofstream output(filename, std::ios::trunc);
  if (!output)
    return false;
  output << "LOTUS_VFSPTA 2\n";
  output << "U " << (stats_.svfgHasSU ? 1 : 0) << '\n';
  for (const auto &[location, objects] : consume_)
    for (const auto &[object, version] : objects)
      output << "C " << location << ' ' << object << ' ' << version << '\n';
  for (const auto &[location, objects] : yield_)
    for (const auto &[object, version] : objects)
      output << "Y " << location << ' ' << object << ' ' << version << '\n';
  for (const auto &[object, version] : initialVersion_)
    output << "I " << object << ' ' << version << '\n';
  for (const auto &[object, canonical] : equivalentObject_)
    output << "Q " << object << ' ' << canonical << '\n';
  // SVF has no separate field-insensitive object (getFIObjVar returns the
  // base); record the Lotus identity map for reporting.
  for (ObjectID object : graph_->getAllObjectIds()) {
    const ObjectID canonical = graph_->canonicalObjectId(object);
    if (canonical != object)
      output << "F " << object << ' ' << canonical << '\n';
  }
  for (const auto &[object, melds] : versionMelds_)
    for (std::size_t index = 0; index < melds.size(); ++index) {
      output << "M " << object << ' ' << index + 1 << ' '
             << melds[index].size();
      for (NodeID token : melds[index])
        output << ' ' << token;
      output << '\n';
    }
  for (const auto &[key, pointsToSet] : versionedPointsTo_) {
    output << "P " << key.object << ' ' << key.version << ' '
           << pointsToSet.size();
    for (ObjectID target : pointsToSet)
      output << ' ' << target;
    output << '\n';
  }
  for (const auto &[node, pointsToSet] : topLevelPointsTo_) {
    output << "T " << node << ' ' << pointsToSet.size();
    for (ObjectID target : pointsToSet)
      output << ' ' << target;
    output << '\n';
  }
  for (const auto &[object, sources] : versionReliance_)
    for (const auto &[source, destinations] : sources) {
      output << "R " << object << ' ' << source << ' ' << destinations.size();
      for (Version destination : destinations)
        output << ' ' << destination;
      output << '\n';
    }
  for (const auto &[object, versions] : statementReliance_)
    for (const auto &[version, statements] : versions) {
      output << "D " << object << ' ' << version << ' ' << statements.size();
      for (NodeID statement : statements)
        output << ' ' << statement;
      output << '\n';
    }
  output << "E\n";
  return output.good();
}

bool VersionedFlowSensitivePTA::readVersionedAnalysisResultFromFile(
    const std::string &filename) {
  std::ifstream input(filename);
  std::string magic;
  unsigned formatVersion = 0;
  if (!(input >> magic >> formatVersion) || magic != "LOTUS_VFSPTA" ||
      (formatVersion != 1 && formatVersion != 2))
    return false;

  topLevelPointsTo_.clear();
  versionedPointsTo_.clear();
  consume_.clear();
  yield_.clear();
  versionReliance_.clear();
  statementReliance_.clear();
  initialVersion_.clear();
  versionMelds_.clear();
  equivalentObject_.clear();
  labelledObjects_.clear();
  deltaNodes_.clear();
  deltaSourceNodes_.clear();
  strongUpdateSites_.clear();
  weakUpdateSites_.clear();
  versionPropagationQueue_ = {};
  queuedVersionPropagations_.clear();
  statementWorklist_ = {};
  queuedStatements_.clear();
  sccComponentOf_.clear();
  loadedCanonicalObjects_.clear();
  stats_ = {};

  auto readSet = [&](std::size_t count, PointsToSet &set) {
    for (std::size_t i = 0; i < count; ++i) {
      ObjectID value = 0;
      if (!(input >> value))
        return false;
      set.insert(value);
    }
    return true;
  };
  std::string record;
  while (input >> record) {
    if (record == "E")
      break;
    if (record == "U") {
      unsigned hasSU = 0;
      if (!(input >> hasSU))
        return false;
      stats_.svfgHasSU = hasSU != 0;
    } else if (record == "C" || record == "Y") {
      NodeID location = 0;
      ObjectID object = 0;
      Version version = InvalidVersion;
      if (!(input >> location >> object >> version))
        return false;
      (record == "C" ? consume_ : yield_)[location][object] = version;
    } else if (record == "I") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      if (!(input >> object >> version))
        return false;
      initialVersion_[object] = version;
    } else if (record == "Q") {
      ObjectID object = 0;
      ObjectID canonical = 0;
      if (!(input >> object >> canonical))
        return false;
      equivalentObject_[object] = canonical;
    } else if (record == "M") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> version >> count) || version == InvalidVersion)
        return false;
      auto &melds = versionMelds_[object];
      if (melds.size() < version)
        melds.resize(version);
      if (!readSet(count, melds[version - 1]))
        return false;
    } else if (record == "P") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> version >> count) ||
          !readSet(count, versionedPointsTo_[{object, version}]))
        return false;
    } else if (record == "T") {
      NodeID node = 0;
      std::size_t count = 0;
      if (!(input >> node >> count) || !readSet(count, topLevelPointsTo_[node]))
        return false;
    } else if (record == "R") {
      ObjectID object = 0;
      Version source = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> source >> count))
        return false;
      auto &destinations = versionReliance_[object][source];
      for (std::size_t i = 0; i < count; ++i) {
        Version destination = InvalidVersion;
        if (!(input >> destination))
          return false;
        destinations.push_back(destination);
      }
    } else if (record == "D") {
      ObjectID object = 0;
      Version version = InvalidVersion;
      std::size_t count = 0;
      if (!(input >> object >> version >> count))
        return false;
      auto &statements = statementReliance_[object][version];
      for (std::size_t i = 0; i < count; ++i) {
        NodeID statement = 0;
        if (!(input >> statement))
          return false;
        statements.push_back(statement);
      }
    } else if (record == "F") {
      // Field-insensitive identity record (informational; solving IDs stay
      // unchanged and are only remapped by canonicalObject()).
      ObjectID representative = 0;
      ObjectID base = 0;
      if (!(input >> representative >> base))
        return false;
      loadedCanonicalObjects_[representative] = base;
    } else {
      return false;
    }
  }
  if (record != "E")
    return false;

  for (const auto &[nodeID, node] : *graph_)
    if (inScope(node))
      ++stats_.nodes;
  stats_.versionedObjects = versionMelds_.size();
  for (const auto &[object, canonical] : equivalentObject_)
    stats_.equivalentObjects += object != canonical;
  for (const auto &[object, melds] : versionMelds_)
    stats_.versions += melds.size();
  for (const auto &[key, pointsToSet] : versionedPointsTo_)
    stats_.versionedFacts += pointsToSet.size();
  for (const auto &[object, versions] : statementReliance_)
    for (const auto &[version, statements] : versions)
      stats_.statementReliances += statements.size();
  // Restore bookkeeping that the file format omits, so the loaded solver can
  // answer queries and accept new on-the-fly edges.
  for (const auto &[object, melds] : versionMelds_)
    labelledObjects_.insert(object);
  buildDeltaMaps();
  buildVersionProcessingOrder();
  // Reconnect the indirect-call edges the saved solution depends on.
  reconnectIndirectCallsAfterRead();
  return true;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::constantPointsTo(const Constant *constant) const {
  PointsToSet result;
  if (!constant || isa<ConstantPointerNull>(constant) ||
      !constant->getType()->isPointerTy())
    return result;
  const PointsToSet &known = graph_->getObjectIds(constant);
  result.insert(known.begin(), known.end());
  if (!result.empty())
    return mapObjects(result);
  if (const auto *expression = dyn_cast<ConstantExpr>(constant))
    if (expression->isCast() ||
        expression->getOpcode() == Instruction::GetElementPtr)
      return constantPointsTo(dyn_cast<Constant>(expression->getOperand(0)));
  if (ObjectID object = graph_->getObjectId(constant->stripPointerCasts()))
    result.insert(object);
  return mapObjects(result);
}

void VersionedFlowSensitivePTA::initializeGlobalMemory() {
  const Module *module = nullptr;
  for (const auto &[nodeID, node] : *graph_)
    if (const Function *function = node->getFunction()) {
      module = function->getParent();
      break;
    }
  if (!module)
    return;

  for (const GlobalVariable &global : module->globals()) {
    if (!global.hasInitializer())
      continue;
    PointsToSet storageBases;
    if (ObjectID object = graph_->getObjectId(&global))
      storageBases.insert(object);
    if (storageBases.empty()) {
      const PointsToSet &known = graph_->getObjectIds(&global);
      storageBases.insert(known.begin(), known.end());
    }
    auto storageAtOffset = [&](uint64_t offset) {
      PointsToSet storage;
      for (ObjectID base : storageBases) {
        const SVFG::ObjectInfo *info = graph_->getObjectInfo(base);
        const ObjectID canonical =
            info && info->baseObjId != 0 ? info->baseObjId : base;
        const ObjectID field = offset == 0
                                   ? canonical
                                   : graph_->getOffsetObject(canonical, offset);
        if (field != 0)
          storage.insert(field);
      }
      return mapObjects(storage);
    };

    const DataLayout &layout = module->getDataLayout();
    std::function<void(const Constant *, Type *, uint64_t)> seed =
        [&](const Constant *constant, Type *type, uint64_t offset) {
          if (!constant || !type)
            return;
          if (type->isPointerTy()) {
            const PointsToSet pointsTo = constantPointsTo(constant);
            for (ObjectID storage : storageAtOffset(offset)) {
              const Version version = initialVersion(storage);
              if (version != InvalidVersion)
                versionedPointsTo_[{storage, version}].insert(pointsTo.begin(),
                                                              pointsTo.end());
            }
            return;
          }
          if (auto *structure = dyn_cast<StructType>(type)) {
            const StructLayout *structureLayout =
                layout.getStructLayout(structure);
            for (unsigned field = 0; field < structure->getNumElements();
                 ++field)
              seed(constant->getAggregateElement(field),
                   structure->getElementType(field),
                   offset + structureLayout->getElementOffset(field));
            return;
          }
          if (auto *array = dyn_cast<ArrayType>(type)) {
            // Arrays collapse to their base object in gepTransfer(), so every
            // element initialiser targets the same offset. A per-element stride
            // would seed unreachable field objects and drop later initialisers.
            // Keep this in sync with
            // FlowSensitivePTA::initializeGlobalMemory().
            for (uint64_t element = 0; element < array->getNumElements();
                 ++element)
              seed(constant->getAggregateElement(element),
                   array->getElementType(), offset);
          }
        };
    seed(global.getInitializer(), global.getValueType(), 0);
  }
}

const Value *
VersionedFlowSensitivePTA::accessPointer(const Instruction *instruction) {
  if (const auto *load = dyn_cast_or_null<LoadInst>(instruction))
    return load->getPointerOperand();
  if (const auto *store = dyn_cast_or_null<StoreInst>(instruction))
    return store->getPointerOperand();
  if (const auto *rmw = dyn_cast_or_null<AtomicRMWInst>(instruction))
    return rmw->getPointerOperand();
  if (const auto *cmpxchg = dyn_cast_or_null<AtomicCmpXchgInst>(instruction))
    return cmpxchg->getPointerOperand();
  return nullptr;
}

const Value *
VersionedFlowSensitivePTA::storedValue(const Instruction *instruction) {
  if (const auto *store = dyn_cast_or_null<StoreInst>(instruction))
    return store->getValueOperand();
  if (const auto *rmw = dyn_cast_or_null<AtomicRMWInst>(instruction))
    return rmw->getValOperand();
  if (const auto *cmpxchg = dyn_cast_or_null<AtomicCmpXchgInst>(instruction))
    return cmpxchg->getNewValOperand();
  return nullptr;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::pointerTargets(const Value *pointer) const {
  if (!pointer || !pointer->getType()->isPointerTy())
    return {};
  const SVFGNode *node = graph_->getValueNode(pointer);
  return node ? pointsTo(node) : PointsToSet{};
}

VersionedFlowSensitivePTA::PointsToSet VersionedFlowSensitivePTA::accessTargets(
    const Value *pointer, const PointsToSet &memoryPointsTo) const {
  if (!pointer || !pointer->getType()->isPointerTy())
    return expandIndirectObjects(memoryPointsTo);
  if (graph_->getValueNode(pointer))
    return expandIndirectObjects(pointerTargets(pointer));
  // The value has no SVFG node (for example a constant-expression GEP or a
  // hand-built graph): fall back to the object set recorded for the value,
  // then to the MemorySSA access region.  An empty flow-sensitive set for an
  // existing node means the pointer is bottom here, so no fallback is used.
  PointsToSet known = graph_->getObjectIds(pointer);
  if (known.empty())
    known = memoryPointsTo;
  return expandIndirectObjects(known);
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::directInput(const SVFGNode &node) const {
  PointsToSet result;
  for (const SVFGEdge *edge : node.getInEdges()) {
    if (!edge || !inScope(edge->getSrcNode()) ||
        !isDirectVFGEdge(edge->getEdgeKind()))
      continue;
    const PointsToSet &source = pointsTo(edge->getSrcNode());
    result.insert(source.begin(), source.end());
  }
  return result;
}

bool VersionedFlowSensitivePTA::isStrongUpdate(
    const PointsToSet &targets) const {
  if (targets.size() != 1)
    return false;
  const ObjectID object = *targets.begin();
  // A Lotus merged object class cannot support strong updates: assigning
  // to one member must not kill the other members' contents.
  if (objectClustering_.clusterSize(object) > 1)
    return false;
  const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
  if (!info || info->isUnknown || info->isHeap || info->isArray ||
      info->isFieldInsensitive)
    return false;
  if (info->baseObjId != 0 && graph_->isFieldInsensitiveObject(info->baseObjId))
    return false;
  const Value *allocation = graph_->getObjectValue(object);
  if (!allocation && info->baseObjId != 0)
    allocation = graph_->getObjectValue(info->baseObjId);
  if (const auto *instruction = dyn_cast_or_null<Instruction>(allocation))
    if (recursiveFunctions_.count(instruction->getFunction()) != 0)
      return false;
  return true;
}

void VersionedFlowSensitivePTA::updateConnectedNodes(
    const std::vector<const SVFGEdge *> &newEdges) {
  // SVF's VersionedFlowSensitive::updateConnectedNodes().  New edges never
  // invalidate the version labels: delta nodes are prelabelled with a
  // dedicated version, so an edge is wired into the reliance map and the
  // points-to facts are propagated on the spot.
  for (const SVFGEdge *edge : newEdges) {
    if (!edge)
      continue;
    const SVFGNode *source = edge->getSrcNode();
    const SVFGNode *destination = edge->getDstNode();
    if (!inScope(source) || !inScope(destination))
      continue;
    if (!isIndirectVFGEdge(edge->getEdgeKind())) {
      // Direct inter-procedural edge: the destination gained a predecessor and
      // must be re-evaluated.
      enqueueStatement(destination->getId());
      // External summaries can introduce a fresh address node for an
      // allocation or an unknown return after the initial worklist seed.
      if (isa<AddrSVFGNode>(source))
        enqueueStatement(source->getId());
      continue;
    }
    PointsToSet objects = expandIndirectObjects(edge->getPointsTo());
    const bool wildcard =
        std::any_of(objects.begin(), objects.end(), [&](ObjectID object) {
          return graph_->isUnknownObject(object);
        });
    if (wildcard)
      objects = relevantObjects();
    for (ObjectID object : objects) {
      Version sourceVersion = getYield(source->getId(), object);
      Version destinationVersion = getConsume(destination->getId(), object);
      if (sourceVersion == InvalidVersion ||
          destinationVersion == InvalidVersion) {
        // No labels for this object at one end of the edge.  On a structural
        // delta edge (to a delta node, or out of a delta source) this simply
        // means the object carries no facts through the edge, so SVF skips
        // it.  Any other new edge means the graph was built without a
        // pre-analysis label for the object: label just that object instead
        // of rebuilding the whole graph.
        const bool structuralDeltaEdge =
            isDeltaNode(destination->getId()) || isDeltaSource(source->getId());
        if (structuralDeltaEdge || labelledObjects_.count(object) != 0)
          continue;
        if (!labelObjectIncremental(object))
          continue;
        sourceVersion = getYield(source->getId(), object);
        destinationVersion = getConsume(destination->getId(), object);
        if (sourceVersion == InvalidVersion ||
            destinationVersion == InvalidVersion)
          continue;
      }
      if (!addReliance(object, sourceVersion, destinationVersion))
        continue;
      ++stats_.deltaVersionUpdates;
      // Propagate the newly reachable facts right away.
      propagateVersion(object, sourceVersion, destinationVersion);
    }
  }
  buildVersionProcessingOrder();
}

void VersionedFlowSensitivePTA::reconnectIndirectCallsAfterRead() {
  if (!config_.connectIndirectCall)
    return;

  // The file stores solved facts but not the edges added by on-the-fly
  // call-graph construction. Recover them from the loaded solution: for each
  // indirect callsite, connect the callees its function pointer points to.
  std::vector<std::pair<const CallBase *, const Function *>> connections;
  std::unordered_set<const CallBase *> seenCallsites;
  for (const auto &[functionPointer, callSites] : graph_->getIndCallSiteMap()) {
    for (const CallBase *callSite : callSites) {
      if (!callSite || !seenCallsites.insert(callSite).second)
        continue;
      const Value *called = callSite->getCalledOperand();
      if (!called)
        continue;
      called = called->stripPointerCasts();
      const SVFGNode *node = graph_->getValueNode(called);
      if (!node)
        continue;
      for (ObjectID object : pointsTo(node)) {
        const auto *target =
            dyn_cast_or_null<Function>(graph_->getObjectValue(object));
        if (target)
          connections.emplace_back(callSite, target);
      }
    }
  }

  bool connectedAny = false;
  for (const auto &[callSite, target] : connections) {
    if (graph_->hasConnectedCallee(callSite, target))
      continue;
    std::unordered_map<const SVFGEdge *, SVFGNodeBS> oldEdges;
    collectCallsiteCalleeEdges(*graph_, callSite, target, oldEdges);
    if (!config_.connectIndirectCall(callSite, target))
      continue;
    connectedAny = true;
    std::unordered_map<const SVFGEdge *, SVFGNodeBS> currentEdges;
    collectCallsiteCalleeEdges(*graph_, callSite, target, currentEdges);
    std::vector<const SVFGEdge *> newEdges;
    for (const auto &[edge, guard] : currentEdges) {
      const auto previous = oldEdges.find(edge);
      if (previous == oldEdges.end() || previous->second != guard)
        newEdges.push_back(edge);
    }
    updateConnectedNodes(newEdges);
  }
  if (connectedAny)
    initializeRecursiveFunctions();
}

bool VersionedFlowSensitivePTA::resolveIndirectCalls(
    const SVFGNode &node, const PointsToSet &pointsToSet) {
  const NodeID functionPointer =
      node.hasValueId() ? node.getValueId() : node.getId();
  const auto &callSites = graph_->getIndCallSites(functionPointer);
  if (callSites.empty())
    return false;
  if (!config_.connectIndirectCall) {
    stats_.unresolvedIndirectCalls += callSites.size();
    return false;
  }
  const auto otfStarted = std::chrono::steady_clock::now();
  bool changed = false;
  for (const CallBase *callSite : callSites)
    for (ObjectID object : pointsToSet) {
      const auto *target =
          dyn_cast_or_null<Function>(graph_->getObjectValue(object));
      if (!target)
        continue;
      // SVF bounds every on-the-fly edge with the auxiliary Andersen call
      // graph; apply the same bound so a non-bounding connector cannot widen
      // the analysis.
      if (config_.isIndirectCallTargetAllowed &&
          !config_.isIndirectCallTargetAllowed(callSite, target)) {
        ++stats_.indirectCallsRejected;
        continue;
      }
      // Snapshot only the edges incident to this callsite/callee's actual and
      // formal nodes.
      std::unordered_map<const SVFGEdge *, SVFGNodeBS> oldEdges;
      collectCallsiteCalleeEdges(*graph_, callSite, target, oldEdges);
      const auto oldRecursiveFunctions = recursiveFunctions_;
      const bool wasConnected =
          graph_->getConnectedCallees(callSite).count(target) != 0;
      const bool callbackReportedChange =
          config_.connectIndirectCall(callSite, target);
      const bool newlyConnected =
          !wasConnected &&
          graph_->getConnectedCallees(callSite).count(target) != 0;
      if (callbackReportedChange || newlyConnected) {
        ++stats_.indirectCallEdges;
        if (!config_.isIndirectCallTargetAllowed)
          ++stats_.unboundedIndirectCalls;
        changed = true;
        std::unordered_map<const SVFGEdge *, SVFGNodeBS> currentEdges;
        collectCallsiteCalleeEdges(*graph_, callSite, target, currentEdges);
        std::vector<const SVFGEdge *> newEdges;
        newEdges.reserve(currentEdges.size());
        for (const auto &[edge, guard] : currentEdges) {
          const auto previous = oldEdges.find(edge);
          if (previous == oldEdges.end() || previous->second != guard)
            newEdges.push_back(edge);
        }
        updateConnectedNodes(newEdges);
        initializeRecursiveFunctions();
        if (recursionChangeAffectsStrongUpdates(oldRecursiveFunctions)) {
          if (config_.incrementalRecursionUpdate)
            demoteStrongUpdatesToWeak(oldRecursiveFunctions);
          else
            topologyChanged_ = true;
        }
      }
    }
  stats_.otfSeconds += std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - otfStarted)
                           .count();
  return changed;
}

VersionedFlowSensitivePTA::PointsToSet
VersionedFlowSensitivePTA::gepTransfer(const GepSVFGNode &gep) {
  const auto *instruction = dyn_cast_or_null<GetElementPtrInst>(gep.getValue());
  PointsToSet bases = directInput(gep);
  if (!instruction)
    return bases;
  PointsToSet result;
  for (ObjectID base : bases) {
    if (graph_->isUnknownObject(base)) {
      result.insert(base);
      continue;
    }
    // SVF: `if (isFieldInsensitive(o)) { set(o); continue; }`
    if (graph_->isFieldInsensitiveObject(base)) {
      const ObjectID fi = fieldInsensitiveRepresentative(base);
      result.insert(fi != 0 ? fi : base);
      continue;
    }
    const SVFG::GepAccessInfo access = graph_->getGepAccess(instruction);
    if (!access.valid) {
      // SVF collapses a variant field access: setObjFieldInsensitive(o) then
      // getFIObjVar(o).  The builder normally decides this during SVFG
      // construction; do it here as a backstop.
      const ObjectID fi = fieldInsensitiveRepresentative(base);
      result.insert(fi != 0 ? fi : base);
      continue;
    }
    // Annotated constant GEP: use the canonical (base, offset) object.
    {
      ObjectID canonical = base;
      uint64_t baseOffset = 0;
      if (const SVFG::ObjectInfo *info = graph_->getObjectInfo(base)) {
        if (info->baseObjId != 0)
          canonical = info->baseObjId;
        if (info->hasFieldOffset)
          baseOffset = info->fieldOffset;
      }
      if (access.relativeOffset <=
          std::numeric_limits<uint64_t>::max() - baseOffset) {
        const uint64_t totalOffset = baseOffset + access.relativeOffset;
        const ObjectID field =
            totalOffset == 0 ? canonical
                             : graph_->getOffsetObject(canonical, totalOffset);
        if (field != 0) {
          result.insert(field);
          continue;
        }
      }
    }
    if (ObjectID mapped = graph_->getGepObject(instruction, base))
      result.insert(mapped);
  }
  if (result.empty() && !bases.empty()) {
    const PointsToSet &known = graph_->getObjectIds(gep.getValue());
    result.insert(known.begin(), known.end());
  }
  return mapObjects(result);
}

void VersionedFlowSensitivePTA::enqueueStatement(NodeID node) {
  // NodeID 0 is a valid SVFG node id in Lotus (unlike SVF), so there is no
  // sentinel to reject here.
  if (!queuedStatements_.insert(node).second)
    return;
  auto component = sccComponentOf_.find(node);
  // Nodes outside the decomposition are processed after every in-scope SCC.
  const std::size_t priority = component == sccComponentOf_.end()
                                   ? sccComponentOf_.size()
                                   : component->second;
  statementWorklist_.emplace(priority, node);
  ++stats_.statementWorklistPushes;
}

void VersionedFlowSensitivePTA::enqueueDependentStatements(ObjectID object,
                                                           Version version) {
  auto dependentsIt = intrinsicDependents_.find(mapObject(object));
  if (dependentsIt != intrinsicDependents_.end())
    for (NodeID statement : dependentsIt->second)
      enqueueStatement(statement);
  if (version == InvalidVersion)
    return;
  auto objectIt = statementReliance_.find(mapObject(object));
  if (objectIt == statementReliance_.end())
    return;
  auto versionIt = objectIt->second.find(version);
  if (versionIt == objectIt->second.end())
    return;
  for (NodeID statement : versionIt->second)
    enqueueStatement(statement);
}

void VersionedFlowSensitivePTA::propagate(const SVFGNode &node) {
  for (const SVFGEdge *edge : node.getOutEdges()) {
    if (!edge || !inScope(edge->getDstNode()))
      continue;
    // Indirect edges are inert under versioning: their effect is expressed
    // through versionReliance and propagateVersion instead (SVF removes them
    // from the SVFG outright).
    if (!isDirectVFGEdge(edge->getEdgeKind()))
      continue;
    enqueueStatement(edge->getDstNode()->getId());
  }
}

void VersionedFlowSensitivePTA::propagateVersion(ObjectID object,
                                                 Version version) {
  // versionReliance_ is not mutated during propagation (only
  // updateConnectedNodes appends to it), so the reference stays valid.
  const std::vector<Version> &reliant = getReliantVersions(object, version);
  for (Version destination : reliant)
    propagateVersion(object, version, destination);
}

void VersionedFlowSensitivePTA::propagateVersion(ObjectID object,
                                                 Version source,
                                                 Version destination) {
  if (source == InvalidVersion || destination == InvalidVersion ||
      source == destination)
    return;
  object = mapObject(object);
  const PointsToSet copy(versionedPointsTo(object, source));
  if (copy.empty())
    return;
  PointsToSet &target = versionedPointsTo_[{object, destination}];
  const std::size_t oldSize = target.size();
  target.insert(copy.begin(), copy.end());
  if (oldSize == target.size())
    return;
  ++stats_.versionPropagations;
  // o:destination changed: notify the versions which rely on it and the
  // statements which consume it (SVF's DummyVersionPropSVFGNode worklist
  // entry plus stmtReliance pushes).
  const VersionedObject key{object, destination};
  if (queuedVersionPropagations_.insert(key).second)
    versionPropagationQueue_.push(key);
  enqueueDependentStatements(object, destination);
}

bool VersionedFlowSensitivePTA::drainVersionPropagation() {
  bool changed = false;
  while (!versionPropagationQueue_.empty() && !topologyChanged_ &&
         !stats_.timedOut) {
    if (timeLimitExceeded()) {
      noteTimeout();
      break;
    }
    const VersionedObject pending = versionPropagationQueue_.front();
    versionPropagationQueue_.pop();
    queuedVersionPropagations_.erase(pending);
    changed = true;
    propagateVersion(pending.object, pending.version);
  }
  return changed;
}

bool VersionedFlowSensitivePTA::processIntrinsicActualOut(
    const ActualOutSVFGNode &actualOut) {
  const auto *intrinsic =
      dyn_cast_or_null<IntrinsicInst>(actualOut.getCallSite());
  if (!intrinsic || intrinsic->arg_size() < 2)
    return false;
  const Intrinsic::ID intrinsicId = intrinsic->getIntrinsicID();
  if (intrinsicId != Intrinsic::memcpy && intrinsicId != Intrinsic::memmove &&
      intrinsicId != Intrinsic::memset)
    return false;

  struct Location {
    ObjectID object = 0;
    ObjectID base = 0;
    uint64_t offset = 0;
  };
  auto locations = [&](const Value *pointer) {
    std::vector<Location> result;
    for (ObjectID object : objectsWithFields(pointer)) {
      const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
      result.push_back({object,
                        info && info->baseObjId != 0 ? info->baseObjId : object,
                        info && info->hasFieldOffset ? info->fieldOffset : 0});
    }
    return result;
  };
  auto roots = [&](const Value *pointer) {
    PointsToSet objects = pointerTargets(pointer);
    if (objects.empty()) {
      const PointsToSet &known = graph_->getObjectIds(pointer);
      objects.insert(known.begin(), known.end());
    }
    objects = mapObjects(objects);
    std::vector<Location> result;
    for (ObjectID object : objects) {
      const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
      result.push_back({object,
                        info && info->baseObjId != 0 ? info->baseObjId : object,
                        info && info->hasFieldOffset ? info->fieldOffset : 0});
    }
    return result;
  };
  auto memoryBeforeCall = [&](ObjectID object) {
    PointsToSet result;
    bool foundVersion = false;
    for (const SVFGNode *actualIn :
         graph_->getActualIns(actualOut.getCallSite())) {
      Version version = getConsume(actualIn->getId(), object);
      if (version == InvalidVersion)
        version = getYield(actualIn->getId(), object);
      if (version == InvalidVersion)
        continue;
      foundVersion = true;
      const PointsToSet &points = versionedPointsTo(object, version);
      result.insert(points.begin(), points.end());
    }
    for (const SVFGEdge *edge : actualOut.getInEdges()) {
      const SVFGNode *source = edge ? edge->getSrcNode() : nullptr;
      if (!source)
        continue;
      Version version = getYield(source->getId(), object);
      if (version == InvalidVersion)
        continue;
      foundVersion = true;
      const PointsToSet &points = versionedPointsTo(object, version);
      result.insert(points.begin(), points.end());
    }
    if (!foundVersion) {
      Version version = getConsume(actualOut.getId(), object);
      if (version == InvalidVersion)
        version = initialVersion(object);
      const PointsToSet &points = versionedPointsTo(object, version);
      result.insert(points.begin(), points.end());
    }
    return result;
  };

  const Value *destinationPointer = intrinsic->getArgOperand(0);
  const std::vector<Location> destinationLocations =
      locations(destinationPointer);
  const std::vector<Location> destinationRoots = roots(destinationPointer);
  const auto *length = intrinsic->arg_size() >= 3
                           ? dyn_cast<ConstantInt>(intrinsic->getArgOperand(2))
                           : nullptr;
  const bool exactLength = length != nullptr;
  const uint64_t copiedBytes = length ? length->getZExtValue() : 0;
  auto pointerSizeForObject = [&](ObjectID object) {
    for (const auto &entry : graph_->getValueNodeMap()) {
      const auto *gep = dyn_cast_or_null<GetElementPtrInst>(entry.first);
      if (!gep || graph_->getObjectIds(gep).count(object) == 0)
        continue;
      if (const auto *pointerType =
              dyn_cast<PointerType>(gep->getResultElementType()))
        return static_cast<uint64_t>(
            intrinsic->getModule()->getDataLayout().getPointerSize(
                pointerType->getAddressSpace()));
    }
    return static_cast<uint64_t>(
        intrinsic->getModule()->getDataLayout().getPointerSize());
  };
  auto fullyCovered = [&](ObjectID object, uint64_t relativeOffset) {
    const uint64_t pointerSize = pointerSizeForObject(object);
    return !exactLength || (relativeOffset <= copiedBytes &&
                            pointerSize <= copiedBytes - relativeOffset);
  };

  std::vector<Location> sourceLocations;
  std::vector<Location> sourceRoots;
  if (intrinsicId == Intrinsic::memcpy || intrinsicId == Intrinsic::memmove) {
    sourceLocations = locations(intrinsic->getArgOperand(1));
    sourceRoots = roots(intrinsic->getArgOperand(1));
  }

  ObjectID unknownObject = 0;
  for (const auto &[object, label] : graph_->getObjectDebugMap())
    if (graph_->isUnknownObject(object)) {
      unknownObject = object;
      break;
    }

  bool changed = false;
  PointsToSet changedObjects;
  for (const Location &destination : destinationLocations) {
    uint64_t relativeOffset = std::numeric_limits<uint64_t>::max();
    for (const Location &root : destinationRoots)
      if (root.base == destination.base && destination.offset >= root.offset)
        relativeOffset =
            std::min(relativeOffset, destination.offset - root.offset);

    PointsToSet replacement;
    const bool outsideWrite =
        relativeOffset == std::numeric_limits<uint64_t>::max() ||
        (exactLength && relativeOffset >= copiedBytes);
    if (outsideWrite) {
      replacement = memoryBeforeCall(destination.object);
    } else if (intrinsicId == Intrinsic::memcpy ||
               intrinsicId == Intrinsic::memmove) {
      bool matchedSource = false;
      for (const Location &sourceRoot : sourceRoots) {
        const uint64_t sourceOffset = sourceRoot.offset + relativeOffset;
        if (sourceOffset < sourceRoot.offset)
          continue;
        for (const Location &source : sourceLocations)
          if (source.base == sourceRoot.base && source.offset == sourceOffset) {
            matchedSource = true;
            const PointsToSet sourceMemory = memoryBeforeCall(source.object);
            replacement.insert(sourceMemory.begin(), sourceMemory.end());
          }
      }
      if (!matchedSource && unknownObject != 0)
        replacement.insert(unknownObject);
    } else {
      const auto *fill = dyn_cast<ConstantInt>(intrinsic->getArgOperand(1));
      if ((!fill || !fill->isZero()) && unknownObject != 0)
        replacement.insert(unknownObject);
    }

    if (!outsideWrite && !fullyCovered(destination.object, relativeOffset)) {
      if (unknownObject != 0) {
        replacement.clear();
        replacement.insert(unknownObject);
      }
    } else if (!outsideWrite && !exactLength) {
      const PointsToSet oldMemory = memoryBeforeCall(destination.object);
      replacement.insert(oldMemory.begin(), oldMemory.end());
    }

    const Version yielded = getYield(actualOut.getId(), destination.object);
    if (yielded == InvalidVersion)
      continue;
    PointsToSet &output = versionedPointsTo_[{destination.object, yielded}];
    const std::size_t oldSize = output.size();
    output.insert(replacement.begin(), replacement.end());
    if (oldSize != output.size()) {
      changed = true;
      changedObjects.insert(destination.object);
    }
  }
  // Propagate the writes to the versions relying on what this actual-out
  // yields.
  for (ObjectID object : changedObjects) {
    const Version yielded = getYield(actualOut.getId(), object);
    if (yielded == InvalidVersion)
      continue;
    propagateVersion(object, yielded);
    enqueueDependentStatements(object, yielded);
  }
  return changed;
}

bool VersionedFlowSensitivePTA::processNode(const SVFGNode &node) {
  ++stats_.nodeProcesses;
  PointsToSet next = pointsTo(&node);
  if (const auto *address = dyn_cast<AddrSVFGNode>(&node)) {
    ObjectID object = address->getObjectId();
    if (object == 0 && address->getValue())
      object = graph_->getObjectId(address->getValue());
    next.clear();
    if (object != 0)
      next.insert(mapObject(object));
    if (isa_and_nonnull<CallBase>(address->getValue())) {
      const PointsToSet aliases = directInput(node);
      next.insert(aliases.begin(), aliases.end());
    }
  } else if (const auto *load = dyn_cast<LoadSVFGNode>(&node)) {
    next.clear();
    // SVF's pointer-only SVFG has no node for a non-pointer load.
    if (!config_.pointerOnlySVFG || !load->getInstruction() ||
        load->getInstruction()->getType()->isPointerTy()) {
      const PointsToSet targets = accessTargets(
          accessPointer(load->getInstruction()), load->getMemoryPointsTo());
      for (ObjectID object : targets) {
        if (graph_->isConstantObject(object))
          continue;
        const Version consumed = getConsume(node.getId(), object);
        // SVF's VFS skips objects without a consume label at this location.
        // Falling back to the object's initial version would inject memory
        // facts that do not actually reach the load.
        if (consumed == InvalidVersion)
          continue;
        const PointsToSet &memory = versionedPointsTo(object, consumed);
        next.insert(memory.begin(), memory.end());
      }
    }
  } else if (const auto *store = dyn_cast<StoreSVFGNode>(&node)) {
    const auto *instruction = store->getInstruction();
    const Value *valOp = storedValue(instruction);
    const Value *ptrOp = accessPointer(instruction);
    // SVF's processStore() only handles pointer-typed stored values; a scalar
    // store leaves every pointer version unchanged and, since it has no
    // pointer-only VFG node, does not even appear in its worklist.
    if (config_.pointerOnlySVFG && valOp && !valOp->getType()->isPointerTy())
      return false;
    const PointsToSet targets =
        accessTargets(ptrOp, store->getMemoryPointsTo());
    // Match SVF: do not process a store until the flow-sensitive points-to
    // set of its LHS is known.  An empty set means the store is dead at this
    // point; copying the consumed versions into the yielded versions here
    // would permanently pollute the (monotone) version facts with facts from
    // before the LHS was refined.
    if (targets.empty())
      return false;
    const bool strong = isStrongUpdate(targets);
    const ObjectID strongObject = strong ? *targets.begin() : 0;
    const PointsToSet stored =
        valOp ? pointerTargets(valOp) : directInput(node);
    bool updated = false;
    PointsToSet changedObjects;
    auto updateVersion = [&](ObjectID object, Version version,
                             const PointsToSet &values) {
      if (version == InvalidVersion)
        return;
      // Copy because `values` may alias an element of `versionedPointsTo_`,
      // which can rehash when the destination version is inserted.
      const PointsToSet copy(values);
      PointsToSet &destination = versionedPointsTo_[{object, version}];
      const std::size_t oldSize = destination.size();
      destination.insert(copy.begin(), copy.end());
      if (destination.size() != oldSize) {
        updated = true;
        changedObjects.insert(object);
      }
    };

    // Transfer the stored value into the versions this store yields.
    if (valOp && valOp->getType()->isPointerTy()) {
      for (ObjectID object : targets) {
        if (graph_->isConstantObject(object))
          continue;
        updateVersion(object, getYield(node.getId(), object), stored);
      }
    }

    // Copy every prelabeled object's consumed version into its yielded
    // version.  SVF applies this to the whole consume map, not just to the
    // current flow-sensitive target set; without it, an object that reaches
    // this store but is not a target of the LHS loses its memory facts.
    if (const auto locationIt = consume_.find(node.getId());
        locationIt != consume_.end()) {
      for (const auto &[object, consumed] : locationIt->second) {
        if (strong && object == strongObject)
          continue;
        updateVersion(object, getYield(node.getId(), object),
                      versionedPointsTo(object, consumed));
      }
    }

    if (strong) {
      weakUpdateSites_.erase(node.getId());
      strongUpdateSites_.insert(node.getId());
    } else {
      strongUpdateSites_.erase(node.getId());
      weakUpdateSites_.insert(node.getId());
    }
    if (updated) {
      // Changed objects must be propagated to the versions relying on the
      // store's yielded version, and the statements consuming them must be
      // re-evaluated (SVF's processStore propagation step).
      for (ObjectID object : changedObjects) {
        const Version yielded = getYield(node.getId(), object);
        if (yielded == InvalidVersion)
          continue;
        propagateVersion(object, yielded);
        enqueueDependentStatements(object, yielded);
      }
      return true;
    }
  } else if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(&node)) {
    if (processIntrinsicActualOut(*actualOut))
      return true;
  } else if (const auto *gep = dyn_cast<GepSVFGNode>(&node)) {
    next = gepTransfer(*gep);
  } else if (!isa<MSSASVFGNode>(&node)) {
    next = directInput(node);
  }

  PointsToSet &current = topLevelPointsTo_[node.getId()];
  const std::size_t oldSize = current.size();
  current.insert(next.begin(), next.end());
  if (current.size() == oldSize)
    return false;
  resolveIndirectCalls(node, current);
  return true;
}

bool VersionedFlowSensitivePTA::timeLimitExceeded() const {
  return hasDeadline_ && std::chrono::steady_clock::now() >= deadline_;
}

std::string VersionedFlowSensitivePTA::statisticsReport() const {
  std::ostringstream out;
  out << "VersionedFlowSensitivePTA statistics\n";
  out << "  nodes: " << stats_.nodes << "\n";
  out << "  sccs: " << stats_.sccs << " (max " << stats_.maxSccSize << ")\n";
  out << "  solve iterations: " << stats_.solveIterations << "\n";
  out << "  node processes: " << stats_.nodeProcesses << "\n";
  out << "  versioned objects: " << stats_.versionedObjects << " ("
      << stats_.equivalentObjects << " equivalent)\n";
  out << "  versions: " << stats_.versions << "\n";
  out << "  versioned facts: " << stats_.versionedFacts << "\n";
  out << "  version propagations: " << stats_.versionPropagations << "\n";
  out << "  statement reliances: " << stats_.statementReliances << "\n";
  out << "  strong updates: " << stats_.strongUpdates << "\n";
  out << "  weak updates: " << stats_.weakUpdates << "\n";
  out << "  indirect call edges: " << stats_.indirectCallEdges << "\n";
  out << "  indirect calls rejected by bound: " << stats_.indirectCallsRejected
      << "\n";
  if (stats_.unboundedIndirectCalls != 0)
    out << "  WARNING unbounded on-the-fly edges: "
        << stats_.unboundedIndirectCalls
        << " (no indirect-call target filter configured)\n";
  if (stats_.unresolvedIndirectCalls != 0)
    out << "  WARNING unresolved indirect callsites: "
        << stats_.unresolvedIndirectCalls << "\n";
  out << "  dynamic field-insensitive collapses: "
      << stats_.fieldInsensitiveCollapses << "\n";
  out << "  version pts non-empty/max/avg: " << stats_.nonEmptyVersions << "/"
      << stats_.maxVersionPtsSize << "/" << stats_.averageVersionPtsSize
      << "\n";
  out << "  pointers/objects: " << stats_.totalPointers << " / "
      << stats_.totalObjects << " (FI " << stats_.fieldInsensitiveObjects
      << ", FS " << stats_.fieldSensitiveObjects << ")\n";
  out << "  versions total/max-per-object/single-version-objects: "
      << stats_.totalVersions << "/" << stats_.maxVersionsForObject << "/"
      << stats_.singleVersionObjects << "\n";
  out << "  version facts existing/empty: " << stats_.totalExistingVPts << "/"
      << stats_.totalEmptyVPts << "\n";
  out << "  address-taken vars: " << stats_.addrTakenVars << " (max/avg pts "
      << stats_.maxAddrTakenPtsSize << "/" << stats_.averageAddrTakenPtsSize
      << ")\n";
  out << "  clustered objects: " << stats_.clusteredObjects << " ("
      << stats_.mergedObjectClusters << " merged classes)\n";
  out << "  delta nodes/sources: " << stats_.deltaNodes << " / "
      << stats_.deltaSourceNodes << "\n";
  out << "  delta version updates: " << stats_.deltaVersionUpdates << "\n";
  out << "  relabelings: " << stats_.relabelings << " (incremental "
      << stats_.deltaRelabelings << ")\n";
  out << "  svfgHasSU: " << (stats_.svfgHasSU ? "yes" : "no") << "\n";
  out << "  time: solve " << stats_.solveSeconds << "s, labeling "
      << stats_.labelingSeconds << "s, otf " << stats_.otfSeconds << "s\n";
  out << "  timed out: " << (stats_.timedOut ? "yes" : "no") << "\n";
  return out.str();
}

bool VersionedFlowSensitivePTA::solveCurrentTopology() {
  // SVF seeds the solver worklist with every node once (SCCDetect) and then
  // revisits only the successors of changed nodes plus the statements which
  // consume a changed version (stmtReliance / dummy propagation nodes).
  statementWorklist_ = {};
  queuedStatements_.clear();
  // Keep propagations seeded from global initializers before this call.
  // buildVersionLabels() clears these queues at the beginning of an epoch.
  // Same decomposition as the sparse solver / SVF's WPAFSSolver: components
  // are emitted in reverse topological order (Tarjan order, consumers before
  // producers) and the priority queue keeps each sweep in that order; a
  // changed statement requeues its consumers through propagate().
  const flow_sensitive::SVFGSCC scc = flow_sensitive::computeSVFGSCCs(
      *graph_, [this](const SVFGNode *node) { return inScope(node); });
  sccComponentOf_ = scc.componentOf;
  stats_.sccs = scc.components.size();
  stats_.maxSccSize = 0;
  for (const auto &component : scc.components)
    stats_.maxSccSize = std::max(stats_.maxSccSize, component.size());
  for (const auto &component : scc.components)
    for (const SVFGNode *node : component)
      enqueueStatement(node->getId());

  bool anyChanged = false;
  bool changed = true;
  while (changed && !topologyChanged_ && !stats_.timedOut) {
    ++stats_.solveIterations;
    changed = drainVersionPropagation();
    while (!statementWorklist_.empty() && !topologyChanged_ &&
           !stats_.timedOut) {
      if (timeLimitExceeded()) {
        noteTimeout();
        break;
      }
      const NodeID nodeID = statementWorklist_.top().second;
      statementWorklist_.pop();
      queuedStatements_.erase(nodeID);
      const SVFGNode *node = graph_->getNode(nodeID);
      if (!node || !inScope(node))
        continue;
      if (!processNode(*node))
        continue;
      changed = true;
      propagate(*node);
    }
    anyChanged |= changed;
  }
  return anyChanged;
}

const VersionedFlowSensitivePTA::Statistics &
VersionedFlowSensitivePTA::solve() {
  const auto started = std::chrono::steady_clock::now();
  stats_ = {};
  hasDeadline_ = config_.timeLimitSeconds != 0;
  if (hasDeadline_)
    deadline_ = started +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(config_.timeLimitSeconds));
  do {
    if (timeLimitExceeded()) {
      stats_.timedOut = true;
      break;
    }
    topologyChanged_ = false;
    initializeRecursiveFunctions();
    topLevelPointsTo_.clear();
    versionedPointsTo_.clear();
    strongUpdateSites_.clear();
    weakUpdateSites_.clear();
    {
      const auto labelingStarted = std::chrono::steady_clock::now();
      buildVersionLabels();
      stats_.labelingSeconds +=
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        labelingStarted)
              .count();
    }
    initializeGlobalMemory();
    // Global initializers live in the per-object initial version; push them
    // into the versions which rely on it before solving starts (the old
    // full-sweep propagator did this implicitly on every round).  Seed in the
    // clustered object order so heavily used objects converge first.
    for (ObjectID object : versionProcessingOrder_) {
      auto version = initialVersion_.find(object);
      if (version != initialVersion_.end())
        propagateVersion(object, version->second);
    }
    solveCurrentTopology();
  } while (topologyChanged_ && !stats_.timedOut);

  for (const auto &[nodeID, node] : *graph_)
    if (inScope(node))
      ++stats_.nodes;
  stats_.versionedFacts = 0;
  stats_.nonEmptyVersions = 0;
  stats_.maxVersionPtsSize = 0;
  stats_.averageVersionPtsSize = 0.0;
  for (const auto &[versionedObject, pointsToSet] : versionedPointsTo_) {
    stats_.versionedFacts += pointsToSet.size();
    stats_.maxVersionPtsSize =
        std::max<std::size_t>(stats_.maxVersionPtsSize, pointsToSet.size());
    if (!pointsToSet.empty())
      ++stats_.nonEmptyVersions;
  }
  if (!versionedPointsTo_.empty())
    stats_.averageVersionPtsSize =
        static_cast<double>(stats_.versionedFacts) /
        static_cast<double>(versionedPointsTo_.size());
  stats_.statementReliances = 0;
  for (const auto &[object, versions] : statementReliance_)
    for (const auto &[version, statements] : versions)
      stats_.statementReliances += statements.size();
  stats_.strongUpdates = strongUpdateSites_.size();
  stats_.weakUpdates = weakUpdateSites_.size();
  stats_.svfgHasSU = !strongUpdateSites_.empty();
  // SVF-named statistics (VersionedFlowSensitiveStat).
  stats_.totalPointers = graph_->countPointerValueNodes();
  const auto [fiObjects, fsObjects] = graph_->countBaseObjects();
  stats_.fieldInsensitiveObjects = fiObjects;
  stats_.fieldSensitiveObjects = fsObjects;
  stats_.totalObjects = fiObjects + fsObjects;
  stats_.totalExistingVPts = versionedPointsTo_.size();
  stats_.maxPtsSize = stats_.maxVersionPtsSize;
  stats_.averagePtsSize = stats_.averageVersionPtsSize;
  std::size_t versionTotal = 0;
  for (const auto &[object, melds] : versionMelds_) {
    versionTotal += melds.size();
    stats_.maxVersionsForObject =
        std::max(stats_.maxVersionsForObject, melds.size());
    if (melds.size() == 1)
      ++stats_.singleVersionObjects;
  }
  stats_.totalVersions = versionTotal;
  stats_.totalEmptyVPts =
      stats_.totalExistingVPts > stats_.nonEmptyVersions
          ? stats_.totalExistingVPts - stats_.nonEmptyVersions
          : 0;
  {
    std::size_t addrTakenTotal = 0;
    for (const auto &[nodeID, node] : *graph_) {
      if (!inScope(node))
        continue;
      const auto *store = dyn_cast<StoreSVFGNode>(node);
      if (!store)
        continue;
      const Value *pointer = accessPointer(store->getInstruction());
      if (!pointer || !pointer->getType()->isPointerTy())
        continue;
      // Pointer-only mode ignores scalar stores, so they do not contribute an
      // address-taken variable either.
      const Value *stored = storedValue(store->getInstruction());
      if (config_.pointerOnlySVFG && stored && !stored->getType()->isPointerTy())
        continue;
      const std::size_t size =
          accessTargets(pointer, store->getMemoryPointsTo()).size();
      if (size == 0)
        continue;
      ++stats_.addrTakenVars;
      addrTakenTotal += size;
      stats_.maxAddrTakenPtsSize = std::max(stats_.maxAddrTakenPtsSize, size);
    }
    if (stats_.addrTakenVars != 0)
      stats_.averageAddrTakenPtsSize =
          static_cast<double>(addrTakenTotal) /
          static_cast<double>(stats_.addrTakenVars);
  }
  stats_.solveSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  return stats_;
}

const VersionedFlowSensitivePTA::PointsToSet &
VersionedFlowSensitivePTA::pointsTo(const SVFGNode *node) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto it = topLevelPointsTo_.find(node->getId());
  return it == topLevelPointsTo_.end() ? empty : it->second;
}

std::optional<VersionedFlowSensitivePTA::PointsToSet>
VersionedFlowSensitivePTA::pointsTo(const Value *value) const {
  if (!value || !value->getType()->isPointerTy())
    return std::nullopt;
  const SVFGNode *node = graph_->getValueNode(value);
  if (!inScope(node))
    return std::nullopt;
  return pointsTo(node);
}

std::optional<bool>
VersionedFlowSensitivePTA::mayAlias(const Value *lhs, const Value *rhs) const {
  auto left = pointsTo(lhs);
  auto right = pointsTo(rhs);
  if (!left || !right)
    return std::nullopt;
  for (const PointsToSet *set : {&*left, &*right})
    for (ObjectID object : *set)
      if (graph_->isUnknownObject(object))
        return true;
  *left = expandIndirectObjects(*left);
  *right = expandIndirectObjects(*right);
  const PointsToSet *small = &*left;
  const PointsToSet *large = &*right;
  if (large->size() < small->size())
    std::swap(small, large);
  for (ObjectID object : *small)
    if (large->count(object) != 0)
      return true;
  return false;
}

} // namespace lotus::alias
