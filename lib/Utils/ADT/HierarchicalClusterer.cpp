#include "Utils/ADT/HierarchicalClusterer.h"

#include "Utils/FastCluster/FastCluster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace utils {
namespace {

constexpr double kOccurrenceEpsilon = 0.0001;

/// Index into the condensed (upper triangle) distance matrix. Requires i < j.
std::size_t condensedIndex(std::size_t n, std::size_t i, std::size_t j) {
  return n * (n - 1) / 2 - (n - i) * (n - i - 1) / 2 + j - i - 1;
}

std::size_t wordsFor(std::size_t count, std::size_t wordBits) {
  return count == 0 ? 0 : (count - 1) / wordBits + 1;
}

double
wordSpanForSet(const HierarchicalClusterer::Set &set,
               const std::unordered_map<std::uint32_t, std::uint32_t> &denseOf,
               const std::vector<std::uint32_t> &rankOfDense,
               std::size_t wordBits) {
  if (set.objects.empty())
    return 0.0;
  std::uint32_t low = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t high = 0;
  for (std::uint32_t object : set.objects) {
    const std::uint32_t rank = rankOfDense[denseOf.at(object)];
    low = std::min(low, rank);
    high = std::max(high, rank);
  }
  const double words =
      static_cast<double>(high / wordBits - low / wordBits + 1);
  return words * static_cast<double>(set.occurrences);
}

/// One connected component of the co-occurrence graph.
struct Region {
  std::vector<std::uint32_t> objects;  // global dense indices, ascending
  std::vector<std::size_t> setIndices; // indices into the unique set table
};

int toFastClusterMethod(HierarchicalClusterer::Linkage linkage) {
  switch (linkage) {
  case HierarchicalClusterer::Linkage::Single:
    return HCLUST_METHOD_SINGLE;
  case HierarchicalClusterer::Linkage::Complete:
    return HCLUST_METHOD_COMPLETE;
  case HierarchicalClusterer::Linkage::Average:
    return HCLUST_METHOD_AVERAGE;
  case HierarchicalClusterer::Linkage::Median:
    return HCLUST_METHOD_MEDIAN;
  case HierarchicalClusterer::Linkage::Best:
    break;
  }
  return HCLUST_METHOD_COMPLETE;
}

/// SVF's distance matrix: sets that need fewer words are more similar, and
/// repeated occurrences slightly reduce the distance so that frequently seen
/// combinations win ties.
std::vector<double> buildDistanceMatrix(
    const std::vector<HierarchicalClusterer::Set> &sets,
    const std::vector<std::size_t> &setIndices,
    const std::unordered_map<std::uint32_t, std::uint32_t> &denseOf,
    const std::unordered_map<std::uint32_t, std::uint32_t> &localOf,
    std::size_t regionSize, std::size_t wordBits) {
  std::vector<double> distances(regionSize * (regionSize - 1) / 2,
                                static_cast<double>(regionSize * regionSize));

  for (std::size_t setIndex : setIndices) {
    const auto &objects = sets[setIndex].objects;
    const std::size_t occurrences =
        std::max<std::size_t>(1, sets[setIndex].occurrences);
    const double distance =
        static_cast<double>(wordsFor(objects.size(), wordBits));

    for (std::size_t i = 0; i < objects.size(); ++i) {
      const std::uint32_t ii = localOf.at(denseOf.at(objects[i]));
      for (std::size_t j = i + 1; j < objects.size(); ++j) {
        const std::uint32_t jj = localOf.at(denseOf.at(objects[j]));
        const std::size_t low = std::min(ii, jj);
        const std::size_t high = std::max(ii, jj);
        double &existing = distances[condensedIndex(regionSize, low, high)];
        if (distance < existing)
          existing = distance - kOccurrenceEpsilon;
        if (distance == std::ceil(existing)) {
          if (existing - static_cast<double>(occurrences) * kOccurrenceEpsilon >
              std::floor(existing)) {
            existing -= static_cast<double>(occurrences) * kOccurrenceEpsilon;
          } else {
            existing = std::floor(existing) + kOccurrenceEpsilon;
          }
        }
      }
    }
  }
  return distances;
}

/// Assigns leaf positions in left-to-right dendrogram order. The merge array
/// uses R's column-major convention: merge[i] is the left child of node i+1
/// and merge[(n-1)+i] the right child. Negative children are leaves
/// (-1 .. -n).
bool traverseDendrogram(const std::vector<int> &merge, std::size_t n,
                        std::vector<std::uint32_t> &localRank) {
  if (n < 2)
    return false;
  std::vector<char> visited(n, 0);
  std::vector<int> stack;
  stack.push_back(static_cast<int>(n - 1));
  std::uint32_t next = 0;

  while (!stack.empty()) {
    const int node = stack.back();
    stack.pop_back();
    if (node < 0) {
      const std::size_t leaf = static_cast<std::size_t>(-node - 1);
      if (leaf >= n)
        return false;
      localRank[leaf] = next++;
      continue;
    }
    if (node == 0 || static_cast<std::size_t>(node) >= n || visited[node])
      return false;
    visited[node] = 1;
    const int left = merge[static_cast<std::size_t>(node) - 1];
    const int right = merge[(n - 1) + static_cast<std::size_t>(node) - 1];
    // LIFO: push right first so the left subtree is emitted first.
    stack.push_back(right);
    stack.push_back(left);
  }
  return next == n;
}

} // namespace

HierarchicalClusterer::Result
HierarchicalClusterer::cluster(const std::vector<Set> &sets,
                               const std::vector<ObjectId> &objects,
                               const Options &options) {
  Result result;
  const std::size_t wordBits = options.wordBits == 0 ? 64 : options.wordBits;

  // Universal object universe, ascending. Objects outside all sets still get a
  // rank so the caller can remap them without special cases.
  std::vector<ObjectId> universe = objects;
  for (const Set &set : sets)
    universe.insert(universe.end(), set.objects.begin(), set.objects.end());
  std::sort(universe.begin(), universe.end());
  universe.erase(std::unique(universe.begin(), universe.end()), universe.end());

  if (universe.empty()) {
    result.stats = Statistics{};
    return result;
  }

  // Dense indexes: ascending original ID order, SVF's DENSE allocation.
  std::unordered_map<ObjectId, std::uint32_t> denseOf;
  denseOf.reserve(universe.size() * 2);
  for (std::size_t i = 0; i < universe.size(); ++i)
    denseOf.emplace(universe[i], static_cast<std::uint32_t>(i));

  // Deduplicate sets (defensively) and accumulate occurrences.
  std::vector<Set> uniqueSets;
  uniqueSets.reserve(sets.size());
  {
    // Map from canonical (sorted) set to its position in uniqueSets.
    struct VectorLess {
      bool operator()(const std::vector<ObjectId> &lhs,
                      const std::vector<ObjectId> &rhs) const {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                            rhs.end());
      }
    };
    std::vector<std::pair<std::vector<ObjectId>, std::size_t>> canonical;
    canonical.reserve(sets.size());
    for (const Set &set : sets) {
      if (set.objects.empty())
        continue;
      std::vector<ObjectId> sorted = set.objects;
      std::sort(sorted.begin(), sorted.end());
      sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
      // Drop IDs that are not part of the universe (defensive).
      sorted.erase(std::remove_if(sorted.begin(), sorted.end(),
                                  [&](ObjectId id) {
                                    return denseOf.find(id) == denseOf.end();
                                  }),
                   sorted.end());
      if (sorted.empty())
        continue;
      canonical.emplace_back(std::move(sorted),
                             std::max<std::size_t>(1, set.occurrences));
    }

    std::stable_sort(canonical.begin(), canonical.end(),
                     [](const auto &lhs, const auto &rhs) {
                       return VectorLess{}(lhs.first, rhs.first);
                     });
    for (auto &entry : canonical) {
      if (!uniqueSets.empty() && uniqueSets.back().objects == entry.first) {
        uniqueSets.back().occurrences += entry.second;
      } else {
        uniqueSets.push_back(Set{entry.first, entry.second});
      }
    }
  }

  result.stats.objects = universe.size();
  result.stats.uniqueSets = uniqueSets.size();
  result.stats.bestLinkage = Linkage::Complete;

  // Co-occurrence graph over dense indexes. A star per set is enough for
  // connected components and avoids a quadratic blow-up on large sets.
  std::vector<std::vector<std::uint32_t>> neighbours(universe.size());
  for (const Set &set : uniqueSets) {
    if (set.objects.size() < 2)
      continue;
    const std::uint32_t first = denseOf.at(set.objects.front());
    for (std::size_t i = 1; i < set.objects.size(); ++i) {
      const std::uint32_t other = denseOf.at(set.objects[i]);
      neighbours[first].push_back(other);
      neighbours[other].push_back(first);
    }
  }

  std::vector<Region> regions;
  std::vector<std::uint32_t> regionOf(
      universe.size(), std::numeric_limits<std::uint32_t>::max());
  if (options.regionObjects) {
    std::vector<std::uint32_t> stack;
    for (std::size_t start = 0; start < universe.size(); ++start) {
      if (regionOf[start] != std::numeric_limits<std::uint32_t>::max())
        continue;
      const std::uint32_t label = static_cast<std::uint32_t>(regions.size());
      Region region;
      stack.clear();
      stack.push_back(static_cast<std::uint32_t>(start));
      regionOf[start] = label;
      while (!stack.empty()) {
        const std::uint32_t current = stack.back();
        stack.pop_back();
        region.objects.push_back(current);
        for (std::uint32_t next : neighbours[current]) {
          if (regionOf[next] == std::numeric_limits<std::uint32_t>::max()) {
            regionOf[next] = label;
            stack.push_back(next);
          }
        }
      }
      std::sort(region.objects.begin(), region.objects.end());
      regions.push_back(std::move(region));
    }
  } else {
    Region region;
    region.objects.resize(universe.size());
    std::iota(region.objects.begin(), region.objects.end(), 0u);
    regions.push_back(std::move(region));
  }

  // Attach each set to the region its objects belong to.
  for (std::size_t setIndex = 0; setIndex < uniqueSets.size(); ++setIndex) {
    const std::uint32_t first =
        denseOf.at(uniqueSets[setIndex].objects.front());
    regions[regionOf[first]].setIndices.push_back(setIndex);
  }

  regions.erase(std::remove_if(regions.begin(), regions.end(),
                               [](const Region &region) {
                                 return region.objects.empty();
                               }),
                regions.end());

  std::vector<std::uint32_t> rankOf(universe.size(), 0);
  std::uint32_t allocCounter = 0;

  for (Region &region : regions) {
    const std::size_t regionSize = region.objects.size();

    // Local index within the region, ascending original object ID. Keyed by
    // dense index and sized by the region, not the whole universe.
    std::unordered_map<std::uint32_t, std::uint32_t> localOf;
    localOf.reserve(region.objects.size() * 2);
    for (std::size_t i = 0; i < region.objects.size(); ++i)
      localOf.emplace(region.objects[i], static_cast<std::uint32_t>(i));

    auto assignIdentity = [&]() {
      for (std::uint32_t dense : region.objects)
        rankOf[dense] = allocCounter++;
    };

    ++result.stats.regions;
    result.stats.largestRegion =
        std::max(result.stats.largestRegion, regionSize);

    if (regionSize < wordBits || regionSize > options.maxRegionObjects ||
        region.setIndices.empty()) {
      if (regionSize > options.maxRegionObjects)
        result.stats.fellBackToIdentity = true;
      assignIdentity();
      continue;
    }

    ++result.stats.clusteredRegions;
    result.stats.largestClusteredRegion =
        std::max(result.stats.largestClusteredRegion, regionSize);

    std::vector<double> distances = buildDistanceMatrix(
        uniqueSets, region.setIndices, denseOf, localOf, regionSize, wordBits);

    std::vector<Linkage> methods;
    if (options.linkage == Linkage::Best)
      methods = {Linkage::Single, Linkage::Complete, Linkage::Average};
    else
      methods = {options.linkage};

    bool found = false;
    std::vector<std::uint32_t> bestLocal;
    double bestWords = std::numeric_limits<double>::infinity();
    Linkage bestMethod = Linkage::Complete;

    // The ascending order is always a candidate: clustering must not make a
    // region worse than the allocation order it started from.
    {
      std::vector<std::uint32_t> identityLocal(regionSize);
      std::iota(identityLocal.begin(), identityLocal.end(), 0u);
      double words = 0.0;
      for (std::size_t setIndex : region.setIndices) {
        const Set &set = uniqueSets[setIndex];
        std::uint32_t low = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t high = 0;
        for (ObjectId object : set.objects) {
          const std::uint32_t local = localOf.at(denseOf.at(object));
          low = std::min(low, local);
          high = std::max(high, local);
        }
        words += static_cast<double>(high / wordBits - low / wordBits + 1) *
                 static_cast<double>(set.occurrences);
      }
      bestWords = words;
      bestLocal = std::move(identityLocal);
      found = true;
    }

    for (Linkage method : methods) {
      std::vector<int> merge(2 * (regionSize - 1));
      std::vector<double> height(regionSize - 1);
      std::vector<double> scratch =
          distances; // fastcluster consumes the matrix
      int status = 0;
      try {
        status = hclust_fast(static_cast<int>(regionSize), scratch.data(),
                             toFastClusterMethod(method), merge.data(),
                             height.data());
      } catch (const std::exception &) {
        status = 1;
      }
      if (status != 0)
        continue;

      std::vector<std::uint32_t> localRank(regionSize, 0);
      if (!traverseDendrogram(merge, regionSize, localRank))
        continue;

      // Evaluate: total words for the sets attached to this region.
      double words = 0.0;
      for (std::size_t setIndex : region.setIndices) {
        const Set &set = uniqueSets[setIndex];
        std::uint32_t low = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t high = 0;
        for (ObjectId object : set.objects) {
          const std::uint32_t local = localOf.at(denseOf.at(object));
          const std::uint32_t rank = localRank[local];
          low = std::min(low, rank);
          high = std::max(high, rank);
        }
        words += static_cast<double>(high / wordBits - low / wordBits + 1) *
                 static_cast<double>(set.occurrences);
      }

      if (words < bestWords) {
        bestWords = words;
        bestLocal = std::move(localRank);
        bestMethod = method;
        found = true;
      }
    }

    if (!found) {
      result.stats.fellBackToIdentity = true;
      assignIdentity();
      continue;
    }

    for (std::size_t i = 0; i < regionSize; ++i) {
      const std::uint32_t dense = region.objects[i];
      rankOf[dense] = allocCounter + bestLocal[i];
    }
    allocCounter += static_cast<std::uint32_t>(regionSize);
    result.stats.bestLinkage = bestMethod;
  }


  // Original (ascending-order) span versus the clustered span.
  std::vector<std::uint32_t> identityRank(universe.size());
  std::iota(identityRank.begin(), identityRank.end(), 0u);
  double originalWords = 0.0;
  double clusteredWords = 0.0;
  for (const Set &set : uniqueSets) {
    originalWords += wordSpanForSet(set, denseOf, identityRank, wordBits);
    clusteredWords += wordSpanForSet(set, denseOf, rankOf, wordBits);
  }
  result.stats.originalWordSpan = originalWords;
  result.stats.clusteredWordSpan = clusteredWords;

  result.newToOld.resize(universe.size());
  result.oldToNew.reserve(universe.size() * 2);
  for (std::size_t dense = 0; dense < universe.size(); ++dense) {
    const std::uint32_t rank = rankOf[dense];
    const ObjectId oldId = universe[dense];
    result.newToOld[rank] = oldId;
    result.oldToNew.emplace(oldId,
                            options.idBase + static_cast<ObjectId>(rank));
  }

  return result;
}

} // namespace utils
} // namespace lotus
