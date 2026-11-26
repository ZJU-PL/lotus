/**
 * @file HierarchicalClusterer.h
 * @brief Hierarchical (fastcluster) renumbering for points-to set IDs.
 *
 * Implements the object-clustering idea of SVF's
 * `NodeIDAllocator::Clusterer` (Sui and Xue, OOPSLA'21): objects that occur
 * together in points-to sets are placed at adjacent identifiers so that the
 * 64-bit word representation of `SVFGNodeBS` needs fewer words per set.
 *
 * Unlike `ObjectClustering`, this pass never merges objects.  It is a
 * bijection on object identifiers and therefore preserves points-to precision
 * and strong updates.
 *
 * The clustering itself is the vendored BSD `fastcluster` library
 * (`Utils/FastCluster`).  Regions are the connected components of the
 * co-occurrence graph; only regions large enough to span a word are clustered,
 * exactly like SVF's regioning.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace utils {

using ClusterObjectId = std::uint32_t;

/// Agglomerative linkage. `Best` runs single, complete and average and keeps
/// the mapping with the fewest words (SVF's HCLUST_METHOD_SVF_BEST).
enum class ClusterLinkage { Single, Complete, Average, Median, Best };

/// One distinct points-to set and how often it occurs.
struct ClusterSet {
  /// Sorted, unique object identifiers.
  std::vector<ClusterObjectId> objects;
  std::size_t occurrences = 1;
};

struct ClusterOptions {
  /// New identifiers are `idBase + rank`. Keep this above any identifier used
  /// for graph nodes so the two namespaces stay disjoint.
  ClusterObjectId idBase = 0;
  ClusterLinkage linkage = ClusterLinkage::Best;
  /// Word width of the target sparse bitset (SVFGNodeBS uses 64).
  std::size_t wordBits = 64;
  /// Regions larger than this are left in ascending order. The distance matrix
  /// is quadratic in the region size.
  std::size_t maxRegionObjects = 4000;
  /// Partition co-occurring objects into connected components first.
  bool regionObjects = true;
};

struct ClusterStatistics {
  std::size_t objects = 0;
  std::size_t uniqueSets = 0;
  std::size_t regions = 0;
  std::size_t clusteredRegions = 0;
  std::size_t largestRegion = 0;
  std::size_t largestClusteredRegion = 0;
  /// Occurrence-weighted average number of words per distinct set.
  double originalWordSpan = 0.0;
  double clusteredWordSpan = 0.0;
  ClusterLinkage bestLinkage = ClusterLinkage::Complete;
  bool fellBackToIdentity = false;
};

struct ClusterResult {
  /// old identifier -> new identifier (`options.idBase + rank`).
  std::unordered_map<ClusterObjectId, ClusterObjectId> oldToNew;
  /// rank -> old identifier. `newId == options.idBase + index`.
  std::vector<ClusterObjectId> newToOld;
  ClusterStatistics stats;
};

class HierarchicalClusterer {
public:
  using ObjectId = ClusterObjectId;
  using Set = ClusterSet;
  using Options = ClusterOptions;
  using Statistics = ClusterStatistics;
  using Result = ClusterResult;
  using Linkage = ClusterLinkage;

  /// @param sets     distinct points-to sets; may be empty.
  /// @param objects  the complete object universe (unclustered objects are
  ///                 kept as singleton regions). If empty, derived from @p
  ///                 sets.
  static Result cluster(const std::vector<Set> &sets,
                        const std::vector<ObjectId> &objects,
                        const Options &options = Options());
};

} // namespace utils
} // namespace lotus
