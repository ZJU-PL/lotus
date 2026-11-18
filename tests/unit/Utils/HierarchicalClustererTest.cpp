#include "Utils/ADT/HierarchicalClusterer.h"

#include "Alias/InclusionBased/FlowSensitive/Support/BitvectorClusterer.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>

using lotus::utils::ClusterLinkage;
using lotus::utils::ClusterObjectId;
using lotus::utils::ClusterOptions;
using lotus::utils::ClusterSet;
using lotus::utils::HierarchicalClusterer;

namespace {

ClusterSet makeSet(std::initializer_list<ClusterObjectId> objects,
                   std::size_t occurrences = 1) {
  ClusterSet set;
  set.objects.assign(objects.begin(), objects.end());
  std::sort(set.objects.begin(), set.objects.end());
  set.occurrences = occurrences;
  return set;
}

void expectBijection(const HierarchicalClusterer::Result &result,
                     const std::vector<ClusterObjectId> &universe,
                     ClusterObjectId idBase) {
  ASSERT_EQ(result.oldToNew.size(), universe.size());
  ASSERT_EQ(result.newToOld.size(), universe.size());
  std::set<ClusterObjectId> newIds;
  for (ClusterObjectId oldId : universe) {
    auto it = result.oldToNew.find(oldId);
    ASSERT_NE(it, result.oldToNew.end()) << "missing object " << oldId;
    const ClusterObjectId newId = it->second;
    EXPECT_GE(newId, idBase);
    newIds.insert(newId);
  }
  EXPECT_EQ(newIds.size(), universe.size()) << "mapping is not injective";

  for (const ClusterObjectId oldId : universe) {
    const ClusterObjectId newId = result.oldToNew.at(oldId);
    ASSERT_LT(newId - idBase, result.newToOld.size());
    EXPECT_EQ(result.newToOld[newId - idBase], oldId);
  }
}

} // namespace

TEST(HierarchicalClustererTest, EmptyInput) {
  const auto result = HierarchicalClusterer::cluster({}, {});
  EXPECT_TRUE(result.oldToNew.empty());
  EXPECT_TRUE(result.newToOld.empty());
  EXPECT_EQ(result.stats.objects, 0u);
}

TEST(HierarchicalClustererTest, SmallRegionKeepsSetsContiguous) {
  const std::vector<ClusterObjectId> objects{5, 7, 9, 11};
  const std::vector<ClusterSet> sets{makeSet({5, 11}), makeSet({7, 9})};
  const auto result = HierarchicalClusterer::cluster(sets, objects);

  expectBijection(result, objects, 0);
  // Fewer than 64 objects: no distance matrix is built, but objects of one
  // connected component still receive consecutive identifiers.
  EXPECT_EQ(result.stats.clusteredRegions, 0u);
  for (const ClusterSet &set : sets) {
    ClusterObjectId low = result.oldToNew.at(set.objects.front());
    ClusterObjectId high = low;
    for (ClusterObjectId object : set.objects) {
      low = std::min(low, result.oldToNew.at(object));
      high = std::max(high, result.oldToNew.at(object));
    }
    EXPECT_EQ(high - low + 1, set.objects.size());
  }
}

TEST(HierarchicalClustererTest, IdBaseIsRespected) {
  const std::vector<ClusterObjectId> objects{1, 2, 3};
  const std::vector<ClusterSet> sets{makeSet({1, 2, 3})};
  ClusterOptions options;
  options.idBase = 1u << 30;
  const auto result = HierarchicalClusterer::cluster(sets, objects, options);
  expectBijection(result, objects, options.idBase);
  for (const auto &entry : result.oldToNew)
    EXPECT_GE(entry.second, options.idBase);
}

TEST(HierarchicalClustererTest, CoOccurringObjectsBecomeAdjacent) {
  // One connected region of 256 objects: a bridging set makes every object
  // reachable, and 128 pair sets are far apart in the original ordering.
  constexpr ClusterObjectId kObjects = 256;
  std::vector<ClusterObjectId> universe;
  for (ClusterObjectId i = 0; i < kObjects; ++i)
    universe.push_back(i);

  std::vector<ClusterSet> sets;
  ClusterSet bridge;
  bridge.objects = universe;
  sets.push_back(bridge);
  for (ClusterObjectId i = 0; i < 128; ++i)
    sets.push_back(makeSet({i, i + 128}));

  const auto result = HierarchicalClusterer::cluster(sets, universe);
  expectBijection(result, universe, 0);

  EXPECT_EQ(result.stats.objects, kObjects);
  EXPECT_EQ(result.stats.regions, 1u);
  EXPECT_EQ(result.stats.clusteredRegions, 1u);
  EXPECT_LT(result.stats.clusteredWordSpan, result.stats.originalWordSpan);

  std::size_t pairsInOneWord = 0;
  for (ClusterObjectId i = 0; i < 128; ++i) {
    const ClusterObjectId lhs = result.oldToNew.at(i);
    const ClusterObjectId rhs = result.oldToNew.at(i + 128);
    if (lhs / 64 == rhs / 64)
      ++pairsInOneWord;
  }
  EXPECT_GE(pairsInOneWord, 120u)
      << "hierarchical clustering failed to co-locate most co-occurring pairs";
}

TEST(HierarchicalClustererTest, DisconnectedRegionsAreKeptSeparate) {
  // {0,1} and {100,101} are separate components; ranks must not interleave
  // the two regions' identities.
  const std::vector<ClusterObjectId> universe{0, 1, 100, 101};
  const std::vector<ClusterSet> sets{makeSet({0, 1}), makeSet({100, 101})};
  const auto result = HierarchicalClusterer::cluster(sets, universe);
  expectBijection(result, universe, 0);
  EXPECT_EQ(result.stats.regions, 2u);
}

TEST(HierarchicalClustererTest, DeterministicAcrossRuns) {
  std::vector<ClusterObjectId> universe;
  for (ClusterObjectId i = 0; i < 128; ++i)
    universe.push_back(i);
  std::vector<ClusterSet> sets;
  ClusterSet bridge;
  bridge.objects = universe;
  sets.push_back(bridge);
  for (ClusterObjectId i = 0; i < 64; ++i)
    sets.push_back(makeSet({i, i + 64}));

  const auto first = HierarchicalClusterer::cluster(sets, universe);
  const auto second = HierarchicalClusterer::cluster(sets, universe);
  ASSERT_EQ(first.oldToNew.size(), second.oldToNew.size());
  for (const auto &entry : first.oldToNew)
    EXPECT_EQ(entry.second, second.oldToNew.at(entry.first));
}

TEST(HierarchicalClustererTest, ExplicitMedianLinkageIsAccepted) {
  std::vector<ClusterObjectId> universe;
  for (ClusterObjectId i = 0; i < 64; ++i)
    universe.push_back(i);
  std::vector<ClusterSet> sets;
  for (ClusterObjectId i = 0; i < 32; ++i)
    sets.push_back(makeSet({i, i + 32}));
  ClusterSet all;
  all.objects = universe;
  sets.push_back(all);

  ClusterOptions options;
  options.linkage = ClusterLinkage::Median;
  const auto result = HierarchicalClusterer::cluster(sets, universe, options);
  expectBijection(result, universe, 0);
  EXPECT_FALSE(result.stats.fellBackToIdentity);
  EXPECT_EQ(result.stats.clusteredRegions, 1u);
  EXPECT_LE(result.stats.clusteredWordSpan, result.stats.originalWordSpan);
}

TEST(BitvectorClustererTest, HierarchicalMappingCoLocatesCoOccurringObjects) {
  using lotus::alias::flow_sensitive::BitvectorClusterer;
  std::vector<std::vector<ClusterObjectId>> sets;
  std::vector<ClusterObjectId> bridge;
  for (ClusterObjectId i = 0; i < 256; ++i)
    bridge.push_back(i);
  sets.push_back(bridge);
  for (ClusterObjectId i = 0; i < 128; ++i)
    sets.push_back({i, i + 128});

  const BitvectorClusterer clusterer = BitvectorClusterer::hierarchical(sets);
  std::set<ClusterObjectId> mapped;
  for (ClusterObjectId i = 0; i < 256; ++i)
    mapped.insert(clusterer.toClustered(i));
  EXPECT_EQ(mapped.size(), 256u);

  std::size_t pairsInOneWord = 0;
  for (ClusterObjectId i = 0; i < 128; ++i) {
    const ClusterObjectId lhs = clusterer.toClustered(i);
    const ClusterObjectId rhs = clusterer.toClustered(i + 128);
    EXPECT_EQ(clusterer.toOriginal(lhs), i);
    EXPECT_EQ(clusterer.toOriginal(rhs), i + 128);
    if (lhs / 64 == rhs / 64)
      ++pairsInOneWord;
  }
  EXPECT_GE(pairsInOneWord, 120u);
}
