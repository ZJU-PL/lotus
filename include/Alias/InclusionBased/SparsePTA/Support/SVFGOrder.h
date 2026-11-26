/**
 * @file SVFGOrder.h
 * @brief Strongly-connected-component ordering shared by the sparse and
 *        versioned flow-sensitive pointer analyses.
 *
 * SVF solves both `fspta` and `vfspta` through `WPAFSSolver`, which seeds its
 * worklist from `SCCDetect`: statements are visited in reverse topological
 * order of their SVFG SCCs, and only the successors of a changed statement are
 * revisited.  Lotus's `FlowSensitivePTA` already does this; this header makes
 * the same order available to `VersionedFlowSensitivePTA` instead of
 * duplicating the traversal.
 *
 * The traversal intentionally matches the one previously embedded in
 * `FlowSensitivePTA::computeSCCs()` so existing iteration counts and worklist
 * behaviour are unchanged.
 */
#pragma once

#include "IR/SVFG/SVFG.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::alias::flow_sensitive {

/// Result of an SVFG-wide SCC decomposition.
struct SVFGSCC {
  /// Components in reverse topological order (Tarjan emission order): a
  /// component never precedes one of its successors here.
  std::vector<std::vector<const lotus::analysis::SVFGNode *>> components;
  /// NodeID -> index in `components`.
  std::unordered_map<std::uint32_t, std::size_t> componentOf;
  /// Inter-component successors, deduplicated.
  std::vector<std::vector<std::size_t>> successors;
};

/// Compute the SCC decomposition of @p graph restricted to @p inScope.
template <typename InScope>
SVFGSCC computeSVFGSCCs(const lotus::analysis::SVFG &graph, InScope inScope) {
  using NodeID = std::uint32_t;
  SVFGSCC result;
  std::unordered_map<NodeID, int> index, lowlink;
  std::unordered_set<NodeID> onStack;
  std::vector<const lotus::analysis::SVFGNode *> stack;
  int nextIndex = 0;
  std::function<void(const lotus::analysis::SVFGNode *)> visit =
      [&](const lotus::analysis::SVFGNode *node) {
        index[node->getId()] = lowlink[node->getId()] = nextIndex++;
        stack.push_back(node);
        onStack.insert(node->getId());
        for (const lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
          const lotus::analysis::SVFGNode *successor =
              edge ? edge->getDstNode() : nullptr;
          if (!inScope(successor))
            continue;
          if (index.find(successor->getId()) == index.end()) {
            visit(successor);
            lowlink[node->getId()] =
                std::min(lowlink[node->getId()], lowlink[successor->getId()]);
          } else if (onStack.count(successor->getId()) != 0) {
            lowlink[node->getId()] =
                std::min(lowlink[node->getId()], index[successor->getId()]);
          }
        }
        if (lowlink[node->getId()] != index[node->getId()])
          return;
        result.components.emplace_back();
        do {
          const lotus::analysis::SVFGNode *member = stack.back();
          stack.pop_back();
          onStack.erase(member->getId());
          result.components.back().push_back(member);
          if (member == node)
            break;
        } while (!stack.empty());
      };
  for (const auto &entry : graph)
    if (inScope(entry.second) && index.find(entry.first) == index.end())
      visit(entry.second);

  for (std::size_t i = 0; i < result.components.size(); ++i)
    for (const lotus::analysis::SVFGNode *node : result.components[i])
      result.componentOf[node->getId()] = i;
  result.successors.resize(result.components.size());
  for (std::size_t i = 0; i < result.components.size(); ++i) {
    std::unordered_set<std::size_t> successors;
    for (const lotus::analysis::SVFGNode *node : result.components[i])
      for (const lotus::analysis::SVFGEdge *edge : node->getOutEdges())
        if (edge && inScope(edge->getDstNode())) {
          const auto targetIt =
              result.componentOf.find(edge->getDstNode()->getId());
          if (targetIt == result.componentOf.end())
            continue;
          if (targetIt->second != i)
            successors.insert(targetIt->second);
        }
    result.successors[i].assign(successors.begin(), successors.end());
    std::sort(result.successors[i].begin(), result.successors[i].end());
  }
  return result;
}

} // namespace lotus::alias::flow_sensitive
