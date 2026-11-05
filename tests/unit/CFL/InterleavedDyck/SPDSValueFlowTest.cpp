#include "CFL/InterleavedDyck/SPDS/ValueFlow.h"
#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"

#include <functional>
#include <limits>
#include <random>

#include <gtest/gtest.h>

namespace d = lotus::cfl::interleaved_dyck;
namespace s = d::spds;

namespace {
// Directly interpret original graph paths: no product graph, pruning, CFL
// grammar or SPDS implementation is used by this oracle.
bool balanced(const std::vector<d::Label> &word, d::LabelKind open,
              d::LabelKind close) {
  std::vector<unsigned> stack;
  for (auto label : word) {
    if (label.kind == open)
      stack.push_back(label.id);
    else if (label.kind == close) {
      if (stack.empty() || stack.back() != label.id)
        return false;
      stack.pop_back();
    }
  }
  return stack.empty();
}

s::Result enumerateDAG(const d::Graph &graph, d::PairSet &exact) {
  s::Result result;
  std::map<d::Vertex, std::vector<d::Edge>> outgoing;
  for (auto edge : graph.edges())
    outgoing[edge.source].push_back(edge);
  for (auto source : graph.vertices()) {
    std::vector<d::Label> word;
    std::function<void(d::Vertex)> visit = [&](d::Vertex vertex) {
      if (source != vertex && !word.empty() &&
          word.front().kind == d::LabelKind::OpenBracket &&
          word.back().kind == d::LabelKind::CloseBracket) {
        const bool call = balanced(word, d::LabelKind::OpenParenthesis,
                                   d::LabelKind::CloseParenthesis);
        const bool field = balanced(word, d::LabelKind::OpenBracket,
                                    d::LabelKind::CloseBracket);
        if (call)
          result.parenthesis_pairs.insert({source, vertex});
        if (field)
          result.bracket_pairs.insert({source, vertex});
        if (call && field)
          exact.insert({source, vertex});
      }
      for (auto edge : outgoing[vertex]) {
        word.push_back(edge.label);
        visit(edge.target);
        word.pop_back();
      }
    };
    visit(source);
  }
  for (auto pair : result.parenthesis_pairs)
    if (result.bracket_pairs.count(pair))
      result.upper_bound.insert(pair);
  return result;
}
} // namespace

TEST(InterleavedDyckSPDSValueFlow, ExhaustiveOriginalPathsOnDAGs) {
  std::mt19937 random(20261104);
  for (unsigned trial = 0; trial < 300; ++trial) {
    SCOPED_TRACE(trial);
    d::Graph graph;
    // Sparse, negative IDs and shuffled insertion order exercise remapping.
    const std::vector<d::Vertex> ids{83, -91, 0, 715, -3, 44, 9};
    for (auto it = ids.rbegin(); it != ids.rend(); ++it)
      graph.addVertex(*it);
    for (unsigned u = 0; u < ids.size(); ++u)
      for (unsigned v = u + 1; v < ids.size(); ++v)
        for (unsigned choice = 0; choice < 2; ++choice) {
          if (random() % 3)
            continue;
          auto kind = static_cast<d::LabelKind>(random() % 5);
          d::Label label{kind, static_cast<unsigned>(random() % 3)};
          if (kind == d::LabelKind::Neutral)
            label = d::Label::neutral();
          graph.addEdge(ids[u], ids[v], label);
        }
    d::PairSet exact;
    const auto expected = enumerateDAG(graph, exact);
    const s::ValueFlowAnalysis analysis(graph);
    for (auto direction : {s::DemandDirection::Auto, s::DemandDirection::Post,
                           s::DemandDirection::Pre}) {
      const auto result = analysis.analyzeAll(direction);
      EXPECT_EQ(result.parenthesis_pairs, expected.parenthesis_pairs);
      EXPECT_EQ(result.bracket_pairs, expected.bracket_pairs);
      EXPECT_EQ(result.upper_bound, expected.upper_bound);
      for (auto pair : exact)
        EXPECT_TRUE(result.mayReach(pair.source, pair.target));
    }
  }
}

TEST(InterleavedDyckSPDSValueFlow, CrossingStacksAndUnboundedCycles) {
  d::Graph graph;
  // [7 (9 ]7 [8 )9 ]8 is balanced in both projections but not as one stack.
  const std::vector<d::Label> word{
      d::Label::openBracket(7),      d::Label::openParenthesis(9),
      d::Label::closeBracket(7),     d::Label::openBracket(8),
      d::Label::closeParenthesis(9), d::Label::closeBracket(8)};
  for (unsigned i = 0; i < word.size(); ++i)
    graph.addEdge(i, i + 1, word[i]);
  graph.addEdge(1, 1, d::Label::openParenthesis(42));
  graph.addEdge(5, 5, d::Label::closeParenthesis(42));
  const s::ValueFlowAnalysis analysis(graph);
  EXPECT_TRUE(analysis.analyzeFrom(0).mayReach(0, 6));
  EXPECT_TRUE(analysis.analyzeTo(6).mayReach(0, 6));
  EXPECT_EQ(analysis.analyzeAll(s::DemandDirection::Post).upper_bound,
            analysis.analyzeAll(s::DemandDirection::Pre).upper_bound);
}

TEST(InterleavedDyckSPDSValueFlow, EndpointsTypesAndIdentifiers) {
  d::Graph g;
  const auto lo = std::numeric_limits<d::Vertex>::min();
  const auto hi = std::numeric_limits<d::Vertex>::max();
  g.addEdge(lo, -3, d::Label::openBracket(7));
  g.addEdge(-3, 4, d::Label::openParenthesis(9));
  g.addEdge(4, 5, d::Label::closeParenthesis(9));
  g.addEdge(5, hi, d::Label::closeBracket(7));
  g.addEdge(5, 6, d::Label::closeBracket(8));
  g.addEdge(12, lo, d::Label::neutral());
  g.addEdge(hi, 13, d::Label::neutral());
  g.addVertex(99);
  s::ValueFlowAnalysis a(g);
  const d::PairSet expected{{lo, hi}};
  EXPECT_EQ(a.analyzeAll().upper_bound, expected);
  EXPECT_EQ(a.analyzeAll(s::DemandDirection::Pre).upper_bound, expected);
  EXPECT_EQ(a.analyzeFrom(lo).upper_bound, expected);
  EXPECT_EQ(a.analyzeTo(hi).upper_bound, expected);
  EXPECT_TRUE(a.analyzeFrom(99).upper_bound.empty());
  EXPECT_TRUE(a.analyzeTo(99).upper_bound.empty());
  EXPECT_EQ(
      a.analyzeDemands({{lo, hi}, {lo, hi}, {lo, 6}, {12, 13}}).upper_bound,
      expected);
  EXPECT_THROW(a.analyzeDemands({{lo, 42}}), std::invalid_argument);
  EXPECT_THROW(a.analyzeFrom(42), std::invalid_argument);
}

TEST(InterleavedDyckSPDSValueFlow, EmptyReflexiveAndLimits) {
  EXPECT_TRUE(
      s::ValueFlowAnalysis(d::Graph{}).analyzeAll().upper_bound.empty());
  d::Graph g;
  g.addEdge(0, 1, d::Label::openBracket(0));
  g.addEdge(1, 0, d::Label::closeBracket(0));
  EXPECT_TRUE(s::ValueFlowAnalysis(g).analyzeAll().upper_bound.empty());
  EXPECT_TRUE(
      s::ValueFlowAnalysis(g).analyzeDemands({{0, 0}}).upper_bound.empty());
  s::Options options;
  options.parentheses = s::StackAcceptance::Any;
  EXPECT_THROW(s::ValueFlowAnalysis(g, options), std::invalid_argument);
  options.parentheses = s::StackAcceptance::Empty;
  options.brackets = s::StackAcceptance::Any;
  EXPECT_THROW(s::ValueFlowAnalysis(g, options), std::invalid_argument);
  options.brackets = s::StackAcceptance::Empty;
  options.limits.max_transitions = 1;
  EXPECT_THROW(s::ValueFlowAnalysis(g, options).analyzeAll(), s::ResourceLimit);
}

TEST(InterleavedDyckSPDSValueFlow, EndpointConstraintAppliesToBothWitnesses) {
  d::Graph g;
  // A call-balanced neutral path cannot supply the call witness: it does
  // not start/end in brackets. The other path has an unmatched call.
  g.addEdge(0, 3, d::Label::neutral());
  g.addEdge(0, 1, d::Label::openBracket(0));
  g.addEdge(1, 2, d::Label::openParenthesis(0));
  g.addEdge(2, 3, d::Label::closeBracket(0));
  EXPECT_TRUE(s::Solver().prepare(g).analyzeAll().mayReach(0, 3));
  EXPECT_FALSE(s::ValueFlowAnalysis(g).analyzeAll().mayReach(0, 3));
  // Still an upper bound: a separate bracket-ended call-balanced path
  // can supply the call witness despite mismatching bracket types.
  g.addEdge(0, 4, d::Label::openBracket(1));
  g.addEdge(4, 3, d::Label::closeBracket(2));
  EXPECT_TRUE(s::ValueFlowAnalysis(g).analyzeAll().mayReach(0, 3));
}

TEST(InterleavedDyckSPDSValueFlow, DifferentialWithCFLProjectionClosure) {
  namespace detail = d::staged_bounds::detail;
  std::mt19937 random(71);
  for (unsigned trial = 0; trial < 60; ++trial) {
    SCOPED_TRACE(trial);
    d::Graph g;
    for (int v = 0; v < 6; ++v)
      g.addVertex(v);
    for (int e = 0; e < 14; ++e) {
      d::Label label{static_cast<d::LabelKind>(random() % 5),
                     static_cast<unsigned>(random() % 2)};
      if (label.kind == d::LabelKind::Neutral)
        label = d::Label::neutral();
      g.addEdge(random() % 6, random() % 6, label);
    }
    const auto product =
        detail::valueFlowTransform(detail::removeValueFlowUnreachable(g));
    const d::staged_bounds::Solver oracle;
    const auto expected =
        detail::filterValueFlowPairs(oracle.intersection(product));
    s::ValueFlowAnalysis a(g);
    const auto result = a.analyzeAll();
    EXPECT_EQ(result.parenthesis_pairs,
              detail::filterValueFlowPairs(oracle.projectedReachability(
                  product, d::staged_bounds::Alphabet::Parenthesis)));
    EXPECT_EQ(result.bracket_pairs,
              detail::filterValueFlowPairs(oracle.projectedReachability(
                  product, d::staged_bounds::Alphabet::Bracket)));
    EXPECT_EQ(a.analyzeAll(s::DemandDirection::Post).upper_bound, expected);
    EXPECT_EQ(a.analyzeAll(s::DemandDirection::Pre).upper_bound, expected);
    std::vector<d::Pair> demands;
    for (int u = 0; u < 6; ++u) {
      d::PairSet from, to;
      for (auto p : expected) {
        if (p.source == u)
          from.insert(p);
        if (p.target == u)
          to.insert(p);
      }
      EXPECT_EQ(a.analyzeFrom(u).upper_bound, from);
      EXPECT_EQ(a.analyzeTo(u).upper_bound, to);
      for (int v = 0; v < 6; ++v)
        demands.push_back({u, v});
    }
    EXPECT_EQ(a.analyzeDemands(demands).upper_bound, expected);
    EXPECT_EQ(a.analyzeDemands(demands, s::DemandDirection::Pre).upper_bound,
              expected);
  }
}
