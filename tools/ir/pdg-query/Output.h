#pragma once

#include <vector>

namespace pdg {
class ProgramGraph;
class CypherQueryExecutor;
class CypherResult;
class CypherQuery;
struct RuleDescriptor;
struct RuleEngineResult;
struct PDGQueryResult;
struct DiffQueryResult;
struct SummaryQueryResult;
struct ImpactQueryResult;
struct ResourceFlowQueryResult;
} // namespace pdg

namespace lotus::pdg_query {
struct Options;

/// Rendering only; parsing, rule selection and analysis stay in Driver.
class Output {
public:
  explicit Output(const Options &config);
  void printVersion() const;
  void printPDGInfo(pdg::ProgramGraph &graph) const;
  void printSchema() const;
  void printRules(const std::vector<pdg::RuleDescriptor> &rules) const;
  void
  printCweCatalog(const std::vector<pdg::RuleDescriptor> &selectedRules) const;
  void printRuleResult(const pdg::RuleEngineResult &result) const;
  void printResult(const pdg::PDGQueryResult &result) const;
  void printDiff(const pdg::DiffQueryResult &result) const;
  void printSummary(const pdg::SummaryQueryResult &result) const;
  void printImpact(const pdg::ImpactQueryResult &result) const;
  void printResourceFlow(const pdg::ResourceFlowQueryResult &result) const;
  void printQueryResult(pdg::CypherQueryExecutor &executor,
                        const pdg::CypherResult &result) const;
  void printQueryPlan(const pdg::CypherQuery &query) const;

private:
  void printResultText(const pdg::PDGQueryResult &result) const;
  void printResultJson(const pdg::PDGQueryResult &result) const;
  void printResultDot(const pdg::PDGQueryResult &result) const;
  void printSummaryText(const pdg::SummaryQueryResult &result) const;
  void printSummaryJson(const pdg::SummaryQueryResult &result) const;
  void printImpactText(const pdg::ImpactQueryResult &result) const;
  void printImpactJson(const pdg::ImpactQueryResult &result) const;
  void printResourceFlowText(const pdg::ResourceFlowQueryResult &result) const;
  void printResourceFlowJson(const pdg::ResourceFlowQueryResult &result) const;
  const Options &config_;
};
} // namespace lotus::pdg_query
