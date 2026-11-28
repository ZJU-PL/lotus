#include "Solvers/EGraph.h"

#include <gtest/gtest.h>

using namespace lotus::egraph;

TEST(EGraphCoreTest, RebuildMergesCongruentParents) {
  EGraph<SymbolLang> egraph;

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id y = egraph.add(SymbolLang::leaf("y"));
  Id fx = egraph.add(SymbolLang("f", {x}));
  Id fy = egraph.add(SymbolLang("f", {y}));

  EXPECT_NE(egraph.find(fx), egraph.find(fy));

  egraph.unite(x, y, "eq");
  egraph.rebuild();

  EXPECT_EQ(egraph.find(x), egraph.find(y));
  EXPECT_EQ(egraph.find(fx), egraph.find(fy));
}

TEST(EGraphCoreTest, RebuildRepairsEveryNodeInMergedParentClass) {
  EGraph<SymbolLang> egraph;

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id y = egraph.add(SymbolLang::leaf("y"));
  Id z = egraph.add(SymbolLang::leaf("z"));
  Id fx = egraph.add(SymbolLang("f", {x}));
  Id gy = egraph.add(SymbolLang("g", {y}));
  Id gz = egraph.add(SymbolLang("g", {z}));

  egraph.unite(fx, gy);
  egraph.rebuild();
  ASSERT_NE(egraph.find(fx), egraph.find(gz));

  egraph.unite(z, y);
  egraph.rebuild();

  EXPECT_EQ(egraph.find(fx), egraph.find(gz));
  EXPECT_EQ(egraph.totalSize(), egraph.memoSize());
}

TEST(EGraphCoreTest, LookupUsesCanonicalChildren) {
  EGraph<SymbolLang> egraph;

  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  egraph.unite(a, b, "eq");
  egraph.rebuild();

  auto found = egraph.lookup(SymbolLang("g", {a}));
  EXPECT_FALSE(found.has_value());

  Id ga = egraph.add(SymbolLang("g", {a}));
  egraph.rebuild();

  auto gb = egraph.lookup(SymbolLang("g", {b}));
  ASSERT_TRUE(gb.has_value());
  EXPECT_EQ(egraph.find(ga), egraph.find(*gb));
}

TEST(EGraphCoreTest, UnionFindMatchesEggRepresentativeBehavior) {
  UnionFind uf;
  for (size_t i = 0; i < 10; ++i) {
    uf.makeSet();
  }

  uf.unite(Id::fromIndex(0), Id::fromIndex(1));
  uf.unite(Id::fromIndex(0), Id::fromIndex(2));
  uf.unite(Id::fromIndex(0), Id::fromIndex(3));

  uf.unite(Id::fromIndex(6), Id::fromIndex(7));
  uf.unite(Id::fromIndex(6), Id::fromIndex(8));
  uf.unite(Id::fromIndex(6), Id::fromIndex(9));

  for (size_t i = 0; i < 10; ++i) {
    (void)uf.findMut(Id::fromIndex(i));
  }

  EXPECT_EQ(uf.find(Id::fromIndex(0)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(1)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(2)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(3)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(4)), Id::fromIndex(4));
  EXPECT_EQ(uf.find(Id::fromIndex(5)), Id::fromIndex(5));
  EXPECT_EQ(uf.find(Id::fromIndex(6)), Id::fromIndex(6));
  EXPECT_EQ(uf.find(Id::fromIndex(7)), Id::fromIndex(6));
  EXPECT_EQ(uf.find(Id::fromIndex(8)), Id::fromIndex(6));
  EXPECT_EQ(uf.find(Id::fromIndex(9)), Id::fromIndex(6));
}

TEST(EGraphCoreTest, SimpleAddFromEggDoesNotBreakRebuild) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id x2 = egraph.add(SymbolLang::leaf("x"));
  Id plus = egraph.add(SymbolLang("+", {x, x2}));

  auto [merged, changed] = egraph.unionInstantiations(
      Pattern<SymbolLang>::parse("x").ast(),
      Pattern<SymbolLang>::parse("y").ast(), Subst{}, "union x and y");
  EXPECT_TRUE(changed);
  egraph.rebuild();

  EXPECT_EQ(egraph.find(x), egraph.find(merged));
  EXPECT_TRUE(egraph.lookup(SymbolLang("+", {x, x2})).has_value());
  EXPECT_EQ(egraph.find(plus),
            egraph.find(*egraph.lookup(SymbolLang("+", {x, x2}))));
}

TEST(EGraphCoreTest, FlatHashMapAndBinaryMemoTablePreserveTombstoneInvariants) {
  FlatHashMap<uint64_t, uint32_t> map;
  constexpr uint32_t kCount = 400;
  for (uint32_t i = 0; i < kCount; ++i) {
    auto [it, inserted] = map.try_emplace(static_cast<uint64_t>(i) * 17u + 1u, i);
    EXPECT_TRUE(inserted);
    EXPECT_EQ(it->second, i);
  }
  for (uint32_t i = 0; i < kCount; i += 2) {
    EXPECT_EQ(map.erase(static_cast<uint64_t>(i) * 17u + 1u), 1u);
    EXPECT_EQ(map.erase(static_cast<uint64_t>(i) * 17u + 1u), 0u);
  }
  EXPECT_EQ(map.size(), kCount / 2);
  FlatHashMap<uint64_t, uint32_t> copied(map);
  for (uint32_t i = 0; i < kCount; ++i) {
    auto it = copied.find(static_cast<uint64_t>(i) * 17u + 1u);
    if (i % 2 == 0) {
      EXPECT_EQ(it, copied.end());
    } else {
      ASSERT_NE(it, copied.end());
      EXPECT_EQ(it->second, i);
    }
  }
  for (uint32_t i = kCount; i < kCount * 3; ++i) {
    EXPECT_TRUE(map.try_emplace(static_cast<uint64_t>(i) * 17u + 1u, i).second);
  }
  for (uint32_t i = 0; i < kCount * 3; ++i) {
    auto it = map.find(static_cast<uint64_t>(i) * 17u + 1u);
    if (i < kCount && i % 2 == 0) {
      EXPECT_EQ(it, map.end());
    } else {
      ASSERT_NE(it, map.end());
      EXPECT_EQ(it->second, i);
    }
  }

  BinaryMemoTable bmemo;
  bmemo.ensureSpace(256);
  for (uint32_t i = 0; i < 200; ++i) {
    uint64_t k = BinaryMemoTable::packKey(Id(i), Id(i + 1000));
    uint64_t h = fastMix64(k);
    auto [slot, inserted] =
        bmemo.findOrInsertUnchecked(k, h, [&]() { return std::pair<uint32_t, Id>{i, Id(i + 5)}; });
    EXPECT_TRUE(inserted);
    EXPECT_EQ(slot->nidx, i);
  }
  for (uint32_t i = 0; i < 200; i += 3) {
    uint64_t k = BinaryMemoTable::packKey(Id(i), Id(i + 1000));
    uint64_t h = fastMix64(k);
    EXPECT_FALSE(bmemo.erase(k, h, i + 1)); // wrong nidx must not erase
    EXPECT_TRUE(bmemo.erase(k, h, i));
    EXPECT_EQ(bmemo.find(k, h), nullptr);
  }
  bmemo.reserve(1024);
  for (uint32_t i = 0; i < 200; ++i) {
    uint64_t k = BinaryMemoTable::packKey(Id(i), Id(i + 1000));
    auto *slot = bmemo.find(k, fastMix64(k));
    if (i % 3 == 0) {
      EXPECT_EQ(slot, nullptr);
    } else {
      ASSERT_NE(slot, nullptr);
      EXPECT_EQ(slot->nidx, i);
      EXPECT_EQ(slot->owner, Id(i + 5));
    }
  }
}

TEST(EGraphCoreTest, DeepBinaryCongruenceCascadeAndLazyNodesMatchEgg) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  Id ca = a;
  Id cb = b;
  // Exceeds the initial sequential scan threshold so parent-edge materialization
  // and lazy original-node materialization are both exercised.
  for (int i = 0; i < 24; ++i) {
    ca = egraph.add(SymbolLang("pair", {ca, a}));
    cb = egraph.add(SymbolLang("pair", {cb, b}));
  }
  egraph.rebuild();
  ASSERT_NE(egraph.find(ca), egraph.find(cb));

  egraph.unite(a, b);
  size_t unions = egraph.rebuild();

  EXPECT_EQ(unions, 24u);
  EXPECT_EQ(egraph.find(ca), egraph.find(cb));
  EXPECT_EQ(egraph.totalSize(), egraph.memoSize());
  EXPECT_FALSE(egraph.originalExpr(ca).items().empty());
  EXPECT_FALSE(egraph.nodes().empty());
}
