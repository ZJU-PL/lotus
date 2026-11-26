/**
 * @file BitvectorClusterer.h
 * @brief Permutation-based ID clustering for cache-dense bitvectors.
 *
 * Implements SVF-style NodeID clustering (NodeIDAllocator::Clusterer::cluster):
 * rather than merging objects semantically (which destroys must-alias
 * precision and disables strong updates), this renumbers ObjectIDs according to
 * their co-occurrence in points-to sets.
 *
 * By mapping frequently co-occurring pointees to contiguous IDs within the
 * same 64-bit word block, CoreBitVector / SVFGNodeBS set operations achieve
 * maximal cache density and word-skipping speedups without altering analysis
 * precision.
 */
#pragma once

#include "IR/SVFG/SVFGNodeBS.h"
#include "Utils/ADT/HierarchicalClusterer.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::alias::flow_sensitive {

class BitvectorClusterer {
public:
  using ObjectID = uint32_t;

  BitvectorClusterer() = default;

  template <typename PtsSetContainer>
  explicit BitvectorClusterer(const PtsSetContainer &sets,
                              ObjectID maxObjectId = 0) {
    build(sets, maxObjectId);
  }

  /// Build the permutation with the hierarchical fastcluster pass used by
  /// SVF's NodeIDAllocator::Clusterer (and by SVFGBuilder's
  /// objectIdClustering). Unlike build(), this evaluates several linkages and
  /// keeps the mapping with the fewest words. New IDs are `idBase + rank`.
  template <typename PtsSetContainer>
  static BitvectorClusterer hierarchical(
      const PtsSetContainer &sets, ObjectID idBase = 1,
      lotus::utils::ClusterLinkage linkage =
          lotus::utils::ClusterLinkage::Best) {
    std::vector<lotus::utils::ClusterSet> input;
    std::vector<ObjectID> universe;
    for (const auto &pointsTo : sets) {
      lotus::utils::ClusterSet set;
      set.objects.assign(pointsTo.begin(), pointsTo.end());
      universe.insert(universe.end(), set.objects.begin(), set.objects.end());
      if (!set.objects.empty())
        input.push_back(std::move(set));
    }

    lotus::utils::ClusterOptions options;
    options.idBase = idBase;
    options.linkage = linkage;
    const auto result =
        lotus::utils::HierarchicalClusterer::cluster(input, universe, options);

    BitvectorClusterer clusterer;
    for (const auto &entry : result.oldToNew) {
      clusterer.originalToClustered_[entry.first] = entry.second;
      clusterer.clusteredToOriginal_[entry.second] = entry.first;
    }
    return clusterer;
  }

  template <typename PtsSetContainer>
  void build(const PtsSetContainer &sets, ObjectID /*maxObjectId*/ = 0) {
    std::unordered_map<ObjectID, std::unordered_set<ObjectID>> coGraph;

    for (const auto &pts : sets) {
      if (pts.empty())
        continue;
      std::vector<ObjectID> elements;
      for (ObjectID obj : pts) {
        if (obj == 0)
          continue;
        elements.push_back(obj);
        coGraph[obj]; // Keep singleton sets in the permutation too.
      }
      for (std::size_t i = 0; i < elements.size(); ++i) {
        for (std::size_t j = i + 1; j < elements.size(); ++j) {
          coGraph[elements[i]].insert(elements[j]);
          coGraph[elements[j]].insert(elements[i]);
          // A spanning path keeps large sets together without constructing
          // a quadratic clique for each large points-to set.
          if (elements.size() > 64)
            break;
        }
      }
    }

    originalToClustered_.clear();
    clusteredToOriginal_.clear();
    std::unordered_set<ObjectID> visited;

    ObjectID nextId = 1;

    std::vector<ObjectID> allNodes;
    allNodes.reserve(coGraph.size());
    for (const auto &entry : coGraph)
      allNodes.push_back(entry.first);

    // Cuthill-McKee heuristic: process vertices by ascending degree
    std::sort(allNodes.begin(), allNodes.end(), [&](ObjectID a, ObjectID b) {
      return coGraph[a].size() == coGraph[b].size()
                 ? a < b
                 : coGraph[a].size() < coGraph[b].size();
    });

    for (ObjectID start : allNodes) {
      if (visited.count(start))
        continue;
      std::queue<ObjectID> queue;
      queue.push(start);
      visited.insert(start);

      while (!queue.empty()) {
        ObjectID curr = queue.front();
        queue.pop();

        originalToClustered_[curr] = nextId;
        clusteredToOriginal_[nextId] = curr;
        ++nextId;

        std::vector<ObjectID> neighbors;
        for (ObjectID nbr : coGraph[curr]) {
          if (!visited.count(nbr))
            neighbors.push_back(nbr);
        }
        std::sort(neighbors.begin(), neighbors.end(),
                  [&](ObjectID a, ObjectID b) {
                    return coGraph[a].size() == coGraph[b].size()
                               ? a < b
                               : coGraph[a].size() < coGraph[b].size();
                  });
        for (ObjectID nbr : neighbors) {
          if (visited.insert(nbr).second)
            queue.push(nbr);
        }
      }
    }

    // Complete each open permutation path into a cycle. IDs outside these
    // paths retain their identity without colliding with a dense destination.
    // This uses space proportional to the observed objects, not the largest ID
    // (Lotus's object IDs commonly start near 1 << 30).
    for (ObjectID start : allNodes) {
      if (clusteredToOriginal_.count(start) != 0)
        continue;
      ObjectID end = start;
      auto found = originalToClustered_.find(end);
      while (found != originalToClustered_.end()) {
        end = found->second;
        found = originalToClustered_.find(end);
      }
      originalToClustered_[end] = start;
      clusteredToOriginal_[start] = end;
    }
  }

  ObjectID toClustered(ObjectID id) const {
    auto it = originalToClustered_.find(id);
    return it != originalToClustered_.end() ? it->second : id;
  }

  ObjectID toOriginal(ObjectID id) const {
    auto it = clusteredToOriginal_.find(id);
    return it != clusteredToOriginal_.end() ? it->second : id;
  }

  analysis::SVFGNodeBS toClustered(const analysis::SVFGNodeBS &bs) const {
    analysis::SVFGNodeBS result;
    for (ObjectID id : bs)
      result.insert(toClustered(id));
    return result;
  }

  analysis::SVFGNodeBS toOriginal(const analysis::SVFGNodeBS &bs) const {
    analysis::SVFGNodeBS result;
    for (ObjectID id : bs)
      result.insert(toOriginal(id));
    return result;
  }

  template <typename PtsSetContainer>
  static double averageWordSpan(const PtsSetContainer &sets) {
    if (sets.empty())
      return 0.0;
    double totalWords = 0;
    std::size_t count = 0;
    for (const auto &pts : sets) {
      if (pts.empty())
        continue;
      ObjectID minId = std::numeric_limits<ObjectID>::max();
      ObjectID maxId = 0;
      for (ObjectID id : pts) {
        if (id < minId)
          minId = id;
        if (id > maxId)
          maxId = id;
      }
      totalWords += static_cast<double>((maxId / 64) - (minId / 64) + 1);
      ++count;
    }
    return count > 0 ? totalWords / count : 0.0;
  }

private:
  std::unordered_map<ObjectID, ObjectID> originalToClustered_;
  std::unordered_map<ObjectID, ObjectID> clusteredToOriginal_;
};

} // namespace lotus::alias::flow_sensitive
