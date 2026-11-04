#include "CFL/InterleavedDyck/StagedBounds/Solver.h"

#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"

#include <stdexcept>
#include <string>

namespace lotus::cfl::interleaved_dyck::staged_bounds {

ApproximationResult Solver::analyze(const Graph &input, BenchmarkKind benchmark,
                                    const Options &options) const {
  if (options.parity_groups == 0 ||
      options.parity_groups > detail::MAX_PARITY_GROUPS) {
    throw std::invalid_argument("parity_groups must be between 1 and " +
                                std::to_string(detail::MAX_PARITY_GROUPS));
  }

  ApproximationResult result;
  Graph graph = benchmark == BenchmarkKind::ValueFlow
                    ? detail::removeValueFlowUnreachable(input)
                    : input;
  result.regularization = detail::regularization(graph, benchmark);
  if (options.stage_completed) {
    options.stage_completed(Method::Regularization);
  }
  if (options.method == Method::Regularization) {
    return result;
  }

  result.intersection =
      intersection(graph, GrammarStrength::Classic, options.parity_groups);
  if (benchmark == BenchmarkKind::ValueFlow) {
    result.intersection =
        detail::filterBracketPaths(graph, result.intersection);
  }
  if (options.stage_completed) {
    options.stage_completed(Method::Intersection);
  }
  if (options.method == Method::Intersection) {
    return result;
  }

  graph = detail::retainMatchedLabels(
      detail::removeNotOnCandidatePaths(graph, result.intersection));
  Graph under_graph = graph;
  if (benchmark == BenchmarkKind::ValueFlow) {
    under_graph = detail::valueFlowTransform(under_graph);
  }
  result.underapproximation = underapproximation(under_graph);
  if (benchmark == BenchmarkKind::ValueFlow) {
    result.underapproximation =
        detail::filterValueFlowPairs(result.underapproximation);
  }
  if (options.stage_completed) {
    options.stage_completed(Method::Underapproximation);
  }
  if (options.method == Method::Underapproximation) {
    return result;
  }

  result.mutual_refinement = detail::refinedWithCondensation(
      graph, result.underapproximation, GrammarStrength::Classic,
      options.parity_groups, benchmark, options.factorized_tracing);
  graph = detail::retainMatchedLabels(
      detail::removeNotOnCandidatePaths(graph, result.mutual_refinement));
  if (options.stage_completed) {
    options.stage_completed(Method::MutualRefinement);
  }
  if (options.method == Method::MutualRefinement) {
    return result;
  }

  result.stronger_grammar = detail::refinedWithCondensation(
      graph, result.underapproximation, GrammarStrength::Parity,
      options.parity_groups, benchmark, options.factorized_tracing);
  graph = detail::retainMatchedLabels(
      detail::removeNotOnCandidatePaths(graph, result.stronger_grammar));
  if (options.stage_completed) {
    options.stage_completed(Method::StrongerGrammar);
  }
  if (options.method == Method::StrongerGrammar) {
    return result;
  }

  const PairSet classic_on_demand = detail::onDemand(
      graph, result.underapproximation, result.stronger_grammar,
      GrammarStrength::Classic, options.parity_groups, benchmark,
      options.factorized_tracing);
  graph = detail::retainMatchedLabels(
      detail::removeNotOnCandidatePaths(graph, classic_on_demand));
  result.on_demand =
      detail::onDemand(graph, result.underapproximation, classic_on_demand,
                       GrammarStrength::Parity, options.parity_groups,
                       benchmark, options.factorized_tracing);
  if (options.stage_completed) {
    options.stage_completed(Method::OnDemand);
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds
