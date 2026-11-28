#include "IR/PDG/Query/DiffQuery.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"

#include "QuerySupport.h"

#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace pdg {
using namespace llvm;
using namespace query_detail;

bool DiffQueryResult::isIdentical() const {
  for (size_t i = 0; i < node_diffs.size(); ++i) {
    if (node_diffs[i].kind != DiffKind::Preserved)
      return false;
  }
  for (size_t i = 0; i < edge_diffs.size(); ++i) {
    if (edge_diffs[i].kind != DiffKind::Preserved)
      return false;
  }
  return true;
}

static const Module *inferModuleFromNodes(const NodeSet &a, const NodeSet &b) {
  auto findIn = [](const NodeSet &nodes) -> const Module * {
    for (Node *n : nodes) {
      if (!n)
        continue;
      if (const Function *f = n->getFunc())
        return f->getParent();
      if (const Value *v = n->getValue()) {
        if (const auto *inst = dyn_cast<Instruction>(v))
          return inst->getModule();
        if (const auto *gv = dyn_cast<GlobalValue>(v))
          return gv->getParent();
      }
    }
    return nullptr;
  };
  if (const Module *m = findIn(a))
    return m;
  return findIn(b);
}

DiffQueryResult DiffQuery::diff(const PDGQueryResult &before,
                                const PDGQueryResult &after,
                                const PDGQueryOptions &options) const {
  DiffQueryResult result;
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);
  EdgeSet before_edges = collectInducedEdges(before.nodes, edge_types);
  EdgeSet after_edges = collectInducedEdges(after.nodes, edge_types);

  if (strategy_ == NodeMatchStrategy::PointerIdentity) {
    std::unordered_set<Node *> after_node_set(after.nodes.begin(),
                                              after.nodes.end());
    std::unordered_set<Node *> matched_after_nodes;
    matched_after_nodes.reserve(after.nodes.size());

    for (Node *node : before.nodes) {
      bool matched = after_node_set.count(node) != 0 &&
                     matched_after_nodes.insert(node).second;
      result.node_diffs.push_back(NodeDiffEntry{
          node, matched ? DiffKind::Preserved : DiffKind::Removed});
    }
    for (Node *node : after.nodes) {
      if (matched_after_nodes.count(node) == 0)
        result.node_diffs.push_back(NodeDiffEntry{node, DiffKind::Added});
    }

    struct PtrEdgeKey {
      Node *src;
      Node *dst;
      int type;
      bool operator==(const PtrEdgeKey &other) const {
        return src == other.src && dst == other.dst && type == other.type;
      }
    };
    struct PtrEdgeHash {
      size_t operator()(const PtrEdgeKey &k) const {
        size_t h = std::hash<Node *>()(k.src);
        h ^= std::hash<Node *>()(k.dst) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.type) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
      }
    };
    std::unordered_map<PtrEdgeKey, std::vector<Edge *>, PtrEdgeHash>
        after_edge_buckets;
    for (Edge *e : after_edges) {
      if (!e)
        continue;
      after_edge_buckets[PtrEdgeKey{e->getSrcNode(), e->getDstNode(),
                                    static_cast<int>(e->getEdgeType())}]
          .push_back(e);
    }
    std::unordered_set<Edge *> matched_after_edges;
    for (Edge *e : before_edges) {
      bool matched = false;
      if (e) {
        auto it = after_edge_buckets.find(
            PtrEdgeKey{e->getSrcNode(), e->getDstNode(),
                       static_cast<int>(e->getEdgeType())});
        if (it != after_edge_buckets.end() && !it->second.empty()) {
          Edge *cand = it->second.back();
          it->second.pop_back();
          matched_after_edges.insert(cand);
          matched = true;
        }
      }
      result.edge_diffs.push_back(EdgeDiffEntry{
          e, matched ? DiffKind::Preserved : DiffKind::Removed});
    }
    for (Edge *e : after_edges) {
      if (matched_after_edges.count(e) == 0)
        result.edge_diffs.push_back(EdgeDiffEntry{e, DiffKind::Added});
    }
  } else {
    const Module *mod = inferModuleFromNodes(before.nodes, after.nodes);
    std::unique_ptr<ModuleSlotTracker> slot_tracker;
    if (mod != nullptr)
      slot_tracker = std::make_unique<ModuleSlotTracker>(mod, false);
    llvm::DenseMap<Node *, std::string> key_cache;
    auto getKey = [&](Node *node) -> const std::string & {
      auto it = key_cache.find(node);
      if (it != key_cache.end())
        return it->second;
      return key_cache
          .try_emplace(node, stableNodeKey(node, slot_tracker.get()))
          .first->second;
    };

    std::unordered_map<std::string, std::vector<Node *>> after_node_buckets;
    for (Node *node : after.nodes)
      after_node_buckets[getKey(node)].push_back(node);

    std::unordered_set<Node *> matched_after_nodes;
    for (Node *node : before.nodes) {
      bool matched = false;
      auto it = after_node_buckets.find(getKey(node));
      if (it != after_node_buckets.end() && !it->second.empty()) {
        Node *cand = it->second.back();
        it->second.pop_back();
        matched_after_nodes.insert(cand);
        matched = true;
      }
      result.node_diffs.push_back(NodeDiffEntry{
          node, matched ? DiffKind::Preserved : DiffKind::Removed});
    }
    for (Node *node : after.nodes) {
      if (matched_after_nodes.count(node) == 0)
        result.node_diffs.push_back(NodeDiffEntry{node, DiffKind::Added});
    }

    auto edgeKey = [&](Edge *e) -> std::string {
      std::string k = std::to_string(static_cast<int>(e->getEdgeType()));
      k.push_back('\x1f');
      k += getKey(e->getSrcNode());
      k.push_back('\x1f');
      k += getKey(e->getDstNode());
      return k;
    };
    std::unordered_map<std::string, std::vector<Edge *>> after_edge_buckets;
    for (Edge *e : after_edges) {
      if (e)
        after_edge_buckets[edgeKey(e)].push_back(e);
    }
    std::unordered_set<Edge *> matched_after_edges;
    for (Edge *e : before_edges) {
      bool matched = false;
      if (e) {
        auto it = after_edge_buckets.find(edgeKey(e));
        if (it != after_edge_buckets.end() && !it->second.empty()) {
          Edge *cand = it->second.back();
          it->second.pop_back();
          matched_after_edges.insert(cand);
          matched = true;
        }
      }
      result.edge_diffs.push_back(EdgeDiffEntry{
          e, matched ? DiffKind::Preserved : DiffKind::Removed});
    }
    for (Edge *e : after_edges) {
      if (matched_after_edges.count(e) == 0)
        result.edge_diffs.push_back(EdgeDiffEntry{e, DiffKind::Added});
    }
  }

  for (size_t i = 0; i < result.node_diffs.size(); ++i) {
    if (result.node_diffs[i].kind == DiffKind::Preserved)
      continue;
    const std::string function = functionNameForNode(result.node_diffs[i].node);
    if (!function.empty())
      result.impact_summary.functions[function]++;
    const std::string source = sourceKeyForNode(result.node_diffs[i].node);
    if (!source.empty())
      result.impact_summary.source_locations[source]++;
  }

  return result;
}

DiffQueryResult DiffQuery::diff(const PDGQueryScope &before,
                                const PDGQueryScope &after,
                                const PDGQueryOptions &options) const {
  PDGQueryResult before_result;
  before_result.nodes = scopeNodes(pdg_, before);
  before_result.edges = collectInducedEdges(
      before_result.nodes, edgeTypesForPreset(options.edge_preset));
  PDGQueryResult after_result;
  after_result.nodes = scopeNodes(pdg_, after);
  after_result.edges = collectInducedEdges(
      after_result.nodes, edgeTypesForPreset(options.edge_preset));
  return diff(before_result, after_result, options);
}

} // namespace pdg
