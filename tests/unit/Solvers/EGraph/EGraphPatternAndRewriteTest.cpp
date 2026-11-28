#include "EGraphFeatureTestSupport.h"

TEST(EGraphFeatureTest, RecExprRoundTripsSExpressions) {
  auto expr = RecExpr<SymbolLang>::parse("(f (g a) b)");

  EXPECT_EQ(expr.size(), 4u);
  EXPECT_EQ(expr.toString(), "(f (g a) b)");
}
TEST(EGraphFeatureTest, PatternSearchHandlesRepeatedVariables) {
  EGraph<SymbolLang> egraph;
  Id aa = egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a a)"));
  Id ab = egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a b)"));
  egraph.rebuild();

  Pattern<SymbolLang> same = Pattern<SymbolLang>::parse("(+ ?x ?x)");
  auto matches = same.search(egraph);
  EXPECT_EQ(same.nMatches(egraph), 1u);

  bool found_same = false;
  bool found_mixed = false;
  for (const auto &match : matches) {
    if (egraph.find(match.eclass) == egraph.find(aa)) {
      found_same = true;
    }
    if (egraph.find(match.eclass) == egraph.find(ab)) {
      found_mixed = true;
    }
  }

  EXPECT_TRUE(found_same);
  EXPECT_FALSE(found_mixed);
}

TEST(EGraphFeatureTest, PatternQuestionMarkOperatorsMatchEggParsingRules) {
  auto variable = Pattern<TypedMathLang>::parse("?x");
  ASSERT_TRUE(variable.ast()[variable.ast().root()].isVar());

  auto question = Pattern<TypedMathLang>::parse("?");
  const auto &question_root = question.ast()[question.ast().root()];
  ASSERT_TRUE(question_root.isNode());
  ASSERT_TRUE(question_root.node().isOther());
  EXPECT_EQ(question_root.node().getOther().value, Symbol("?"));
  EXPECT_TRUE(question_root.node().children().empty());

  EXPECT_THROW((void)Pattern<TypedMathLang>::parse("(?x 1)"),
               std::runtime_error);
}
TEST(EGraphFeatureTest, MultiPatternSearchFindsCompatibleClauses) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g b a)"));
  egraph.rebuild();

  MultiPattern<SymbolLang> multi({
      {Var::parse("?f"), Pattern<SymbolLang>::parse("(f ?x ?y)")},
      {Var::parse("?g"), Pattern<SymbolLang>::parse("(g ?x ?y)")},
  });

  auto matches = multi.search(egraph);
  EXPECT_EQ(matches.size(), 1u);
  ASSERT_NE(matches.front().get(Var::parse("?f")), nullptr);
  ASSERT_NE(matches.front().get(Var::parse("?g")), nullptr);
}
TEST(EGraphFeatureTest, MultiPatternRespectsRepeatedBinders) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g b a)"));
  egraph.rebuild();

  MultiPattern<SymbolLang> same_binder({
      {Var::parse("?v"), Pattern<SymbolLang>::parse("(f ?x ?y)")},
      {Var::parse("?v"), Pattern<SymbolLang>::parse("(g ?x ?y)")},
  });

  EXPECT_TRUE(same_binder.search(egraph).empty());
}
TEST(EGraphFeatureTest, RewriteRespectsCyclePolicy) {
  EGraph<SymbolLang, NoCycleAnalysis> cyclic;
  Id a = cyclic.add(SymbolLang::leaf("a"));
  Id fa = cyclic.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  cyclic.rebuild();
  cyclic.unite(a, fa);
  cyclic.rebuild();

  auto pat = Pattern<SymbolLang>::parse("(f ?x)");
  EXPECT_TRUE(pat.search(cyclic).empty());

  EGraph<SymbolLang, NoCycleAnalysis> egraph;
  egraph.add(SymbolLang::leaf("a"));
  egraph.rebuild();

  auto cycle =
      makeRewrite<SymbolLang, NoCycleAnalysis>("self-wrap", "?x", "(f ?x)");
  auto matches = cycle.search(egraph);
  auto applied = cycle.apply(egraph, matches);
  egraph.rebuild();

  ASSERT_EQ(matches.size(), 1u);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(egraph.totalSize(), 2u);
}

TEST(EGraphFeatureTest, CyclePolicyRejectsNestedCycles) {
  EGraph<SymbolLang, NoCycleAnalysis> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id fa = egraph.add(SymbolLang("f", {a}));
  Id ha = egraph.add(SymbolLang("h", {a}));
  egraph.rebuild();

  egraph.unite(a, fa);
  egraph.rebuild();

  auto pattern = Pattern<SymbolLang>::parse("(h (f ?x))");
  auto matches = pattern.searchEClass(egraph, ha);
  EXPECT_TRUE(!matches || matches->substs.empty());
}

TEST(EGraphFeatureTest, CyclePolicyAllowsSharedDagChildren) {
  EGraph<SymbolLang, NoCycleAnalysis> egraph;
  Id root = egraph.addExpr(RecExpr<SymbolLang>::parse("(pair (f a) (f a))"));
  egraph.rebuild();

  auto pattern = Pattern<SymbolLang>::parse("(pair (f ?x) (f ?x))");
  auto matches = pattern.searchEClass(egraph, root);
  ASSERT_TRUE(matches.has_value());
  EXPECT_EQ(matches->substs.size(), 1u);
}

TEST(EGraphFeatureTest, LargeEClassMatchingDoesNotAssumeDiscriminantOrdering) {
  EGraph<SymbolLang> egraph;
  std::optional<Id> root;

  for (size_t i = 0; i < 30; ++i) {
    Id child = egraph.add(SymbolLang::leaf("x" + std::to_string(i)));
    Id unary = egraph.add(SymbolLang("f", {child}));
    Id binary = egraph.add(SymbolLang("f", {child, child}));
    if (!root) {
      root = unary;
    } else {
      egraph.unite(*root, unary);
    }
    egraph.unite(*root, binary);
  }
  egraph.rebuild();

  auto pattern = Pattern<SymbolLang>::parse("(f ?x)");
  auto matches = pattern.searchEClass(egraph, *root);
  ASSERT_TRUE(matches.has_value());
  EXPECT_EQ(matches->substs.size(), 30u);
}
TEST(EGraphFeatureTest, RunnerTreatsUnionOnlyRewriteAsProgress) {
  auto rule = makeRewrite<SymbolLang>("merge-a-b", "a", "b");

  Runner<SymbolLang> runner;
  runner.withExpr(RecExpr<SymbolLang>::parse("a"))
      .withExpr(RecExpr<SymbolLang>::parse("b"))
      .withIterLimit(4)
      .run({rule});

  ASSERT_FALSE(runner.iterations.empty());
  EXPECT_EQ(runner.stop_reason.kind, StopReasonKind::Saturated);
  EXPECT_EQ(runner.egraph.numberOfClasses(), 1u);
}
TEST(EGraphFeatureTest,
     RunnerHookStopRecordsOtherReasonAndSkipsRuleApplication) {
  auto rule = makeRewrite<SymbolLang>("expand", "a", "(f a)");

  Runner<SymbolLang> runner;
  runner.withExpr(RecExpr<SymbolLang>::parse("a"))
      .withHook(std::function<std::optional<std::string>(Runner<SymbolLang> &)>(
          [](auto &) -> std::optional<std::string> {
            return std::string("hook stop");
          }))
      .run({rule});

  ASSERT_EQ(runner.iterations.size(), 1u);
  ASSERT_EQ(runner.stop_reason.kind, StopReasonKind::Other);
  EXPECT_EQ(runner.stop_reason.other_message, "hook stop");
  EXPECT_TRUE(runner.iterations.front().applied.empty());
  ASSERT_TRUE(runner.iterations.front().stop_reason.has_value());
  EXPECT_EQ(runner.iterations.front().stop_reason->kind, StopReasonKind::Other);
  EXPECT_EQ(runner.iterations.front().stop_reason->other_message, "hook stop");
}
TEST(EGraphFeatureTest, RunnerSearchLimitStopSkipsApplyPhase) {
  auto rule = makeRewrite<SymbolLang>("expand", "a", "(f a)");

  Runner<SymbolLang> runner;
  runner.withExpr(RecExpr<SymbolLang>::parse("a")).withNodeLimit(0).run({rule});

  ASSERT_EQ(runner.iterations.size(), 1u);
  ASSERT_EQ(runner.stop_reason.kind, StopReasonKind::NodeLimit);
  EXPECT_EQ(runner.stop_reason.limit, 1u);
  EXPECT_TRUE(runner.iterations.front().applied.empty());
  EXPECT_EQ(runner.egraph.totalSize(), 1u);
}
TEST(EGraphFeatureTest, RunnerBackoffDoNotBanDoesNotOverflowSearchLimit) {
  auto rule = makeRewrite<SymbolLang>("self", "a", "a");

  Runner<SymbolLang> runner;
  BackoffScheduler<SymbolLang, NoAnalysis<SymbolLang>> scheduler;
  scheduler.doNotBan("self");

  runner.withExpr(RecExpr<SymbolLang>::parse("a"))
      .withIterLimit(1)
      .withScheduler(std::move(scheduler))
      .run({rule});

  ASSERT_EQ(runner.iterations.size(), 1u);
  EXPECT_NE(runner.stop_reason.kind, StopReasonKind::NodeLimit);
  EXPECT_NE(runner.stop_reason.kind, StopReasonKind::Other);
  EXPECT_TRUE(runner.iterations.front().applied.empty());
}

TEST(EGraphFeatureTest, RunnerBackoffArithmeticSaturates) {
  constexpr size_t limit = std::numeric_limits<size_t>::max();
  EXPECT_EQ(detail::runnerSaturatingShiftLeft(limit, 1), limit);
  EXPECT_EQ(detail::runnerSaturatingShiftLeft(limit / 2 + 1, 1), limit);
  EXPECT_EQ(detail::runnerSaturatingShiftLeft(1, 1), 2u);
  EXPECT_EQ(detail::runnerSaturatingAdd(limit, 1), limit);
  EXPECT_EQ(detail::runnerSaturatingAdd(limit - 1, 1), limit);
}
TEST(EGraphFeatureTest, PatternSearchWithLimitUsesOperatorIndex) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a c)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(* a b)"));
  egraph.rebuild();

  auto pat = Pattern<SymbolLang>::parse("(+ ?x ?y)");
  auto matches = pat.searchWithLimit(egraph, 1);

  ASSERT_EQ(matches.size(), 1u);
  ASSERT_EQ(matches.front().substs.size(), 1u);
}

TEST(EGraphFeatureTest, PatternSearchCanBeCooperativelyStopped) {
  EGraph<SymbolLang> egraph;
  for (size_t i = 0; i < 32; ++i) {
    Id leaf = egraph.add(SymbolLang::leaf("leaf" + std::to_string(i)));
    egraph.add(SymbolLang("f", {leaf}));
  }
  egraph.rebuild();

  size_t polls = 0;
  WorkControl control{[&]() {
                        ++polls;
                        return true;
                      },
                      1};
  auto matches = Pattern<SymbolLang>::parse("(f ?x)").searchWithLimit(
      egraph, 100, &control);

  EXPECT_TRUE(matches.empty());
  EXPECT_GT(polls, 0u);
}
TEST(EGraphFeatureTest, RewriteBorrowSupportsBorrowedSearcher) {
  Pattern<SymbolLang> lhs = Pattern<SymbolLang>::parse("(+ ?a ?b)");
  Pattern<SymbolLang> rhs = Pattern<SymbolLang>::parse("(+ ?b ?a)");
  BorrowedSearcher searcher(lhs);
  RewriteBorrow<SymbolLang> rewrite =
      makeRewriteBorrow<SymbolLang>("commute", searcher, rhs);

  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(+ x y)"));
  egraph.rebuild();

  auto matches = rewrite.search(egraph);
  ASSERT_EQ(matches.size(), 1u);
  auto applied = rewrite.apply(egraph, matches);
  EXPECT_EQ(applied.size(), 1u);
}
TEST(EGraphFeatureTest, BorrowedSearcherExposesSearchEClassAndNMatchesParity) {
  Pattern<SymbolLang> lhs = Pattern<SymbolLang>::parse("(+ ?a ?b)");
  BorrowedSearcher searcher(lhs);

  EGraph<SymbolLang> egraph;
  Id root = egraph.addExpr(RecExpr<SymbolLang>::parse("(+ x y)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(+ y z)"));
  egraph.rebuild();

  auto match = searcher.searchEClass(egraph, root);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->substs.size(), 1u);
  EXPECT_EQ(searcher.nMatches(egraph), 2u);
}
TEST(EGraphFeatureTest,
     MakeConditionalRewriteBorrowSupportsConditionWithBorrowedVarState) {
  Var shared = Var::parse("?x");
  BorrowedVarCondition condition(shared);

  EXPECT_NO_THROW((void)makeConditionalRewriteBorrow<SymbolLang>(
      "borrowed-cond", "?x", "(f ?x)", condition));
}
TEST(EGraphFeatureTest,
     MakeConditionalRewriteBorrowRejectsUnboundBorrowedConditionVar) {
  Var shared = Var::parse("?y");
  BorrowedVarCondition condition(shared);

  EXPECT_THROW((void)makeConditionalRewriteBorrow<SymbolLang>(
                   "borrowed-cond", "?x", "(f ?x)", condition),
               std::runtime_error);
}
TEST(EGraphFeatureTest, MultiPatternParsesAndApplies) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g a b)"));
  egraph.rebuild();

  auto mp = MultiPattern<SymbolLang>::parse("?u = (f ?x ?y), ?v = (g ?x ?y)");
  auto matches = mp.search(egraph);
  ASSERT_EQ(matches.size(), 1u);

  auto added = mp.apply(egraph, matches);
  egraph.rebuild();
  EXPECT_EQ(added.size(), 1u);
}
TEST(EGraphFeatureTest, EquivsReturnsMatchingEClasses) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  auto matches = egraph.equivs(RecExpr<SymbolLang>::parse("(f a)"),
                               RecExpr<SymbolLang>::parse("(f a)"));
  EXPECT_FALSE(matches.empty());
  EXPECT_TRUE(egraph.equivalent(RecExpr<SymbolLang>::parse("(f a)"),
                                RecExpr<SymbolLang>::parse("(f a)")));
}
TEST(EGraphFeatureTest, PatternAlphaRenameCanonicalizesVariables) {
  auto pat = Pattern<SymbolLang>::parse("(f ?a ?b ?a)");
  auto renamed = pat.alphaRename();
  EXPECT_EQ(renamed.toString(), "(f ?x ?y ?x)");
}
TEST(EGraphFeatureTest, NumericVarsAndSubstInsertBehaveLikeEgg) {
  Var v = Var::parse("?#0012");
  ASSERT_TRUE(v.asU32().has_value());
  EXPECT_EQ(*v.asU32(), 12u);

  Subst subst = Subst::withCapacity(2);
  EXPECT_FALSE(subst.insert(v, Id::fromIndex(1)).has_value());
  auto old = subst.insert(v, Id::fromIndex(2));
  ASSERT_TRUE(old.has_value());
  EXPECT_EQ(old->value(), 1u);
}
TEST(EGraphFeatureTest, PatternProgramRunsWithLimit) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f b)"));
  egraph.rebuild();

  auto pattern = Pattern<SymbolLang>::parse("(f ?x)");
  auto program = PatternProgram<SymbolLang>::compileFromPattern(pattern);
  auto ids = egraph.classIds();
  auto substs = program.runWithLimit(egraph, ids.front(), 1);
  EXPECT_LE(substs.size(), 1u);
}
TEST(EGraphFeatureTest, PatternSearchWithLimitMatchesEggCounts) {
  auto init_expr =
      RecExpr<SymbolLang>::parse("(+ 1 (+ 2 (+ 3 (+ 4 (+ 5 6)))))");
  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("comm", "(+ ?x ?y)", "(+ ?y ?x)"),
      makeRewrite<SymbolLang>("assoc", "(+ ?x (+ ?y ?z))", "(+ (+ ?x ?y) ?z)"),
  };

  Runner<SymbolLang> runner;
  runner.withExpr(init_expr).run(rules);
  const auto &egraph = runner.egraph;

  auto len = [](const std::vector<SearchMatches<SymbolLang>> &matches) {
    size_t total = 0;
    for (const auto &match : matches) {
      total += match.substs.size();
    }
    return total;
  };

  auto pat = Pattern<SymbolLang>::parse("(+ ?x (+ ?y ?z))");
  auto matches = pat.search(egraph);
  EXPECT_EQ(len(matches), 2100u);

  for (size_t limit : {1u, 10u, 100u, 1000u, 10000u}) {
    auto limited = pat.searchWithLimit(egraph, limit);
    EXPECT_EQ(len(limited), std::min(limit, size_t(2100)));
  }

  auto root = egraph.lookupExpr(init_expr);
  ASSERT_TRUE(root.has_value());
  auto eclass_matches = pat.searchEClass(egraph, *root);
  ASSERT_TRUE(eclass_matches.has_value());
  EXPECT_EQ(eclass_matches->substs.size(), 540u);

  for (size_t limit : {1u, 10u, 100u, 1000u}) {
    auto limited = pat.searchEClassWithLimit(egraph, *root, limit);
    ASSERT_TRUE(limited.has_value());
    EXPECT_EQ(limited->substs.size(), std::min(limit, size_t(540)));
  }
}
TEST(EGraphFeatureTest, MultiRewriteRejectsUnboundVariableInApplier) {
  EXPECT_THROW((void)makeMultiRewrite<SymbolLang>(
                   "foo", MultiPattern<SymbolLang>::parse("?x = (foo ?y)"),
                   MultiPattern<SymbolLang>::parse("?x = ?z")),
               std::runtime_error);
}
TEST(EGraphFeatureTest, MultiRewriteAllowsLaterBindingsIntroducedInApplier) {
  EXPECT_NO_THROW((void)makeMultiRewrite<SymbolLang>(
      "foo", MultiPattern<SymbolLang>::parse("?x = (foo ?y)"),
      MultiPattern<SymbolLang>::parse("?z = (baz ?y), ?x = ?z")));
}
TEST(EGraphFeatureTest, MultiPatternSupportsUnboundRhsIntroductions) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("x"));

  std::vector<Rewrite<SymbolLang>> rules = {
      makeMultiRewrite<SymbolLang>(
          "rule1", MultiPattern<SymbolLang>::parse("?x = x"),
          MultiPattern<SymbolLang>::parse("?y = y, ?y = z")),
      makeMultiRewrite<SymbolLang>(
          "rule2", MultiPattern<SymbolLang>::parse("?x = x, ?y = y, ?z = z"),
          MultiPattern<SymbolLang>::parse("?y = y, ?y = z")),
  };

  Runner<SymbolLang> runner;
  runner.withEGraph(std::move(egraph)).run(rules);
  Id y = runner.egraph.addExpr(RecExpr<SymbolLang>::parse("y"));
  Id z = runner.egraph.addExpr(RecExpr<SymbolLang>::parse("z"));
  EXPECT_EQ(runner.egraph.find(y), runner.egraph.find(z));
}
TEST(EGraphFeatureTest, MultiPatternContextTransferMatchesEgg) {
  EGraph<SymbolLang> egraph;
  auto add_string = [&](std::string_view text) {
    return egraph.addExpr(RecExpr<SymbolLang>::parse(text));
  };

  add_string("(lte ctx1 ctx2)");
  add_string("(lte ctx2 ctx2)");
  add_string("(lte ctx1 ctx1)");
  Id x2 = add_string("(tag x ctx2)");
  Id y2 = add_string("(tag y ctx2)");
  Id z2 = add_string("(tag z ctx2)");

  Id x1 = add_string("(tag x ctx1)");
  Id y1 = add_string("(tag y ctx1)");
  Id z1 = add_string("(tag z ctx2)");
  egraph.unite(x1, y1);
  egraph.unite(y2, z2);

  std::vector<Rewrite<SymbolLang>> rules = {makeMultiRewrite<SymbolLang>(
      "context-transfer",
      MultiPattern<SymbolLang>::parse("?x = (tag ?a ?ctx1) = (tag ?b ?ctx1), "
                                      "?t = (lte ?ctx1 ?ctx2), "
                                      "?a1 = (tag ?a ?ctx2), "
                                      "?b1 = (tag ?b ?ctx2)"),
      MultiPattern<SymbolLang>::parse("?a1 = ?b1"))};

  Runner<SymbolLang> runner;
  runner.withEGraph(std::move(egraph)).run(rules);

  EXPECT_EQ(runner.egraph.find(x1), runner.egraph.find(y1));
  EXPECT_EQ(runner.egraph.find(y2), runner.egraph.find(z2));
  EXPECT_EQ(runner.egraph.find(x2), runner.egraph.find(y2));
  EXPECT_EQ(runner.egraph.find(x2), runner.egraph.find(z2));
  EXPECT_NE(runner.egraph.find(y1), runner.egraph.find(z1));
  EXPECT_NE(runner.egraph.find(x1), runner.egraph.find(z1));
}
TEST(EGraphFeatureTest, MultiPatternAllowsBareVariableAfterFirstClause) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  auto searcher = MultiPattern<SymbolLang>::parse("?a = (f ?x), ?y = ?y");
  auto applier = MultiPattern<SymbolLang>::parse("?a = (g ?x ?y)");
  auto rewrite = makeMultiRewrite<SymbolLang>("r", searcher, applier);

  auto matches = rewrite.search(egraph);
  EXPECT_FALSE(matches.empty());
  EXPECT_NO_THROW((void)rewrite.apply(egraph, matches));
}
TEST(EGraphFeatureTest, EmptyMultiPatternIsRejectedLikeEgg) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  MultiPattern<SymbolLang> empty(
      std::vector<std::pair<Var, Pattern<SymbolLang>>>{});
  EXPECT_THROW((void)empty.search(egraph), std::runtime_error);
  EXPECT_THROW((void)empty.searchWithLimit(egraph, 1), std::runtime_error);
  EXPECT_THROW((void)empty.nMatches(egraph), std::runtime_error);
  EXPECT_THROW(
      (void)empty.searchEClassWithLimit(egraph, egraph.classIds().front(), 1),
      std::runtime_error);
}
TEST(EGraphFeatureTest, SearchMatchesCarryPerMatchAstIntoSearcherResults) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f b)"));
  egraph.rebuild();

  auto rewrite = makeRewrite<SymbolLang>("strip-f", "(f ?x)", "?x");
  auto matches = rewrite.search(egraph);
  ASSERT_EQ(matches.size(), 2u);
  const auto *shared_ast = matches.front().ast.get();
  for (const auto &match : matches) {
    ASSERT_TRUE(match.ast);
    EXPECT_EQ(match.ast.get(), shared_ast);
    EXPECT_EQ(match.ast->toString(), "(f ?x)");
  }
}
TEST(EGraphFeatureTest,
     PatternApplierReturnsMatchedEClassIdWithoutExplanations) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f b)"));
  egraph.rebuild();

  auto rewrite = makeRewrite<SymbolLang>("a-to-b", "a", "b");
  auto matches = rewrite.search(egraph);
  ASSERT_EQ(matches.size(), 1u);
  ASSERT_EQ(matches.front().eclass, a);

  auto applied = rewrite.apply(egraph, matches);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied.front(), a);
  EXPECT_NE(applied.front(), b);
}
TEST(EGraphFeatureTest, SemiNaiveSiblingJoinMatchesFullSearchAcrossEpochs) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  Id c = egraph.add(SymbolLang::leaf("c"));
  Id d = egraph.add(SymbolLang::leaf("d"));

  // Epoch 1: outer (+) and right (* a c) present; left (* a b) added later.
  Id mac = egraph.add(SymbolLang("*", {a, c}));
  Id mab_holder = egraph.add(SymbolLang::leaf("slot_left"));
  Id root1 = egraph.add(SymbolLang("+", {mab_holder, mac}));
  egraph.rebuild();

  auto pat = Pattern<SymbolLang>::parse("(+ (* ?x ?y) (* ?x ?z))");
  const auto &rq = pat.relationalQuery();

  FlatMatches delta;
  uint32_t v0 = egraph.version();
  rq.runFlat(egraph, delta, std::numeric_limits<size_t>::max(), 0, nullptr, false);
  EXPECT_EQ(delta.count(), 0u);

  // Epoch 2: insert left child (* a b) in Branch 1 order (left NEW, outer & right OLD).
  Id mab = egraph.add(SymbolLang("*", {a, b}));
  egraph.unite(mab_holder, mab);
  egraph.rebuild();

  rq.runFlat(egraph, delta, std::numeric_limits<size_t>::max(), v0, nullptr, false);
  EXPECT_EQ(delta.count(), 1u);
  EXPECT_EQ(egraph.find(delta.data[0]), egraph.find(root1));

  // Epoch 3: insert right child (* a d) in Branch 2 order (right NEW, outer & left OLD).
  uint32_t v1 = egraph.version();
  Id mad = egraph.add(SymbolLang("*", {a, d}));
  egraph.unite(mac, mad);
  egraph.rebuild();

  rq.runFlat(egraph, delta, std::numeric_limits<size_t>::max(), v1, nullptr, false);
  EXPECT_EQ(delta.count(), 1u);
  EXPECT_EQ(egraph.find(delta.data[0]), egraph.find(root1));
}
TEST(EGraphFeatureTest, SemiNaiveGroundMergeAndBatchedBinarySaturationMatchBackoff) {
  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("comm", "(+ ?a ?b)", "(+ ?b ?a)"),
      makeRewrite<SymbolLang>("assoc", "(+ ?a (+ ?b ?c))", "(+ (+ ?a ?b) ?c)"),
      makeRewrite<SymbolLang>("ground-zero", "(+ zero ?x)", "?x"),
      makeRewrite<SymbolLang>("mk-zero", "alt_zero", "zero"),
  };
  auto expr = RecExpr<SymbolLang>::parse("(+ (+ a alt_zero) (+ b c))");

  Runner<SymbolLang> simple;
  simple.withExpr(expr)
      .withIterLimit(5)
      .withScheduler(SimpleScheduler<SymbolLang, NoAnalysis<SymbolLang>>{})
      .run(rules);

  Runner<SymbolLang> backoff;
  backoff.withExpr(expr)
      .withIterLimit(5)
      .withScheduler(BackoffScheduler<SymbolLang, NoAnalysis<SymbolLang>>{}
                         .withInitialMatchLimit(1000000))
      .run(rules);

  Id s_zero = simple.egraph.addExpr(RecExpr<SymbolLang>::parse("zero"));
  Id s_alt = simple.egraph.addExpr(RecExpr<SymbolLang>::parse("alt_zero"));
  EXPECT_EQ(simple.egraph.find(s_zero), simple.egraph.find(s_alt));
  EXPECT_EQ(simple.egraph.totalSize(), backoff.egraph.totalSize());
  EXPECT_EQ(simple.egraph.numberOfClasses(), backoff.egraph.numberOfClasses());
}
