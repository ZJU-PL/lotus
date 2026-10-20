#include "Driver.h"

#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Analysis/PropertySpec.h"
#include "IR/PDG/Analysis/Query.h"
#include "IR/PDG/QueryLanguage/Cypher.h"
#include "Options.h"
#include "Output.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace llvm;
using namespace pdg;

namespace lotus::pdg_query {
namespace {

static bool selectedRuleIds(std::vector<std::string> &ids,
                            const Options &config) {
  for (const auto &id : config.ruleIds)
    if (std::none_of(
            RuleQuery::catalog().begin(), RuleQuery::catalog().end(),
            [&](const RuleDescriptor &rule) { return rule.id == id; })) {
      errs() << "Unknown PDG rule: " << id << "\n";
      return false;
    }
  if (config.cweIds.empty()) {
    ids.assign(config.ruleIds.begin(), config.ruleIds.end());
    return true;
  }
  for (const auto &rule : RuleQuery::catalog()) {
    bool tagged =
        std::any_of(rule.cwes.begin(), rule.cwes.end(), [&](unsigned cwe) {
          return std::find(config.cweIds.begin(), config.cweIds.end(), cwe) !=
                 config.cweIds.end();
        });
    bool named = config.ruleIds.empty() ||
                 std::find(config.ruleIds.begin(), config.ruleIds.end(),
                           rule.id) != config.ruleIds.end();
    if (tagged && named)
      ids.push_back(rule.id);
  }
  if (ids.empty()) {
    errs() << "No implemented rules match the requested CWE/rule selection\n";
    return false;
  }
  return true;
}

static PDGEdgePreset parseEdgePreset(const Options &config) {
  const std::string preset = StringRef(config.edgePreset).lower();
  if (preset == "data")
    return PDGEdgePreset::Data;
  if (preset == "control")
    return PDGEdgePreset::Control;
  if (preset == "parameter")
    return PDGEdgePreset::Parameter;
  if (preset == "interprocedural")
    return PDGEdgePreset::Interprocedural;
  if (preset == "value-flow")
    return PDGEdgePreset::ValueFlow;
  if (preset == "transform-legality")
    return PDGEdgePreset::TransformLegality;
  return PDGEdgePreset::All;
}

static SummaryKind parseSummaryKind(const Options &config) {
  const std::string kind = StringRef(config.summaryKind).lower();
  if (kind == "input-to-return")
    return SummaryKind::InputToReturn;
  if (kind == "input-to-global-write")
    return SummaryKind::InputToGlobalWrite;
  if (kind == "input-to-callsite")
    return SummaryKind::InputToCallsite;
  if (kind == "global-readers")
    return SummaryKind::GlobalReaders;
  if (kind == "global-writers")
    return SummaryKind::GlobalWriters;
  if (kind == "control-predicates")
    return SummaryKind::ControlPredicates;
  if (kind == "reachable-calls")
    return SummaryKind::ReachableCalls;
  if (kind == "resource-kinds")
    return SummaryKind::ResourceKinds;
  return SummaryKind::All;
}

static ResourceKind parseResourceKind(const Options &config) {
  const std::string kind = StringRef(config.resourceKind).lower();
  if (kind == "heap")
    return ResourceKind::Heap;
  if (kind == "file")
    return ResourceKind::File;
  if (kind == "fd")
    return ResourceKind::FileDescriptor;
  if (kind == "dir")
    return ResourceKind::Directory;
  if (kind == "lock")
    return ResourceKind::Lock;
  return ResourceKind::Unknown;
}

static bool executeQuery(CypherQueryExecutor &executor,
                         const std::string &query_string, const Options &config,
                         const Output &output) {
  CypherParser parser;
  CypherQueryParameters params;
  for (const auto &kv : config.queryParams) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) {
      errs() << "Invalid --param (expected key=value): " << kv << "\n";
      return false;
    }
    params[kv.substr(0, eq)] = kv.substr(eq + 1);
  }

  std::unique_ptr<CypherQuery> query = params.empty()
                                           ? parser.parse(query_string)
                                           : parser.parse(query_string, params);
  if (!query) {
    errs() << "Parse error: " << parser.getLastError().message << "\n";
    return false;
  }

  if (config.explain)
    output.printQueryPlan(*query);

  if (!query->hasLimit() && config.resultLimit > 0)
    const_cast<CypherQuery *>(query.get())->setLimit(config.resultLimit);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  if (!result) {
    errs() << "Error: " << executor.getLastError() << "\n";
    return false;
  }

  output.printQueryResult(executor, *result);

  return true;
}

static void runInteractiveMode(CypherQueryExecutor &executor,
                               const Options &config, const Output &output) {
  outs() << "PDG Query (type 'help' or 'quit')\n> ";

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      outs() << "> ";
      continue;
    }
    if (line == "quit" || line == "exit")
      break;
    if (line == "info")
      output.printPDGInfo(executor.getPDG());
    else if (line == "help")
      outs() << "Commands: help, quit, info\n";
    else
      executeQuery(executor, line, config, output);
    outs() << "> ";
  }
}

static std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos)
    return "";
  const auto last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

static std::string toUpper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return text;
}

static bool startsWithClause(const std::string &text, const char *clause) {
  const std::string upper = toUpper(text);
  const std::string keyword = toUpper(clause);
  if (upper.rfind(keyword, 0) != 0)
    return false;
  return upper.size() == keyword.size() ||
         std::isspace(static_cast<unsigned char>(upper[keyword.size()])) ||
         upper[keyword.size()] == '(';
}

static bool endsWithUnion(const std::string &text) {
  const std::string upper = toUpper(trim(text));
  return upper.size() >= 5 && upper.compare(upper.size() - 5, 5, "UNION") == 0;
}

static bool queryIsComplete(const std::string &text) {
  const std::string upper = toUpper(text);
  return upper.find("RETURN") != std::string::npos ||
         upper.find("LIMIT") != std::string::npos;
}

static void runBatchMode(CypherQueryExecutor &executor,
                         const std::string &filename, const Options &config,
                         const Output &output) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    errs() << "Error: Could not open file " << filename << "\n";
    return;
  }

  std::string pending;
  auto flush = [&]() {
    if (!pending.empty()) {
      executeQuery(executor, pending, config, output);
      pending.clear();
    }
  };

  std::string line;
  while (std::getline(file, line)) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      flush();
      continue;
    }
    // A new MATCH after a completed query starts a new statement. UNION keeps
    // the current statement open for the following MATCH.
    if (!pending.empty() && startsWithClause(trimmed, "MATCH") &&
        queryIsComplete(pending) && !endsWithUnion(pending))
      flush();
    if (!pending.empty())
      pending += ' ';
    pending += trimmed;
    if (trimmed.back() == ';')
      flush();
  }
  flush();
}

static bool selectNodesWithCypher(CypherQueryExecutor &executor,
                                  const std::string &query_string,
                                  std::set<Node *> &nodes) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse(query_string);
  if (!query) {
    errs() << "Parse error: " << parser.getLastError().message << "\n";
    return false;
  }
  std::unique_ptr<CypherResult> result = executor.execute(*query);
  if (!result) {
    errs() << "Query error: " << executor.getLastError() << "\n";
    return false;
  }
  nodes.insert(result->getNodes().begin(), result->getNodes().end());
  return true;
}

static PDGQueryOptions buildAnalysisOptions(const Module &module,
                                            CypherQueryExecutor &executor,
                                            const Options &config) {
  PDGQueryOptions options;
  options.edge_preset = parseEdgePreset(config);
  options.context_mode = config.contextSensitive
                             ? PDGContextMode::ContextSensitive
                             : PDGContextMode::ContextInsensitive;
  options.slice_flavor = config.thin ? SliceFlavor::Thin : SliceFlavor::Full;
  options.explain = true;
  if (!config.scopeFunction.empty()) {
    if (Function *function = module.getFunction(config.scopeFunction))
      options.scope = PDGQueryScope::functionScope(*function);
  } else if (!config.scopeQuery.empty()) {
    std::set<Node *> nodes;
    if (selectNodesWithCypher(executor, config.scopeQuery, nodes))
      options.scope = PDGQueryScope::nodeSet(nodes);
  } else {
    options.scope = PDGQueryScope::wholeGraph();
  }
  return options;
}

static bool executeAnalysis(ProgramGraph &pdg, const Module &module,
                            CypherQueryExecutor &executor,
                            const Options &config, const Output &output) {
  std::string analysis = config.analysisName;
  PDGQueryOptions options = buildAnalysisOptions(module, executor, config);
  PDGCriteria criteria;
  PDGCriteria targets;
  PDGCriteria baseline;

  if (!config.criteriaQuery.empty())
    criteria.cypher_selections.push_back(
        CypherSelection{config.criteriaQuery, ""});
  if (!config.targetQuery.empty())
    targets.cypher_selections.push_back(
        CypherSelection{config.targetQuery, ""});
  if (!config.baselineQuery.empty())
    baseline.cypher_selections.push_back(
        CypherSelection{config.baselineQuery, ""});

  if (!config.propertyFile.empty()) {
    PropertySpec spec;
    std::string error;
    if (!PropertySpec::parseFromFile(config.propertyFile, spec, error)) {
      errs() << "error: " << error << "\n";
      return false;
    }
    criteria.property_specs.push_back(spec);
    if (analysis.empty())
      analysis = config.sliceDirection == "forward" ? "slice-forward"
                                                    : "slice-backward";
  }

  if (analysis.empty()) {
    errs() << "No mode specified. Use -q, -i, -f, or --analysis\n";
    return false;
  }

  if (criteria.empty() &&
      (analysis != "live" && analysis != "dead" &&
       analysis != "resource-flow" && analysis != "rules" &&
       !(analysis == "summary" &&
         options.scope.kind == PDGQueryScope::Kind::Function))) {
    errs() << "Analysis requires criteria. Use --criteria-query or "
              "--property-file\n";
    return false;
  }

  SliceQuery slice_query(pdg);
  DependenceQuery dependence_query(pdg);
  DataFlowQuery dataflow_query(pdg);
  DiffQuery diff_query(pdg);
  SummaryQuery summary_query(pdg);
  ImpactQuery impact_query(pdg);
  ResourceFlowQuery resource_query(pdg);

  if (analysis == "rules") {
    if (config.format != "json" && config.format != "text") {
      errs() << "rules supports --format text|json\n";
      return false;
    }
    try {
      std::vector<std::string> ids;
      if (!selectedRuleIds(ids, config))
        return false;
      TaintPolicy taint_policy;
      taint_policy.max_steps = config.taintStepLimit;
      RuleQueryPolicy rule_policy;
      rule_policy.lifetime_states_per_object = config.lifetimeStateLimit;
      output.printRuleResult(RuleQuery(pdg).analyze(
          ids, criteria, options, &module, taint_policy, rule_policy));
      return true;
    } catch (const std::invalid_argument &error) {
      errs() << error.what() << "\n";
      return false;
    }
  }

  if (analysis == "slice-forward") {
    PDGQueryResult result = slice_query.forward(criteria, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "slice-backward") {
    PDGQueryResult result = slice_query.backward(criteria, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "chop") {
    if (targets.empty()) {
      errs() << "chop requires --target-query\n";
      return false;
    }
    PDGQueryResult result =
        slice_query.chop(criteria, targets, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "shortest-path") {
    if (targets.empty()) {
      errs() << "shortest-path requires --target-query\n";
      return false;
    }
    PDGQueryResult result =
        dependence_query.shortestPath(criteria, targets, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "reaching-defs") {
    PDGQueryResult result =
        dataflow_query.reachingDefinitions(criteria, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "control-region") {
    PDGQueryResult result =
        dataflow_query.controlRegion(criteria, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "controllers") {
    PDGQueryResult result =
        dataflow_query.allControllers(criteria, options, &module);
    output.printResult(result);
    return true;
  }

  if (analysis == "live") {
    PDGQueryResult result = dataflow_query.liveNodes(options);
    output.printResult(result);
    return true;
  }

  if (analysis == "dead") {
    PDGQueryResult result = dataflow_query.deadNodes(options);
    output.printResult(result);
    return true;
  }

  if (analysis == "diff") {
    if (targets.empty()) {
      errs() << "diff requires --target-query\n";
      return false;
    }
    PDGQueryResult before = slice_query.forward(criteria, options, &module);
    PDGQueryResult after = slice_query.forward(targets, options, &module);
    DiffQueryResult result = diff_query.diff(before, after, options);
    output.printDiff(result);
    return true;
  }

  if (analysis == "summary") {
    SummaryPolicy policy;
    policy.kind = parseSummaryKind(config);
    SummaryQueryResult result =
        summary_query.summarize(criteria, policy, options, &module);
    output.printSummary(result);
    return true;
  }

  if (analysis == "impact") {
    ImpactPolicy policy;
    policy.changed_only = !baseline.empty();
    ImpactQueryResult result =
        baseline.empty()
            ? impact_query.analyze(criteria, policy, options, &module)
            : impact_query.analyzeAgainstBaseline(criteria, baseline, policy,
                                                  options, &module);
    output.printImpact(result);
    return true;
  }

  if (analysis == "resource-flow") {
    ResourcePolicy policy;
    policy.resource_kind = parseResourceKind(config);
    ResourceFlowQueryResult result =
        resource_query.analyze(criteria, policy, options, &module);
    output.printResourceFlow(result);
    return true;
  }

  errs() << "Unsupported analysis: " << analysis << "\n";
  return false;
}

} // namespace

Optional<int> handleCatalogOptions(const Options &config) {
  Output output(config);
  if (config.showVersion) {
    output.printVersion();
    return 0;
  }
  if (config.schema) {
    output.printSchema();
    return 0;
  }
  if (config.listRules || config.listCwes) {
    std::vector<std::string> ids;
    if (!selectedRuleIds(ids, config))
      return 1;
    std::vector<RuleDescriptor> rules;
    for (const auto &rule : RuleQuery::catalog())
      if (ids.empty() ||
          std::find(ids.begin(), ids.end(), rule.id) != ids.end())
        rules.push_back(rule);
    if (config.listRules)
      output.printRules(rules);
    else
      output.printCweCatalog(rules);
    return 0;
  }
  if (!config.ruleIds.empty() && config.analysisName != "rules") {
    errs() << "--rule requires --analysis rules\n";
    return 1;
  }
  if (!config.cweIds.empty() && config.analysisName != "rules") {
    errs() << "--cwe requires --analysis rules, --list-rules, or --list-cwes\n";
    return 1;
  }
  return None;
}

int runQueries(ProgramGraph &pdg, const Module &module, const Options &config) {
  Output output(config);
  if (config.verbose)
    output.printPDGInfo(pdg);
  CypherQueryExecutor executor(pdg);
  executor.setUnboundedMaxHops(config.unboundedMaxHops);
  if (!config.analysisName.empty() || !config.propertyFile.empty())
    return executeAnalysis(pdg, module, executor, config, output) ? 0 : 1;
  if (config.interactive) {
    runInteractiveMode(executor, config, output);
    return 0;
  }
  if (!config.queryString.empty())
    return executeQuery(executor, config.queryString, config, output) ? 0 : 1;
  if (!config.queryFile.empty()) {
    runBatchMode(executor, config.queryFile, config, output);
    return 0;
  }
  errs() << "No mode specified. Use -q, -i, -f, or --analysis\n";
  return 1;
}

} // namespace lotus::pdg_query
