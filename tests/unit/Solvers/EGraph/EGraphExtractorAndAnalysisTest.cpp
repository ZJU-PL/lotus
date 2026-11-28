#include "EGraphFeatureTestSupport.h"

TEST(EGraphFeatureTest, ExtractorAstSizeOverflowKeepsMonotoneCosting) {
  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("explode", "(meow ?a)", "(meow (meow ?a ?a))")};

  auto start = RecExpr<SymbolLang>::parse("(meow 42)");
  Runner<SymbolLang> runner;
  runner.withIterLimit(100).withExpr(start).run(rules);

  Extractor<SymbolLang> extractor(runner.egraph);
  auto best = extractor.findBest(runner.roots[0]).second;
  EXPECT_EQ(best.toString(), start.toString());
}
TEST(EGraphFeatureTest, ExtractorSupportsEggStyleMutableCostFunctionApi) {
  EGraph<SymbolLang> egraph;
  Id root = egraph.addExpr(
      RecExpr<SymbolLang>::parse("(root (cheap a) (expensive b))"));
  egraph.rebuild();

  CountingCostFn cost_fn;
  EXPECT_EQ(cost_fn.costRec(
                RecExpr<SymbolLang>::parse("(root (cheap a) (expensive b))")),
            21u);
  EXPECT_GT(cost_fn.calls, 0u);

  Extractor<SymbolLang, NoAnalysis<SymbolLang>, CountingCostFn> extractor(
      egraph, cost_fn);
  auto [cost, best] = extractor.findBest(root);
  EXPECT_EQ(cost, 21u);
  EXPECT_EQ(best.toString(), "(root (cheap a) (expensive b))");
  EXPECT_EQ(extractor.findBestCost(root), 21u);
  EXPECT_EQ(extractor.findBestNode(root).op(), "root");
}

TEST(EGraphFeatureTest, ExtractorUsesParentWorklistInsteadOfGlobalRescans) {
  EGraph<SymbolLang> egraph;
  Id root = egraph.addExpr(RecExpr<SymbolLang>::parse("(f (g (h (i (j a)))))"));
  egraph.rebuild();

  auto calls = std::make_shared<size_t>(0);
  Extractor<SymbolLang, NoAnalysis<SymbolLang>, SharedCountingCostFn> extractor(
      egraph, SharedCountingCostFn(calls));

  EXPECT_EQ(*calls, egraph.totalSize());
  EXPECT_EQ(extractor.findBest(root).second.toString(),
            "(f (g (h (i (j a)))))");
}
TEST(EGraphFeatureTest, AstDepthCostRecMatchesEggStyleDepthMetric) {
  AstDepth<SymbolLang> depth;
  auto expr = RecExpr<SymbolLang>::parse("(f (g (h a)) b)");
  EXPECT_EQ(depth.costRec(expr), 4u);
}
TEST(EGraphFeatureTest, AstDepthSaturatesOnOverflowingChildDepth) {
  AstDepth<SymbolLang> depth;
  SymbolLang node(Symbol("f"), {Id::fromIndex(0)});

  EXPECT_EQ(
      depth.cost(node, [&](Id) { return std::numeric_limits<size_t>::max(); }),
      std::numeric_limits<size_t>::max());
}
TEST(EGraphFeatureTest,
     ExtractorKeepsExtractableLeafRepresentativeInCycleClass) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id fa = egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  Id root = egraph.addExpr(RecExpr<SymbolLang>::parse("(g ok)"));
  egraph.rebuild();
  egraph.unite(a, fa);
  egraph.rebuild();

  Extractor<SymbolLang> extractor(egraph);
  auto [cost, best] = extractor.findBest(root);
  EXPECT_EQ(cost, 2u);
  EXPECT_EQ(best.toString(), "(g ok)");
  auto [cyclic_cost, cyclic_best] = extractor.findBest(a);
  EXPECT_EQ(cyclic_cost, 1u);
  EXPECT_EQ(cyclic_best.toString(), "a");
}

TEST(EGraphFeatureTest, ExtractorPreservesSharedSubexpressionsAsDag) {
  EGraph<SymbolLang> egraph;
  Id x = egraph.add(SymbolLang::leaf(Symbol("x")));
  Id fx = egraph.add(SymbolLang(Symbol("f"), {x}));
  Id root = egraph.add(SymbolLang(Symbol("pair"), {fx, fx}));
  egraph.rebuild();

  Extractor<SymbolLang> extractor(egraph);
  auto [cost, best] = extractor.findBest(root);

  EXPECT_EQ(cost, 5u);
  EXPECT_EQ(best.size(), 3u);
  EXPECT_EQ(best.toString(), "(pair (f x) (f x))");
  EXPECT_EQ(best[best.root()].children()[0], best[best.root()].children()[1]);
}

TEST(EGraphFeatureTest, IdToExprSkipsRecursiveFrontNode) {
  Symbol recursive_op("__id_to_expr_recursive_first__");
  Symbol finite_op("__id_to_expr_finite_second__");
  EGraph<SymbolLang> egraph;
  Id finite = egraph.add(SymbolLang::leaf(finite_op));
  Id recursive = egraph.add(SymbolLang(recursive_op, {finite}));
  egraph.rebuild();
  egraph.unite(finite, recursive);
  egraph.rebuild();

  ASSERT_EQ(egraph[finite].nodes.front().op(), recursive_op);
  EXPECT_EQ(egraph.idToExpr(finite).toString(), finite_op.str());
}
TEST(EGraphFeatureTest, AnalysisPendingQueueDeduplicatesParentsLikeEgg) {
  DedupAnalysis::reset();
  EGraph<SymbolLang, DedupAnalysis> egraph;

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id y = egraph.add(SymbolLang::leaf("y"));
  Id plus1 = egraph.add(SymbolLang(Symbol("+"), {x, y}));
  Id plus2 = egraph.add(SymbolLang(Symbol("+"), {x, y}));
  (void)plus1;
  (void)plus2;
  egraph.rebuild();

  DedupAnalysis::reset();
  egraph.setAnalysisData(x, 7);
  egraph.rebuild();

  EXPECT_EQ(DedupAnalysis::remake_calls, 1);
}
TEST(EGraphFeatureTest, LanguageMapperDropsExplanationStateLikeEgg) {
  EGraph<SymbolLang> source = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = source.addUncanonical(SymbolLang::leaf("a"));
  Id b = source.addUncanonical(SymbolLang::leaf("b"));
  source.unite(a, b, "manual");
  Id fa = source.addUncanonical(SymbolLang(Symbol("f"), {a}));
  Id fb = source.addUncanonical(SymbolLang(Symbol("f"), {b}));
  source.rebuild();

  SimpleLanguageMapper<SymbolLang, NoAnalysis<SymbolLang>, SymbolLang,
                       NoAnalysis<SymbolLang>>
      mapper;
  auto mapped = mapper.mapEGraph(source);

  EXPECT_FALSE(mapped.areExplanationsEnabled());
  EXPECT_TRUE(mapped.clean());
  EXPECT_EQ(mapped.addExpr(RecExpr<SymbolLang>::parse("a")),
            mapped.addExpr(RecExpr<SymbolLang>::parse("b")));
  EXPECT_EQ(mapped.idToExpr(fa).toString(), "(f a)");
  EXPECT_EQ(mapped.idToExpr(fb).toString(), "(f a)");
  EXPECT_TRUE(mapped.unionEvents().empty());
  EXPECT_TRUE(mapped.explanationNodes().empty());
  EXPECT_FALSE(explainEquivalence(mapped, a, b).has_value());
}
TEST(EGraphFeatureTest, DefineLanguageSurfaceConstrainsFromOp) {
  auto plus = RecExpr<TestLang>::parse("(+ x y)");
  EXPECT_EQ(plus.toString(), "(+ x y)");

  EGraph<TestLang> egraph;
  Id root = egraph.addExpr(plus);
  egraph.rebuild();

  auto pat = Pattern<TestLang>::parse("(+ ?a ?b)");
  auto matches = pat.search(egraph);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(egraph.find(matches.front().eclass), egraph.find(root));

  EXPECT_THROW((void)RecExpr<TestLang>::parse("z"), std::runtime_error);
  EXPECT_THROW((void)RecExpr<TestLang>::parse("(- x y)"), std::runtime_error);
}

TEST(EGraphFeatureTest, TypedLanguagePreservesVariantsAndPayloads) {
  auto expr = RecExpr<TypedMathLang>::parse("(+ 41 (- pi))");
  const auto &add = expr[expr.root()].getAdd();

  ASSERT_TRUE(expr[add.ids[0]].isNumber());
  EXPECT_EQ(expr[add.ids[0]].getNumber().value, 41);

  const auto &neg = expr[add.ids[1]].getNeg();
  ASSERT_TRUE(expr[neg.ids[0]].isPi());
  EXPECT_EQ(expr.toString(), "(+ 41 (- pi))");
}

TEST(EGraphFeatureTest, TypedLanguageDistinguishesVariantsAndPayloadValues) {
  auto one = TypedMathLang::makeNumber(1);
  auto two = TypedMathLang::makeNumber(2);
  auto neg = TypedMathLang::makeNeg({Id::fromIndex(0)});
  auto sub = TypedMathLang::makeSub({Id::fromIndex(0), Id::fromIndex(1)});

  EXPECT_EQ(one.discriminant(), two.discriminant());
  EXPECT_TRUE(one.matches(TypedMathLang::makeNumber(1)));
  EXPECT_FALSE(one.matches(two));
  EXPECT_NE(neg.discriminant(), sub.discriminant());
  EXPECT_EQ(LanguageHash<TypedMathLang>{}(one),
            LanguageHash<TypedMathLang>{}(TypedMathLang::makeNumber(1)));
}

TEST(EGraphFeatureTest, TypedLanguageCodecsMatchRustFromStrSemantics) {
  auto true_value = TypedValueCodec<bool>::parse("true");
  auto false_value = TypedValueCodec<bool>::parse("false");
  ASSERT_TRUE(true_value.has_value());
  ASSERT_TRUE(false_value.has_value());
  EXPECT_TRUE(*true_value);
  EXPECT_FALSE(*false_value);
  EXPECT_FALSE(TypedValueCodec<bool>::parse("1").has_value());

  auto small = TypedValueCodec<int8_t>::parse("12");
  ASSERT_TRUE(small.has_value());
  EXPECT_EQ(static_cast<int>(*small), 12);
  EXPECT_EQ(TypedValueCodec<int8_t>::display(*small), "12");

  EXPECT_FALSE(TypedValueCodec<uint32_t>::parse("-1").has_value());
  EXPECT_FALSE(TypedValueCodec<uint8_t>::parse("256").has_value());
  EXPECT_EQ(TypedValueCodec<int32_t>::parse("+12"), 12);
  EXPECT_EQ(TypedValueCodec<uint32_t>::parse("+12"), 12u);

  auto expr = RecExpr<TypedPropLang>::parse("(~ true)");
  ASSERT_TRUE(expr[expr.root()].isNot());
  const Id child = expr[expr.root()].getNot().ids[0];
  ASSERT_TRUE(expr[child].isBool());
  EXPECT_TRUE(expr[child].getBool().value);
  EXPECT_EQ(expr.toString(), "(~ true)");
}

TEST(EGraphFeatureTest, TypedLanguageSupportsVariadicAndDataOperators) {
  auto list = RecExpr<TypedMathLang>::parse("(list 1 2 pi)");
  EXPECT_TRUE(list[list.root()].isList());
  EXPECT_EQ(list[list.root()].children().size(), 3u);

  auto named = RecExpr<TypedMathLang>::parse("(foo 1 2)");
  ASSERT_TRUE(named[named.root()].isNamedBinary());
  EXPECT_EQ(named[named.root()].getNamedBinary().value, Symbol("foo"));

  auto other = RecExpr<TypedMathLang>::parse("(call 1 2 3)");
  ASSERT_TRUE(other[other.root()].isOther());
  EXPECT_EQ(other[other.root()].getOther().value, Symbol("call"));
  EXPECT_EQ(other[other.root()].children().size(), 3u);
}

TEST(EGraphFeatureTest, TypedLanguageRunsThroughEGraphAndMatcher) {
  EGraph<TypedMathLang> egraph;
  Id root = egraph.addExpr(RecExpr<TypedMathLang>::parse("(+ 0 (list 1 2 3))"));
  egraph.rebuild();

  auto matches = Pattern<TypedMathLang>::parse("(+ 0 ?tail)").search(egraph);
  ASSERT_EQ(matches.size(), 1u);
  ASSERT_EQ(matches.front().substs.size(), 1u);
  EXPECT_EQ(egraph.find(matches.front().eclass), egraph.find(root));

  const Id *tail = matches.front().substs.front().get(Var::parse("?tail"));
  ASSERT_NE(tail, nullptr);
  EXPECT_TRUE(egraph[*tail].nodes.front().isList());
}

namespace {
struct CustomAstSize {
  using Cost = size_t;
  template <typename ChildCostFn>
  Cost cost(const SymbolLang &node, ChildCostFn &&child_cost) const {
    size_t total = 1;
    for (Id c : node.children()) {
      total += child_cost(c);
    }
    return total;
  }
};
} // namespace

TEST(EGraphFeatureTest, ExtractorBucketDijkstraMatchesGeneralCostOnBinaryDag) {
  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("comm", "(+ ?a ?b)", "(+ ?b ?a)"),
      makeRewrite<SymbolLang>("assoc", "(+ ?a (+ ?b ?c))", "(+ (+ ?a ?b) ?c)"),
      makeRewrite<SymbolLang>("zero-id", "(+ 0 ?x)", "?x"),
  };
  auto start = RecExpr<SymbolLang>::parse("(+ (+ x (+ y 0)) (+ 0 z))");
  Runner<SymbolLang> runner;
  runner.withExpr(start).withIterLimit(6).run(rules);

  Extractor<SymbolLang> ast_extractor(runner.egraph);
  Extractor<SymbolLang, NoAnalysis<SymbolLang>, CustomAstSize> gen_extractor(
      runner.egraph, CustomAstSize{});

  auto [fast_cost, fast_expr] = ast_extractor.findBest(runner.roots[0]);
  auto [gen_cost, gen_expr] = gen_extractor.findBest(runner.roots[0]);
  EXPECT_EQ(fast_cost, gen_cost);
  EXPECT_EQ(fast_cost, 5u);
  EXPECT_EQ(ast_extractor.findBestNode(runner.roots[0]).op(), "+");
  EXPECT_EQ(gen_extractor.findBestNode(runner.roots[0]).op(), "+");
  EXPECT_FALSE(fast_expr.toString().empty());
  EXPECT_FALSE(gen_expr.toString().empty());
}
