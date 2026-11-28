// Path expressions unit test (Tarjan path expression algorithm).

#include "Utils/Algorithms/PathExpressions/PathExpressions.h"

#include <gtest/gtest.h>

using namespace lotus::pathexpressions;

TEST(PathExpressionsTest, TwoNodesOneEdge) {
  GenericLabeledGraph<int, char> g;
  g.addNode(0);
  g.addNode(1);
  EXPECT_TRUE(g.addEdge(0, 'a', 1));
  EXPECT_FALSE(g.addEdge(0, 'a', 1)); // set semantics: duplicate edge ignored

  PathExpressionComputer<int, char> comp(g);
  auto expr_0_0 = comp.exprBetween(0, 0);
  auto expr_0_1 = comp.exprBetween(0, 1);
  auto expr_1_0 = comp.exprBetween(1, 0);

  EXPECT_TRUE(expr_0_0->isEpsilon()); // empty path
  EXPECT_FALSE(expr_0_1->isEpsilon());
  EXPECT_FALSE(expr_0_1->isEmptySet()); // path 0 -a-> 1
  EXPECT_TRUE(expr_1_0->isEmptySet());  // no path 1 -> 0
}

TEST(PathExpressionsTest, ThreeNodesTwoPaths) {
  GenericLabeledGraph<int, char> g;
  g.addNode(0);
  g.addNode(1);
  g.addNode(2);
  g.addEdge(0, 'a', 1);
  g.addEdge(1, 'b', 2);
  g.addEdge(0, 'c', 2);

  PathExpressionComputer<int, char> comp(g);
  auto expr_0_2 = comp.exprBetween(0, 2);

  EXPECT_FALSE(expr_0_2->isEmptySet());
  EXPECT_FALSE(expr_0_2->isEpsilon());
  EXPECT_TRUE(comp.exprBetween(0, 0)->isEpsilon());
  EXPECT_FALSE(comp.exprBetween(0, 1)->isEmptySet());
  EXPECT_FALSE(comp.exprBetween(0, 2)->isEmptySet());
}

TEST(PathExpressionsTest, SelfLoop) {
  GenericLabeledGraph<int, char> g;
  g.addNode(0);
  g.addEdge(0, 'x', 0);

  PathExpressionComputer<int, char> comp(g);
  auto expr_0_0 = comp.exprBetween(0, 0);
  // Paths: ε, x, xx, xxx, ...  =>  x*
  EXPECT_FALSE(expr_0_0->isEmptySet());
  EXPECT_FALSE(
      expr_0_0->isEpsilon()); // we get star(literal('x')) which is not epsilon
}

TEST(PathExpressionsTest, RegexSimplifications) {
  auto empty = Regex<char>::emptySet();
  auto eps = Regex<char>::epsilon();
  auto a = Regex<char>::literal('a');

  auto union_left = Regex<char>::simplifiedUnion(empty, a);
  EXPECT_TRUE(union_left->equals(*a));

  auto union_right = Regex<char>::simplifiedUnion(a, empty);
  EXPECT_TRUE(union_right->equals(*a));

  auto union_same = Regex<char>::simplifiedUnion(a, a);
  EXPECT_TRUE(union_same->equals(*a));

  auto concat_empty = Regex<char>::simplifiedConcatenation(empty, a);
  EXPECT_TRUE(concat_empty->isEmptySet());

  auto concat_eps_left = Regex<char>::simplifiedConcatenation(eps, a);
  EXPECT_TRUE(concat_eps_left->equals(*a));

  auto concat_eps_right = Regex<char>::simplifiedConcatenation(a, eps);
  EXPECT_TRUE(concat_eps_right->equals(*a));

  auto star_empty = Regex<char>::simplifiedStar(empty);
  EXPECT_TRUE(star_empty->isEpsilon());

  auto star_eps = Regex<char>::simplifiedStar(eps);
  EXPECT_TRUE(star_eps->isEpsilon());

  auto star_literal = Regex<char>::simplifiedStar(a);
  EXPECT_FALSE(star_literal->isEpsilon());
  EXPECT_FALSE(star_literal->isEmptySet());
}

TEST(PathExpressionsTest, RegexToTgf) {
  auto a = Regex<char>::literal('a');
  auto b = Regex<char>::literal('b');
  auto u = Regex<char>::union_(a, b);
  const std::string tgf = RegexToTgf<char>::apply(u);
  EXPECT_EQ(tgf, std::string("0 ∪\n1 a\n2 b\n#\n0 1 0\n0 2 1\n"));
}

TEST(PathExpressionsTest, RegexToCompactTgf) {
  auto a = Regex<char>::literal('a');
  auto b = Regex<char>::literal('b');
  auto c = Regex<char>::literal('c');
  auto u1 = Regex<char>::union_(a, b);
  auto u2 = Regex<char>::union_(u1, c); // ((a ∪ b) ∪ c)
  const std::string tgf = RegexToCompactTgf<char>::apply(u2);
  EXPECT_EQ(tgf, std::string("0 ∪\n1 a\n2 b\n3 c\n#\n0 1 0\n0 2 1\n0 3 2\n"));
}

TEST(PathExpressionsTest, FactorsCommonPrefixSuffixAndFlattensNestedStar) {
  auto a = Regex<char>::literal('a');
  auto b = Regex<char>::literal('b');
  auto c = Regex<char>::literal('c');

  auto ab = Regex<char>::concat(a, b);
  auto ac = Regex<char>::concat(a, c);
  auto factoredPrefix = Regex<char>::simplifiedUnion(ab, ac);
  auto expectedPrefix = Regex<char>::concat(a, Regex<char>::union_(b, c));
  EXPECT_TRUE(factoredPrefix->equals(*expectedPrefix));

  auto ba = Regex<char>::concat(b, a);
  auto ca = Regex<char>::concat(c, a);
  auto factoredSuffix = Regex<char>::simplifiedUnion(ba, ca);
  auto expectedSuffix = Regex<char>::concat(Regex<char>::union_(b, c), a);
  EXPECT_TRUE(factoredSuffix->equals(*expectedSuffix));

  auto starA = Regex<char>::simplifiedStar(a);
  auto starStarA = Regex<char>::simplifiedStar(starA);
  EXPECT_TRUE(starStarA->equals(*starA));
}

TEST(PathExpressionsTest, SequentialDiamondsRemainCompactAndDeterministic) {
  auto buildChain = []() {
    GenericLabeledGraph<int, int> g;
    constexpr int kStages = 20;
    g.addNode(0);
    int edgeId = 0;
    for (int s = 0; s < kStages; ++s) {
      int base = 3 * s;
      int next = base + 3;
      g.addNode(base + 1);
      g.addNode(base + 2);
      g.addNode(next);
      g.addEdge(base, edgeId++, base + 1);
      g.addEdge(base, edgeId++, base + 2);
      g.addEdge(base + 1, edgeId++, next);
      g.addEdge(base + 2, edgeId++, next);
    }
    PathExpressionComputer<int, int> comp(g);
    return comp.exprBetween(0, 3 * kStages);
  };

  auto re1 = buildChain();
  auto re2 = buildChain();
  ASSERT_NE(re1, nullptr);
  ASSERT_NE(re2, nullptr);
  EXPECT_FALSE(re1->isEmptySet());
  EXPECT_TRUE(re1->equals(*re2));
  EXPECT_EQ(re1->toString(), re2->toString());
  // Without common-prefix/suffix factoring + min-fill ordering, 20 diamonds
  // produce >= 2^20 nodes.
  EXPECT_LT(re1->toString().size(), 4096u);
}
