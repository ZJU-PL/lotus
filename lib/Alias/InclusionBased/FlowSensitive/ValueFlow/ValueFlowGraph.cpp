#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowGraph.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <stdexcept>
#include <utility>

namespace lotus::alias::vfg {
namespace {
template <typename T> void appendUnique(std::vector<T> &values, T value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}
// Kosaraju components in source-to-sink order, without recursive DFS.
template <typename ID>
std::pair<std::vector<std::size_t>, std::size_t>
controlComponents(const std::vector<std::vector<ID>> &successors,
                  const std::vector<std::vector<ID>> &predecessors) {
  const std::size_t count = successors.size();
  std::vector<bool> seen(count, false);
  std::vector<std::size_t> postorder;
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
} // namespace

Program::Program() {
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
  objects_.push_back({source, initial, singleton, memory, std::move(name)});
  initializers_.push_back(0);
  return id;
}
BlockID Program::addBlock() {
  const BlockID id = nextID(blocks_.size());
  blocks_.emplace_back();
  return id;
}
void Program::addRoot(BlockID block) {
  checkBlock(block);
  roots_.insert(block);
}
void Program::addControlEdge(BlockID from, BlockID to) {
  checkBlock(from);
  checkBlock(to);
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
  return id;
}
void Program::setInitializer(ObjectID object, NodeID value) {
  checkNode(value);
  if (!object || object >= objects_.size() || !objects_[object].memory)
    throw std::invalid_argument(
        "ValueFlowPTA: initializer needs a memory object");
  if (initializers_[object])
    throw std::invalid_argument("ValueFlowPTA: initializer already set");
  initializers_[object] = value;
}
void Program::setSingleton(ObjectID object, bool singleton) {
  if (!object || object >= objects_.size() || object <= unknownObject())
    throw std::invalid_argument("ValueFlowPTA: invalid singleton object");
  objects_[object].singleton = singleton;
}

class Solver::Impl {
public:
  Impl(const Program &program, Config config) : p(program), config(config) {}
  const Program &p;
  Config config;
  Statistics stats;
  bool weak = false;
  bool analyzed = false;
  std::vector<NodeSet> edges;
  std::vector<PointsToSet> pts;
  std::vector<NodeSet> pted;
  std::vector<NodeSet> reachSeeds;
  NodeSet dirtySignatures;
  std::vector<NodeSet> activeAccesses;
  std::vector<NodeSet> escapePointers;
  std::vector<NodeSet> initialEscapeObjects;
  std::vector<ObjectID> blockedWitness;

  std::vector<bool> reachable;
  std::vector<std::vector<AccessID>> uses;
  std::vector<NodeID> storedThrough;
  std::vector<ObjectID> initializedObject;
  std::set<ObjectID> dirtyReach;
  std::set<ObjectID> pending;
  std::vector<bool> processed;
  std::set<AccessID> finalStrong;

  using CFGNode = std::size_t;
  static constexpr CFGNode InvalidCFGNode = std::numeric_limits<CFGNode>::max();
  std::vector<std::vector<CFGNode>> cfgSuccessors;
  std::vector<std::vector<CFGNode>> cfgPredecessors;
  std::vector<std::vector<CFGNode>> dominatorChildren;
  std::vector<std::vector<CFGNode>> dominanceFrontiers;
  std::vector<CFGNode> immediateDominator;
  std::vector<CFGNode> accessLocation;
  std::vector<AccessID> locationAccess;
  std::vector<std::size_t> weakComponent;
  std::vector<NodeSet> weakPredecessors;

  struct Signature {
    NodeSet accesses;
    NodeSet strong;
    bool operator==(const Signature &other) const {
      return accesses == other.accesses && strong == other.strong;
    }
  };
  std::vector<Signature> previous;
  std::vector<Signature> current;
  // Equal access/kill signatures have equal sparse transfer relations. Share
  // their topology and feed each object's own initializer into its root.
  struct SignatureHash {
    std::size_t operator()(const std::pair<NodeSet, NodeSet> &key) const {
      std::uint64_t hash = 14695981039346656037ULL;
      for (NodeID id : key.first)
        hash = (hash ^ id) * 1099511628211ULL;
      hash = (hash ^ key.first.size()) * 1099511628211ULL;
      for (NodeID id : key.second)
        hash = (hash ^ id) * 1099511628211ULL;
      return static_cast<std::size_t>(hash);
    }
  };
  std::unordered_map<std::pair<NodeSet, NodeSet>, NodeID, SignatureHash>
      flowTemplates;

  void buildDominanceInfo() {
    cfgSuccessors.assign(1, {});
    cfgPredecessors.assign(1, {});
    locationAccess.assign(1, 0);
    accessLocation.assign(p.accesses_.size(), InvalidCFGNode);
    std::vector<CFGNode> blockEntry(p.blocks_.size(), InvalidCFGNode);
    std::vector<CFGNode> blockExit(p.blocks_.size(), InvalidCFGNode);

    auto addNode = [&](AccessID access = 0) {
      const CFGNode node = cfgSuccessors.size();
      cfgSuccessors.emplace_back();
      cfgPredecessors.emplace_back();
      locationAccess.push_back(access);
      return node;
    };
    auto addEdge = [&](CFGNode from, CFGNode to) {
      appendUnique(cfgSuccessors[from], to);
      appendUnique(cfgPredecessors[to], from);
    };

    for (BlockID block = 1; block < p.blocks_.size(); ++block) {
      if (!reachable[block])
        continue;
      CFGNode current = addNode();
      blockEntry[block] = current;
      for (AccessID access : p.blocks_[block].accesses) {
        const CFGNode location = addNode(access);
        accessLocation[access] = location;
        addEdge(current, location);
        current = location;
      }
      blockExit[block] = current;
    }
    for (BlockID block = 1; block < p.blocks_.size(); ++block) {
      if (!reachable[block])
        continue;
      for (BlockID successor : p.blocks_[block].successors)
        if (reachable[successor])
          addEdge(blockExit[block], blockEntry[successor]);
    }
    for (BlockID root : p.roots_)
      if (reachable[root])
        addEdge(0, blockEntry[root]);

    std::vector<bool> visited(cfgSuccessors.size(), false);
    std::vector<std::pair<CFGNode, std::size_t>> dfs{{0, 0}};
    std::vector<CFGNode> postorder;
    visited[0] = true;
    while (!dfs.empty()) {
      auto &[node, successorIndex] = dfs.back();
      if (successorIndex < cfgSuccessors[node].size()) {
        const CFGNode successor = cfgSuccessors[node][successorIndex++];
        if (!visited[successor]) {
          visited[successor] = true;
          dfs.emplace_back(successor, 0);
        }
        continue;
      }
      postorder.push_back(node);
      dfs.pop_back();
    }
    std::vector<CFGNode> reversePostorder(postorder.rbegin(), postorder.rend());
    std::vector<CFGNode> rpoNumber(cfgSuccessors.size(), InvalidCFGNode);
    for (CFGNode number = 0; number < reversePostorder.size(); ++number)
      rpoNumber[reversePostorder[number]] = number;

    immediateDominator.assign(cfgSuccessors.size(), InvalidCFGNode);
    immediateDominator[0] = 0;
    auto intersect = [&](CFGNode left, CFGNode right) {
      while (left != right) {
        while (rpoNumber[left] > rpoNumber[right])
          left = immediateDominator[left];
        while (rpoNumber[right] > rpoNumber[left])
          right = immediateDominator[right];
      }
      return left;
    };
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t index = 1; index < reversePostorder.size(); ++index) {
        const CFGNode node = reversePostorder[index];
        CFGNode newDominator = InvalidCFGNode;
        for (CFGNode predecessor : cfgPredecessors[node]) {
          if (immediateDominator[predecessor] == InvalidCFGNode)
            continue;
          newDominator = newDominator == InvalidCFGNode
                             ? predecessor
                             : intersect(predecessor, newDominator);
        }
        if (newDominator == InvalidCFGNode)
          throw std::logic_error(
              "ValueFlowPTA: reachable control node has no dominator");
        if (immediateDominator[node] != newDominator) {
          immediateDominator[node] = newDominator;
          changed = true;
        }
      }
    }

    dominatorChildren.assign(cfgSuccessors.size(), {});
    for (CFGNode node = 1; node < cfgSuccessors.size(); ++node)
      dominatorChildren[immediateDominator[node]].push_back(node);
    dominanceFrontiers.assign(cfgSuccessors.size(), {});
    for (CFGNode node = 1; node < cfgSuccessors.size(); ++node) {
      if (cfgPredecessors[node].size() < 2)
        continue;
      for (CFGNode predecessor : cfgPredecessors[node]) {
        CFGNode runner = predecessor;
        while (runner != immediateDominator[node]) {
          appendUnique(dominanceFrontiers[runner], node);
          runner = immediateDominator[runner];
        }
      }
    }
    stats.expandedControlFlowNodes = cfgSuccessors.size();
  }

  void reset(bool weakMode) {
    weak = weakMode;
    flowTemplates.clear();
    weakComponent.clear();
    weakPredecessors.clear();
    edges.assign(p.edges_.size(), {});
    for (NodeID node = 1; node < p.edges_.size(); ++node)
      edges[node].insert(p.edges_[node].begin(), p.edges_[node].end());
    pts.assign(p.edges_.size(), {});
    pted.assign(p.objects_.size(), {});
    reachSeeds.assign(p.objects_.size(), {});
    dirtySignatures.clear();
    activeAccesses.assign(p.objects_.size(), {});
    escapePointers.assign(p.objects_.size(), {});
    initialEscapeObjects.assign(p.objects_.size(), {});
    blockedWitness.assign(p.objects_.size(), 0);
    current.assign(p.objects_.size(), {});
    uses.assign(p.edges_.size(), {});
    storedThrough.assign(p.edges_.size(), 0);
    initializedObject.assign(p.edges_.size(), 0);
    previous.assign(p.objects_.size(), {});
    processed.assign(p.objects_.size(), false);
    reachable.assign(p.blocks_.size(), false);
    dirtyReach.clear();
    pending.clear();
    finalStrong.clear();
    stats.indirectEdges = 0;
    stats.strongUpdateSites = 0;

    std::vector<BlockID> work(p.roots_.begin(), p.roots_.end());
    while (!work.empty()) {
      const BlockID block = work.back();
      work.pop_back();
      if (reachable[block])
        continue;
      reachable[block] = true;
      work.insert(work.end(), p.blocks_[block].successors.begin(),
                  p.blocks_[block].successors.end());
    }
    for (AccessID id = 1; id < p.accesses_.size(); ++id) {
      const auto &access = p.accesses_[id];
      if (!reachable[access.block])
        continue;
      uses[access.pointer].push_back(id);
      if (access.kind == Program::AccessKind::Store)
        storedThrough[access.value] = access.pointer;
    }
    buildDominanceInfo();
    for (ObjectID object = 1; object < p.objects_.size(); ++object) {
      dirtyReach.insert(object);
      reachSeeds[object].insert(p.objects_[object].address);
      dirtySignatures.insert(object);
      if (!p.objects_[object].memory)
        continue;
      pending.insert(object);
      // A virtual initial store participates both in reachability and in
      // escape ordering, including global pointer initializers.
      const NodeID initializer =
          p.initializers_[object] ? p.initializers_[object] : p.unknownValue();
      edges[initializer].insert(p.objects_[object].initialStore);
      initializedObject[p.objects_[object].initialStore] = object;
    }
  }

  // Algorithm 1: traverse once per affected object, rather than propagating
  // whole points-to sets through the control-flow graph.
  void computePointedToBy() {
    while (!dirtyReach.empty()) {
      const ObjectID object = *dirtyReach.begin();
      dirtyReach.erase(dirtyReach.begin());
      ++stats.objectTraversals;
      NodeSet seeds;
      seeds.swap(reachSeeds[object]);
      seeds.subtract(pted[object]);
      pted[object].unite(seeds);
      std::vector<NodeID> work(seeds.begin(), seeds.end());
      while (!work.empty()) {
        const NodeID node = work.back();
        work.pop_back();
        pts[node].insert(object);
        if (initializedObject[node])
          initialEscapeObjects[object].insert(initializedObject[node]);
        else if (storedThrough[node])
          escapePointers[object].insert(storedThrough[node]);
        if (!uses[node].empty()) {
          activeAccesses[object].insert(uses[node].begin(), uses[node].end());
          dirtySignatures.insert(object);
          // Existing non-singleton targets cannot lose a strong update.
          for (ObjectID target : pts[node])
            if (p.objects_[target].singleton)
              dirtySignatures.insert(target);
          if (object == p.unknownObject())
            for (ObjectID target = 1; target < p.objects_.size(); ++target)
              if (p.objects_[target].memory)
                dirtySignatures.insert(target);
        }
        // Mark at discovery and subtract whole words. In a dense VFG,
        // enqueueing every successor and checking on pop revisits the same
        // facts once per incoming edge, even after reachability saturates.
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
    if (!weak && p.objects_[object].singleton) {
      for (AccessID id : result.accesses) {
        const auto &access = p.accesses_[id];
        if (access.allowStrong && access.kind == Program::AccessKind::Store &&
            pts[access.pointer].size() == 1 && pts[access.pointer].count(object))
          result.strong.insert(id);
      }
    }
    return result;
  }

  bool escapeBlocked(ObjectID object) {
    // A positive dependency stays valid as points-to sets grow. Recheck only
    // after its witness has left the pending set; newly requeued witnesses
    // are dependencies again. Never cache an unresolved pointer as a fact.
    if (blockedWitness[object] && pending.count(blockedWitness[object]))
      return true;
    auto blockedBy = [&](ObjectID dependency) {
      blockedWitness[object] = dependency;
      return true;
    };
    for (ObjectID dependency : initialEscapeObjects[object])
      if (pending.count(dependency))
        return blockedBy(dependency);
    for (NodeID pointer : escapePointers[object]) {
      const auto &targets = pts[pointer];
      if (targets.empty())
        return true;
      if (targets.count(p.unknownObject()))
        return blockedBy(object); // Wildcard includes the object itself.
      for (ObjectID target : targets)
        if (p.objects_[target].memory && pending.count(target))
          return blockedBy(target);
    }
    return false;
  }

  bool addIndirect(NodeID from, NodeID to) {
    auto &successors = edges[from];
    if (!successors.insert(to).second)
      return false;
    ++stats.indirectEdges;
    // Algorithm 3, UpdateVFG: only objects reaching the source need traversal.
    for (ObjectID object : pts[from]) {
      if (!pted[object].count(to)) {
        reachSeeds[object].insert(to);
        dirtyReach.insert(object);
      }
    }
    return true;
  }

  NodeID addJoin() {
    const NodeID node = nextID(edges.size());
    edges.emplace_back();
    pts.emplace_back();
    uses.emplace_back();
    storedThrough.push_back(0);
    initializedObject.push_back(0);
    ++stats.valueFlowJoinNodes;
    return node;
  }

  // Without kills, control reachability is the complete memory transfer
  // relation. Its SCCs are independent of the object and need computing only
  // once. Project each weak signature onto that DAG, retaining only ancestors
  // of loads and bypassing nodes with a single input.
  NodeID computeWeakFlow(const Signature &sig) {
    if (weakComponent.empty()) {
      auto partition = controlComponents(cfgSuccessors, cfgPredecessors);
      weakComponent = std::move(partition.first);
      weakPredecessors.resize(partition.second);
      for (CFGNode node = 0; node < cfgSuccessors.size(); ++node)
        for (CFGNode next : cfgSuccessors[node])
          if (weakComponent[node] != weakComponent[next])
            weakPredecessors[weakComponent[next]].insert(
                nextID(weakComponent[node]));
    }
    const std::size_t count = weakPredecessors.size();
    std::vector<NodeSet> generated(count);
    std::vector<std::vector<NodeID>> loads(count);
    std::vector<bool> needed(count, false);
    std::vector<std::size_t> work;
    for (AccessID id : sig.accesses) {
      const auto component = weakComponent[accessLocation[id]];
      const auto &access = p.accesses_[id];
      if (access.kind == Program::AccessKind::Store) {
        generated[component].insert(access.value);
      } else {
        loads[component].push_back(access.value);
        if (!needed[component]) {
          needed[component] = true;
          work.push_back(component);
        }
      }
    }
    while (!work.empty()) {
      const auto component = work.back();
      work.pop_back();
      for (auto pred : weakPredecessors[component])
        if (!needed[pred]) {
          needed[pred] = true;
          work.push_back(pred);
        }
    }
    std::vector<NodeID> flow(count, 0);
    const auto root = weakComponent[0];
    for (std::size_t component = 0; component < count; ++component) {
      if (!needed[component])
        continue;
      NodeSet inputs = std::move(generated[component]);
      for (auto pred : weakPredecessors[component])
        if (flow[pred])
          inputs.insert(flow[pred]);
      if (inputs.size() == 1 && component != root) {
        flow[component] = *inputs.begin();
      } else {
        flow[component] = addJoin();
        for (NodeID input : inputs)
          addIndirect(input, flow[component]);
      }
      for (NodeID load : loads[component])
        addIndirect(flow[component], load);
    }
    return flow[root];
  }

  // Algorithm 2's store-plus-IDF construction, applied to the expanded
  // interprocedural control graph. Expanding blocks at memory accesses makes
  // dominance sensitive to instruction order within an LLVM basic block.
  void computeIndirect(ObjectID object, const Signature &sig) {
    ++stats.indirectFlowComputations;
    stats.projectedEvents += sig.accesses.size();
    if (sig.accesses.empty())
      return;
    const auto key = std::make_pair(sig.accesses, sig.strong);
    auto cached = flowTemplates.find(key);
    if (cached != flowTemplates.end()) {
      ++stats.indirectFlowCacheHits;
      addIndirect(p.objects_[object].initialStore, cached->second);
      return;
    }

    const bool hasLoad = std::any_of(sig.accesses.begin(), sig.accesses.end(),
                                     [&](AccessID id) {
      return p.accesses_[id].kind == Program::AccessKind::Load;
    });
    if (!hasLoad)
      return;
    if (sig.strong.empty()) {
      const NodeID root = computeWeakFlow(sig);
      addIndirect(p.objects_[object].initialStore, root);
      flowTemplates.emplace(key, root);
      return;
    }

    std::vector<bool> storeLocation(cfgSuccessors.size(), false);
    std::deque<CFGNode> idfWork;
    for (AccessID access : sig.accesses) {
      if (p.accesses_[access].kind != Program::AccessKind::Store)
        continue;
      const CFGNode location = accessLocation[access];
      storeLocation[location] = true;
      idfWork.push_back(location);
    }

    // Compute the iterated dominance frontier of the relevant stores. A node
    // that is both a store and an IDF join has a phi-like merge immediately
    // before the store's GEN/KILL transfer.
    std::vector<bool> idfLocation(cfgSuccessors.size(), false);
    while (!idfWork.empty()) {
      const CFGNode node = idfWork.front();
      idfWork.pop_front();
      for (CFGNode frontier : dominanceFrontiers[node]) {
        if (idfLocation[frontier])
          continue;
        idfLocation[frontier] = true;
        if (!storeLocation[frontier])
          idfWork.push_back(frontier);
      }
    }

    using SparseNode = NodeID;
    const SparseNode invalidSparse = std::numeric_limits<SparseNode>::max();
    std::vector<std::vector<SparseNode>> sparseSuccessors(1);
    std::vector<NodeSet> sparseEdgeIndex(1);
    std::vector<std::vector<SparseNode>> sparsePredecessors(1);
    std::vector<AccessID> sparseStore(1, 0);
    std::vector<SparseNode> phiNode(cfgSuccessors.size(), invalidSparse);
    std::vector<SparseNode> storeNode(p.accesses_.size(), invalidSparse);
    auto addSparseNode = [&](AccessID store = 0) {
      const SparseNode node = nextID(sparseSuccessors.size());
      sparseSuccessors.emplace_back();
      sparseEdgeIndex.emplace_back();
      sparsePredecessors.emplace_back();
      sparseStore.push_back(store);
      return node;
    };
    for (CFGNode location = 0; location < idfLocation.size(); ++location)
      if (idfLocation[location]) {
        phiNode[location] = addSparseNode();
        ++stats.iteratedDominanceFrontierNodes;
      }
    for (AccessID access : sig.accesses)
      if (p.accesses_[access].kind == Program::AccessKind::Store)
        storeNode[access] = addSparseNode(access);

    auto addSparseEdge = [&](SparseNode from, SparseNode to) {
      if (!sparseEdgeIndex[from].insert(to).second)
        return;
      sparseSuccessors[from].push_back(to);
      sparsePredecessors[to].push_back(from);
      ++stats.sparseGraphEdges;
    };

    // Rename definitions down the dominator tree. IDF nodes receive one input
    // from every incoming control-flow edge; stores consume the current sparse
    // definition and then become the definition seen by following accesses.
    std::vector<SparseNode> definitions{0};
    std::vector<SparseNode> loadDefinition(p.accesses_.size(), invalidSparse);
    struct RenameFrame {
      CFGNode node;
      std::size_t nextChild = 0;
      std::size_t pushes = 0;
      bool entered = false;
    };
    std::vector<RenameFrame> rename{{0}};
    while (!rename.empty()) {
      RenameFrame &frame = rename.back();
      if (!frame.entered) {
        frame.entered = true;
        if (phiNode[frame.node] != invalidSparse) {
          definitions.push_back(phiNode[frame.node]);
          ++frame.pushes;
        }
        const AccessID access = locationAccess[frame.node];
        if (access && sig.accesses.count(access)) {
          if (p.accesses_[access].kind == Program::AccessKind::Store) {
            const SparseNode store = storeNode[access];
            addSparseEdge(definitions.back(), store);
            definitions.push_back(store);
            ++frame.pushes;
          } else {
            loadDefinition[access] = definitions.back();
          }
        }
        for (CFGNode successor : cfgSuccessors[frame.node])
          if (phiNode[successor] != invalidSparse)
            addSparseEdge(definitions.back(), phiNode[successor]);
      }
      if (frame.nextChild < dominatorChildren[frame.node].size()) {
        const CFGNode child = dominatorChildren[frame.node][frame.nextChild++];
        rename.push_back({child});
        continue;
      }
      definitions.resize(definitions.size() - frame.pushes);
      rename.pop_back();
    }

    stats.sparseGraphNodes += sparseSuccessors.size();

    // Factor the GEN/KILL relation into copy edges instead of materializing
    // every reaching-store/load pair. A weak store joins its generated value
    // with its predecessors; a strong store has no predecessor input. Thus
    // reachability through these internal nodes is exactly the GEN/KILL
    // fixed point, including loops, without repeatedly unioning definition
    // sets at every sparse node.
    // Condense copy cycles after removing the inputs killed by strong
    // stores. Every node in such an SCC has the same reaching values.
    const std::size_t count = sparseSuccessors.size();
    auto killsInput = [&](SparseNode node) {
      return sparseStore[node] && sig.strong.count(sparseStore[node]);
    };
    std::vector<bool> seen(count, false);
    std::vector<SparseNode> postorder;
    for (SparseNode start = 0; start < count; ++start) {
      if (seen[start])
        continue;
      std::vector<std::pair<SparseNode, std::size_t>> dfs{{start, 0}};
      seen[start] = true;
      while (!dfs.empty()) {
        auto &frame = dfs.back();
        const auto &next = sparseSuccessors[frame.first];
        if (frame.second < next.size()) {
          const SparseNode successor = next[frame.second++];
          if (!killsInput(successor) && !seen[successor]) {
            seen[successor] = true;
            dfs.emplace_back(successor, 0);
          }
        } else {
          postorder.push_back(frame.first);
          dfs.pop_back();
        }
      }
    }
    std::vector<SparseNode> component(count, invalidSparse);
    SparseNode components = 0;
    for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
      if (component[*it] != invalidSparse)
        continue;
      std::vector<SparseNode> work{*it};
      component[*it] = components;
      while (!work.empty()) {
        const SparseNode node = work.back();
        work.pop_back();
        if (killsInput(node))
          continue;
        for (SparseNode predecessor : sparsePredecessors[node]) {
          if (component[predecessor] == invalidSparse) {
            component[predecessor] = components;
            work.push_back(predecessor);
          }
        }
      }
      ++components;
    }
    std::vector<NodeID> componentNodes;
    componentNodes.reserve(components);
    for (SparseNode i = 0; i < components; ++i) {
      const NodeID node = nextID(edges.size());
      edges.emplace_back();
      pts.emplace_back();
      uses.emplace_back();
      storedThrough.push_back(0);
      initializedObject.push_back(0);
      componentNodes.push_back(node);
      ++stats.valueFlowJoinNodes;
    }
    std::vector<NodeID> flowNodes(count);
    for (SparseNode node = 0; node < count; ++node)
      flowNodes[node] = componentNodes[component[node]];
    for (SparseNode node = 1; node < count; ++node) {
      const AccessID store = sparseStore[node];
      if (store)
        addIndirect(p.accesses_[store].value, flowNodes[node]);
      if (!killsInput(node))
        for (SparseNode predecessor : sparsePredecessors[node])
          if (component[predecessor] != component[node])
            addIndirect(flowNodes[predecessor], flowNodes[node]);
    }
    for (AccessID access : sig.accesses) {
      if (p.accesses_[access].kind != Program::AccessKind::Load)
        continue;
      const SparseNode source = loadDefinition[access];
      if (source == invalidSparse)
        throw std::logic_error(
            "ValueFlowPTA: relevant load has no sparse definition");
      addIndirect(flowNodes[source], p.accesses_[access].value);
    }
    addIndirect(p.objects_[object].initialStore, flowNodes[0]);
    flowTemplates.emplace(key, flowNodes[0]);
  }

  void fallback(const char *reason) {
    stats.usedWeakFallback = true;
    stats.fallbackReason = reason;
    // Rebuild, rather than leave edges justified by an invalidated schedule.
    reset(true);
  }

  void run() {
    analyzed = false;
    stats = {};
    reset(!config.enableStrongUpdates);
    while (true) {
      computePointedToBy();
      bool restart = false;
      for (ObjectID object : dirtySignatures) {
        if (!p.objects_[object].memory)
          continue;
        current[object] = signature(object);
        if (!processed[object] || current[object] == previous[object])
          continue;
        if (!weak) {
          for (AccessID strong : current[object].strong) {
            if (!previous[object].strong.count(strong)) {
              restart = true;
              break;
            }
          }
        }
        pending.insert(object);
      }
      dirtySignatures.clear();
      if (restart) {
        fallback("a new strong store invalidated the escape schedule");
        continue;
      }
      if (pending.empty())
        break;

      ObjectID chosen = 0;
      if (weak) {
        chosen = *pending.begin();
      } else {
        for (ObjectID object : pending) {
          if (!escapeBlocked(object)) {
            chosen = object;
            break;
          }
        }
        if (!chosen) {
          // Non-singletons cannot kill definitions, so breaking an escape
          // cycle at one of them is safe. Scalar cycles need a fallback.
          for (ObjectID object : pending)
            if (!p.objects_[object].singleton) {
              chosen = object;
              break;
            }
        }
        if (!chosen) {
          fallback(
              "scalar escape cycle or unresolved scalar escape dependency");
          continue;
        }
      }
      pending.erase(chosen);
      processed[chosen] = true;
      previous[chosen] = current[chosen];
      computeIndirect(chosen, current[chosen]);
    }
    for (const auto &sig : previous)
      finalStrong.insert(sig.strong.begin(), sig.strong.end());
    stats.strongUpdateSites = finalStrong.size();
    // Internal join IDs are implementation details, not Program node IDs.
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

} // namespace lotus::alias::vfg
