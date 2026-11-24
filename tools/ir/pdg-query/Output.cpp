#include "Output.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Query/DiffQuery.h"
#include "IR/PDG/Query/ImpactQuery.h"
#include "IR/PDG/Query/QueryTypes.h"
#include "IR/PDG/Query/ResourceFlowQuery.h"
#include "IR/PDG/Query/SummaryQuery.h"
#include "IR/PDG/QueryLanguage/CypherAST.h"
#include "IR/PDG/QueryLanguage/CypherExecutor.h"
#include "IR/PDG/QueryLanguage/CypherResult.h"
#include "IR/PDG/Analysis/RuleQuery.h"
#include "IR/PDG/Support/PDGUtils.h"
#include "Options.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

using namespace llvm;
using namespace pdg;

namespace lotus::pdg_query {
namespace {

static std::string describeEdge(CypherQueryExecutor &executor, Edge *edge) {
  if (!edge)
    return "<null>";
  std::string value;
  value += executor.getEdgePropertyString(edge, "label");
  return value;
}

static std::string jsonEscape(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    switch (input[i]) {
    case '\\':
      output += "\\\\";
      break;
    case '"':
      output += "\\\"";
      break;
    case '\n':
      output += "\\n";
      break;
    default:
      output += input[i];
      break;
    }
  }
  return output;
}

static json::Array ruleCatalogJson(const std::vector<RuleDescriptor> &rules) {
  json::Array catalog;
  for (const auto &rule : rules) {
    json::Array cwes;
    for (unsigned cwe : rule.cwes)
      cwes.push_back(static_cast<int64_t>(cwe));
    catalog.push_back(json::Object{{"id", rule.id},
                                   {"severity", rule.severity},
                                   {"codeql_query", rule.codeql_query},
                                   {"coverage", rule.coverage},
                                   {"cwes", std::move(cwes)}});
  }
  return catalog;
}

static json::Object ruleSiteJson(Node *node) {
  json::Object site;
  const auto *inst =
      dyn_cast_or_null<Instruction>(node ? node->getValue() : nullptr);
  if (!inst)
    return site;
  site["function"] = inst->getFunction()->getName().str();
  site["ir"] = describeNode(node);
  if (const DebugLoc &location = inst->getDebugLoc()) {
    site["file"] = location->getFilename().str();
    site["directory"] = location->getDirectory().str();
    site["line"] = static_cast<int64_t>(location.getLine());
    site["column"] = static_cast<int64_t>(location.getCol());
  }
  return site;
}

} // namespace

Output::Output(const Options &config) : config_(config) {}

void Output::printVersion() const {
  outs() << "PDG Query Tool v2.0\n";
  outs() << "Part of the Lotus Program Analysis Framework\n";
}

void Output::printPDGInfo(ProgramGraph &pdg) const {
  raw_ostream &output = config_.format == "json" ? errs() : outs();
  output << "PDG Information:\n";
  output << "  Total nodes: " << pdg.numNode() << "\n";
  output << "  Total edges: " << pdg.numEdge() << "\n";
  output << "  Functions: " << pdg.getFuncWrapperMap().size() << "\n";
}

void Output::printSchema() const {
  // Complete node label list (matching CypherExecutor::matchNodes labelMap)
  static const char *nodeLabels[] = {"INST_FUNCALL",
                                     "INST_RET",
                                     "INST_BR",
                                     "INST_OTHER",
                                     "FUNC_ENTRY",
                                     "PARAM_FORMALIN",
                                     "PARAM_FORMALOUT",
                                     "PARAM_ACTUALIN",
                                     "PARAM_ACTUALOUT",
                                     "VAR_STATICALLOCGLOBALSCOPE",
                                     "VAR_STATICALLOCMODULESCOPE",
                                     "VAR_STATICALLOCFUNCTIONSCOPE",
                                     "VAR_OTHER",
                                     "FUNC",
                                     "CLASS",
                                     "ANNO_VAR",
                                     "ANNO_GLOBAL",
                                     "ANNO_OTHER"};

  // Complete edge type list (matching CypherExecutor)
  static const char *edgeTypes[] = {"IND_CALL",
                                    "CONTROLDEP_CALLINV",
                                    "CONTROLDEP_CALLRET",
                                    "CONTROLDEP_ENTRY",
                                    "CONTROLDEP_BR",
                                    "CONTROLDEP_IND_BR",
                                    "DATA_DEF_USE",
                                    "DATA_RAW",
                                    "DATA_READ",
                                    "DATA_ALIAS",
                                    "DATA_RET",
                                    "PARAMETER_IN",
                                    "PARAMETER_OUT",
                                    "PARAMETER_FIELD",
                                    "GLOBAL_DEP",
                                    "VAL_DEP",
                                    "CLS_MTH",
                                    "ANNO_VAR",
                                    "ANNO_GLOBAL",
                                    "ANNO_OTHER",
                                    "TYPE_OTHEREDGE"};

  // Edge type aliases accepted in Cypher queries
  static const char *edgeAliases[] = {"CALL_INV", "CALL_RET", "PARAM_IN",
                                      "PARAM_OUT"};

  // Node properties (all node types share the same accessible properties)
  static const char *nodeProperties[] = {"type",
                                         "type_id",
                                         "node_type",
                                         "node_type_id",
                                         "label",
                                         "kind",
                                         "func",
                                         "function",
                                         "name",
                                         "opcode",
                                         "callee",
                                         "src_file",
                                         "source_file",
                                         "src_line",
                                         "source_line",
                                         "src_col",
                                         "source_col",
                                         "source_column",
                                         "src",
                                         "source",
                                         "di_type",
                                         "dtype",
                                         "type_name",
                                         "llvm",
                                         "ir",
                                         "arg_count",
                                         "callee_param_count",
                                         "arg<N>_int",
                                         "arg<N>_object_bytes",
                                         "allocation_kind",
                                         "release_kind",
                                         "copy_destination_arg",
                                         "copy_source_arg",
                                         "copy_size_arg",
                                         "copy_destination_bytes",
                                         "copy_size_bytes",
                                         "known_access_count",
                                         "out_of_bounds_access_count",
                                         "read_out_of_bounds",
                                         "write_out_of_bounds",
                                         "access_bytes",
                                         "access_capacity_bytes",
                                         "access_offset_bytes",
                                         "pointer_nullness",
                                         "pointee_initialization",
                                         "format_arg",
                                         "format_kind",
                                         "format_given",
                                         "format_expected",
                                         "format_unbounded_string",
                                         "taint_source_kind",
                                         "taint_source_outputs",
                                         "taint_process_args",
                                         "taint_command_args",
                                         "taint_sql_args",
                                         "taint_path_args",
                                         "taint_format_args",
                                         "taint_allocation_args",
                                         "taint_wordexp_args",
                                         "arg<N>_string_termination",
                                         "arg<N>_string_min_bytes",
                                         "arg<N>_string_max_bytes",
                                         "format_output_max_bytes",
                                         "format_output_small_float_max_bytes",
                                         "format_output_value_flow",
                                         "format_output_has_float",
                                         "format_output_is_buffer",
                                         "format_output_limit_arg"};

  // Edge properties
  static const char *edgeProperties[] = {
      "type", "type_id", "edge_type", "edge_type_id", "label",
      "kind", "src",     "dst",       "src_<p>",      "dst_<p>"};

  outs() << "{\n";

  // --- Node labels ---
  outs() << "  \"node_labels\": [\n";
  for (size_t i = 0; i < sizeof(nodeLabels) / sizeof(nodeLabels[0]); ++i) {
    if (i)
      outs() << ",\n";
    outs() << "    \"" << nodeLabels[i] << "\"";
  }
  outs() << "\n  ],\n";

  // --- Node group labels ---
  outs() << "  \"node_group_labels\": {\n";
  outs() << "    \"INST\": [\"INST_FUNCALL\",\"INST_RET\",\"INST_BR\","
            "\"INST_OTHER\"],\n";
  outs() << "    \"VAR\": [\"VAR_STATICALLOCGLOBALSCOPE\","
            "\"VAR_STATICALLOCMODULESCOPE\","
            "\"VAR_STATICALLOCFUNCTIONSCOPE\",\"VAR_OTHER\"],\n";
  outs() << "    \"PARAM\": [\"PARAM_FORMALIN\",\"PARAM_FORMALOUT\","
            "\"PARAM_ACTUALIN\",\"PARAM_ACTUALOUT\"],\n";
  outs() << "    \"ANNO\": [\"ANNO_VAR\",\"ANNO_GLOBAL\",\"ANNO_OTHER\"]\n";
  outs() << "  },\n";

  // --- Node properties ---
  outs() << "  \"node_properties\": [\n";
  for (size_t i = 0; i < sizeof(nodeProperties) / sizeof(nodeProperties[0]);
       ++i) {
    if (i)
      outs() << ",\n";
    outs() << "    \"" << nodeProperties[i] << "\"";
  }
  outs() << "\n  ],\n";

  // --- Edge types ---
  outs() << "  \"edge_types\": [\n";
  for (size_t i = 0; i < sizeof(edgeTypes) / sizeof(edgeTypes[0]); ++i) {
    if (i)
      outs() << ",\n";
    outs() << "    \"" << edgeTypes[i] << "\"";
  }
  outs() << "\n  ],\n";

  // --- Edge type aliases ---
  outs() << "  \"edge_type_aliases\": [\n";
  for (size_t i = 0; i < sizeof(edgeAliases) / sizeof(edgeAliases[0]); ++i) {
    if (i)
      outs() << ",\n";
    outs() << "    \"" << edgeAliases[i] << "\"";
  }
  outs() << "\n  ],\n";

  // --- Edge group labels ---
  outs() << "  \"edge_group_labels\": {\n";
  outs() << "    \"CONTROL_DEP\": [\"CONTROLDEP_ENTRY\",\"CONTROLDEP_BR\","
            "\"CONTROLDEP_IND_BR\",\"CONTROLDEP_CALLINV\","
            "\"CONTROLDEP_CALLRET\"],\n";
  outs() << "    \"CALL\": [\"CONTROLDEP_CALLINV\",\"CONTROLDEP_CALLRET\","
            "\"IND_CALL\"],\n";
  outs() << "    \"DATA_DEP\": [\"DATA_DEF_USE\"]\n";
  outs() << "  },\n";

  // --- Edge properties ---
  outs() << "  \"edge_properties\": [\n";
  for (size_t i = 0; i < sizeof(edgeProperties) / sizeof(edgeProperties[0]);
       ++i) {
    if (i)
      outs() << ",\n";
    outs() << "    \"" << edgeProperties[i] << "\"";
  }
  outs() << "\n  ],\n";

  // --- Edge presets ---
  outs() << "  \"edge_presets\": {\n";
  static const char *presetNames[] = {"all",
                                      "data",
                                      "control",
                                      "parameter",
                                      "interprocedural",
                                      "value-flow",
                                      "transform-legality"};
  static const PDGEdgePreset presetValues[] = {
      PDGEdgePreset::All,
      PDGEdgePreset::Data,
      PDGEdgePreset::Control,
      PDGEdgePreset::Parameter,
      PDGEdgePreset::Interprocedural,
      PDGEdgePreset::ValueFlow,
      PDGEdgePreset::TransformLegality};

  // Local edge-type-to-name mapping (complete, unlike pdgutils::getEdgeTypeStr
  // which is missing CONTROLDEP_CALLRET and CLS_MTH).
  auto edgeTypeName = [](EdgeType et) -> const char * {
    switch (et) {
    case EdgeType::IND_CALL:
      return "IND_CALL";
    case EdgeType::CONTROLDEP_CALLINV:
      return "CONTROLDEP_CALLINV";
    case EdgeType::CONTROLDEP_CALLRET:
      return "CONTROLDEP_CALLRET";
    case EdgeType::CONTROLDEP_ENTRY:
      return "CONTROLDEP_ENTRY";
    case EdgeType::CONTROLDEP_BR:
      return "CONTROLDEP_BR";
    case EdgeType::CONTROLDEP_IND_BR:
      return "CONTROLDEP_IND_BR";
    case EdgeType::DATA_DEF_USE:
      return "DATA_DEF_USE";
    case EdgeType::DATA_RAW:
      return "DATA_RAW";
    case EdgeType::DATA_READ:
      return "DATA_READ";
    case EdgeType::DATA_ALIAS:
      return "DATA_ALIAS";
    case EdgeType::DATA_RET:
      return "DATA_RET";
    case EdgeType::PARAMETER_IN:
      return "PARAMETER_IN";
    case EdgeType::PARAMETER_OUT:
      return "PARAMETER_OUT";
    case EdgeType::PARAMETER_FIELD:
      return "PARAMETER_FIELD";
    case EdgeType::GLOBAL_DEP:
      return "GLOBAL_DEP";
    case EdgeType::VAL_DEP:
      return "VAL_DEP";
    case EdgeType::CLS_MTH:
      return "CLS_MTH";
    case EdgeType::ANNO_VAR:
      return "ANNO_VAR";
    case EdgeType::ANNO_GLOBAL:
      return "ANNO_GLOBAL";
    case EdgeType::ANNO_OTHER:
      return "ANNO_OTHER";
    case EdgeType::TYPE_OTHEREDGE:
      return "TYPE_OTHEREDGE";
    }
    return "<unknown>";
  };

  for (size_t p = 0; p < sizeof(presetNames) / sizeof(presetNames[0]); ++p) {
    if (p)
      outs() << ",\n";
    outs() << "    \"" << presetNames[p] << "\": [";
    auto edgeSet = edgeTypesForPreset(presetValues[p]);
    size_t ei = 0;
    for (auto et : edgeSet) {
      if (ei++)
        outs() << ", ";
      outs() << "\"" << jsonEscape(edgeTypeName(et)) << "\"";
    }
    outs() << "]";
  }
  outs() << "\n  }\n";

  outs() << "}\n";
}

void Output::printRuleResult(const RuleQueryResult &result) const {
  if (config_.format == "json") {
    json::Array findings;
    for (const auto &finding : result.findings) {
      json::Array evidence;
      for (Node *node : finding.evidence)
        evidence.push_back(ruleSiteJson(node));
      json::Array taint_origins;
      auto valueSite = [](const Value *value) {
        json::Object result;
        if (!value)
          return result;
        std::string ir;
        raw_string_ostream stream(ir);
        value->print(stream);
        stream.flush();
        result["ir"] = ir;
        if (const auto *argument = dyn_cast<Argument>(value)) {
          result["function"] = argument->getParent()->getName().str();
          result["argument"] = static_cast<int64_t>(argument->getArgNo());
        }
        if (const auto *inst = dyn_cast<Instruction>(value)) {
          result["function"] = inst->getFunction()->getName().str();
          if (const DebugLoc &loc = inst->getDebugLoc()) {
            result["file"] = loc->getFilename().str();
            result["line"] = static_cast<int64_t>(loc.getLine());
            result["column"] = static_cast<int64_t>(loc.getCol());
          }
        }
        return result;
      };
      for (const auto &origin : finding.taint_origins)
        taint_origins.push_back(json::Object{
            {"source", valueSite(origin.source)},
            {"concatenation", valueSite(origin.concatenation)},
            {"nonconstant_only", origin.nonconstant_only},
            {"sql_sanitized", origin.sql_sanitized},
            {"allocation_bounded", origin.allocation_bounded},
            {"sink_argument", static_cast<int64_t>(origin.sink_argument)}});
      findings.push_back(
          json::Object{{"rule_id", finding.rule_id},
                       {"message", finding.message},
                       {"site", ruleSiteJson(finding.site)},
                       {"evidence", std::move(evidence)},
                       {"taint_origins", std::move(taint_origins)}});
    }
    json::Array notes;
    for (const auto &note : result.diagnostics.notes)
      notes.push_back(note);
    json::Array unresolved;
    for (const auto &criterion : result.diagnostics.unresolved_criteria)
      unresolved.push_back(criterion);
    outs() << formatv(
        "{0:2}\n",
        json::Value(json::Object{
            {"rules", ruleCatalogJson(result.rules)},
            {"findings", std::move(findings)},
            {"explored_states",
             static_cast<int64_t>(result.diagnostics.explored_states)},
            {"state_limit_hit", result.diagnostics.state_limit_hit},
            {"notes", std::move(notes)},
            {"unresolved_criteria", std::move(unresolved)}}));
    return;
  }
  outs() << "Semantic rule findings: " << result.findings.size() << "\n";
  for (const auto &finding : result.findings)
    outs() << finding.rule_id << ": " << finding.message << "\n  "
           << stableNodeKey(finding.site) << "\n";
  for (const auto &rule : result.rules)
    outs() << rule.id << " coverage: " << rule.coverage << "\n";
  if (result.diagnostics.state_limit_hit)
    errs() << "Analysis incomplete: a state budget or convergence limit was "
              "reached.\n";
  for (const auto &note : result.diagnostics.notes)
    if (note.find("incomplete") != std::string::npos)
      errs() << note << "\n";
}

void Output::printResultText(const PDGQueryResult &result) const {
  outs() << "criteria nodes: " << result.criteria_nodes.size() << "\n";
  outs() << "result nodes: " << result.nodes.size() << "\n";
  outs() << "result edges: " << result.edges.size() << "\n";
  if (!result.diagnostics.unresolved_criteria.empty()) {
    outs() << "unresolved criteria:\n";
    for (size_t i = 0; i < result.diagnostics.unresolved_criteria.size(); ++i)
      outs() << "  - " << result.diagnostics.unresolved_criteria[i] << "\n";
  }
  if (!result.witness_paths.empty()) {
    outs() << "witness paths:\n";
    for (size_t i = 0; i < result.witness_paths.size(); ++i) {
      outs() << "  path " << i << ":";
      for (size_t j = 0; j < result.witness_paths[i].nodes.size(); ++j)
        outs() << " " << stableNodeKey(result.witness_paths[i].nodes[j]);
      outs() << "\n";
    }
  }
  if (config_.dumpSlice) {
    for (PDGQueryResult::NodeSet::const_iterator it = result.nodes.begin();
         it != result.nodes.end(); ++it)
      outs() << "  " << describeNode(*it) << "\n";
  }
}

void Output::printResultJson(const PDGQueryResult &result) const {
  outs() << "{";
  outs() << "\"criteria_nodes\":" << result.criteria_nodes.size() << ",";
  outs() << "\"nodes\":[";
  bool first = true;
  for (PDGQueryResult::NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(stableNodeKey(*it)) << "\"";
  }
  outs() << "],";
  outs() << "\"witness_paths\":[";
  for (size_t i = 0; i < result.witness_paths.size(); ++i) {
    if (i != 0)
      outs() << ",";
    outs() << "[";
    for (size_t j = 0; j < result.witness_paths[i].nodes.size(); ++j) {
      if (j != 0)
        outs() << ",";
      outs() << "\""
             << jsonEscape(stableNodeKey(result.witness_paths[i].nodes[j]))
             << "\"";
    }
    outs() << "]";
  }
  outs() << "]";
  outs() << "}\n";
}

void Output::printResultDot(const PDGQueryResult &result) const {
  outs() << "digraph PDGQuery {\n";
  for (PDGQueryResult::NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    outs() << "  \"" << stableNodeKey(*it) << "\";\n";
  }
  for (PDGQueryResult::EdgeSet::const_iterator it = result.edges.begin();
       it != result.edges.end(); ++it) {
    Edge *edge = *it;
    outs() << "  \"" << stableNodeKey(edge->getSrcNode()) << "\" -> \""
           << stableNodeKey(edge->getDstNode()) << "\""
           << " [label=\"" << pdgutils::getEdgeTypeStr(edge->getEdgeType())
           << "\"];\n";
  }
  outs() << "}\n";
}

void Output::printDiff(const DiffQueryResult &result) const {
  if (config_.format == "json") {
    outs() << "{"
           << "\"added_nodes\":";
    size_t added = 0;
    size_t removed = 0;
    for (size_t i = 0; i < result.node_diffs.size(); ++i) {
      added += result.node_diffs[i].kind == DiffKind::Added;
      removed += result.node_diffs[i].kind == DiffKind::Removed;
    }
    outs() << added << ",\"removed_nodes\":" << removed << "}\n";
    return;
  }

  outs() << "node diffs: " << result.node_diffs.size() << "\n";
  outs() << "edge diffs: " << result.edge_diffs.size() << "\n";
  if (!result.impact_summary.functions.empty()) {
    outs() << "functions:\n";
    for (std::unordered_map<std::string, size_t>::const_iterator it =
             result.impact_summary.functions.begin();
         it != result.impact_summary.functions.end(); ++it)
      outs() << "  " << it->first << ": " << it->second << "\n";
  }
}

void Output::printSummaryText(const SummaryQueryResult &result) const {
  outs() << "function: "
         << (result.summary.function ? result.summary.function->getName()
                                     : "<none>")
         << "\n";
  outs() << "input_to_return: " << result.summary.input_to_return.size()
         << "\n";
  outs() << "input_to_global_write: "
         << result.summary.input_to_global_write.size() << "\n";
  outs() << "input_to_callsite: " << result.summary.input_to_callsite.size()
         << "\n";
  outs() << "global_readers: " << result.summary.global_readers.size() << "\n";
  outs() << "global_writers: " << result.summary.global_writers.size() << "\n";
  outs() << "control_predicates: " << result.summary.control_predicates.size()
         << "\n";
  outs() << "reachable_calls: " << result.summary.reachable_calls.size()
         << "\n";
  if (!result.summary.may_allocate_resource_kinds.empty()) {
    outs() << "may_allocate_resource_kinds:";
    for (std::set<ResourceKind>::const_iterator it =
             result.summary.may_allocate_resource_kinds.begin();
         it != result.summary.may_allocate_resource_kinds.end(); ++it)
      outs() << " " << resourceKindName(*it);
    outs() << "\n";
  }
  if (!result.summary.may_release_resource_kinds.empty()) {
    outs() << "may_release_resource_kinds:";
    for (std::set<ResourceKind>::const_iterator it =
             result.summary.may_release_resource_kinds.begin();
         it != result.summary.may_release_resource_kinds.end(); ++it)
      outs() << " " << resourceKindName(*it);
    outs() << "\n";
  }
}

void Output::printSummaryJson(const SummaryQueryResult &result) const {
  outs() << "{";
  outs() << "\"function\":\""
         << jsonEscape(result.summary.function
                           ? result.summary.function->getName().str()
                           : "")
         << "\",";
  outs() << "\"input_to_return\":" << result.summary.input_to_return.size()
         << ",";
  outs() << "\"input_to_global_write\":"
         << result.summary.input_to_global_write.size() << ",";
  outs() << "\"input_to_callsite\":" << result.summary.input_to_callsite.size()
         << ",";
  outs() << "\"global_readers\":" << result.summary.global_readers.size()
         << ",";
  outs() << "\"global_writers\":" << result.summary.global_writers.size()
         << ",";
  outs() << "\"control_predicates\":"
         << result.summary.control_predicates.size() << ",";
  outs() << "\"reachable_calls\":" << result.summary.reachable_calls.size()
         << ",";
  outs() << "\"may_allocate_resource_kinds\":[";
  bool first = true;
  for (std::set<ResourceKind>::const_iterator it =
           result.summary.may_allocate_resource_kinds.begin();
       it != result.summary.may_allocate_resource_kinds.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(resourceKindName(*it)) << "\"";
  }
  outs() << "],\"may_release_resource_kinds\":[";
  first = true;
  for (std::set<ResourceKind>::const_iterator it =
           result.summary.may_release_resource_kinds.begin();
       it != result.summary.may_release_resource_kinds.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(resourceKindName(*it)) << "\"";
  }
  outs() << "]}\n";
}

void Output::printImpactText(const ImpactQueryResult &result) const {
  outs() << "directly_impacted_nodes: "
         << result.directly_impacted_nodes.nodes.size() << "\n";
  outs() << "transitively_impacted_nodes: "
         << result.transitively_impacted_nodes.nodes.size() << "\n";
  outs() << "impacted_functions:";
  for (std::set<std::string>::const_iterator it =
           result.impacted_functions.begin();
       it != result.impacted_functions.end(); ++it)
    outs() << " " << *it;
  outs() << "\n";
  outs() << "impacted_source_locations: "
         << result.impacted_source_locations.size() << "\n";
  outs() << "boundary_crossings:";
  for (std::unordered_map<std::string, size_t>::const_iterator it =
           result.boundary_crossings.begin();
       it != result.boundary_crossings.end(); ++it)
    outs() << " " << it->first << "=" << it->second;
  outs() << "\n";
  outs() << "ranked_impacts:\n";
  for (size_t i = 0; i < result.ranked_impacts.size(); ++i) {
    outs() << "  " << result.ranked_impacts[i].stable_key
           << " dist=" << result.ranked_impacts[i].shortest_distance
           << " crossings="
           << result.ranked_impacts[i].interprocedural_crossings
           << " paths=" << result.ranked_impacts[i].path_count << "\n";
  }
}

void Output::printImpactJson(const ImpactQueryResult &result) const {
  outs() << "{";
  outs() << "\"directly_impacted_nodes\":"
         << result.directly_impacted_nodes.nodes.size() << ",";
  outs() << "\"transitively_impacted_nodes\":"
         << result.transitively_impacted_nodes.nodes.size() << ",";
  outs() << "\"impacted_functions\":[";
  bool first = true;
  for (std::set<std::string>::const_iterator it =
           result.impacted_functions.begin();
       it != result.impacted_functions.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(*it) << "\"";
  }
  outs() << "],\"boundary_crossings\":{";
  first = true;
  for (std::unordered_map<std::string, size_t>::const_iterator it =
           result.boundary_crossings.begin();
       it != result.boundary_crossings.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(it->first) << "\":" << it->second;
  }
  outs() << "},\"ranked_impacts\":[";
  for (size_t i = 0; i < result.ranked_impacts.size(); ++i) {
    if (i != 0)
      outs() << ",";
    outs() << "{\"node\":\"" << jsonEscape(result.ranked_impacts[i].stable_key)
           << "\","
           << "\"distance\":" << result.ranked_impacts[i].shortest_distance
           << ",\"crossings\":"
           << result.ranked_impacts[i].interprocedural_crossings
           << ",\"path_count\":" << result.ranked_impacts[i].path_count << "}";
  }
  outs() << "]}\n";
}

void Output::printResourceFlowText(
    const ResourceFlowQueryResult &result) const {
  outs() << "acquire_sites: " << result.acquire_sites.size() << "\n";
  outs() << "release_sites: " << result.release_sites.size() << "\n";
  outs() << "orphaned_resources: " << result.orphaned_resources.size() << "\n";
  outs() << "double_release_candidates: "
         << result.double_release_candidates.size() << "\n";
  outs() << "resource_kind_counts:";
  for (std::map<ResourceKind, size_t>::const_iterator it =
           result.resource_kind_counts.begin();
       it != result.resource_kind_counts.end(); ++it)
    outs() << " " << resourceKindName(it->first) << "=" << it->second;
  outs() << "\n";
}

void Output::printResourceFlowJson(
    const ResourceFlowQueryResult &result) const {
  outs() << "{";
  outs() << "\"acquire_sites\":" << result.acquire_sites.size() << ",";
  outs() << "\"release_sites\":" << result.release_sites.size() << ",";
  outs() << "\"orphaned_resources\":" << result.orphaned_resources.size()
         << ",";
  outs() << "\"double_release_candidates\":"
         << result.double_release_candidates.size() << ",";
  outs() << "\"resource_kind_counts\":{";
  bool first = true;
  for (std::map<ResourceKind, size_t>::const_iterator it =
           result.resource_kind_counts.begin();
       it != result.resource_kind_counts.end(); ++it) {
    if (!first)
      outs() << ",";
    first = false;
    outs() << "\"" << jsonEscape(resourceKindName(it->first))
           << "\":" << it->second;
  }
  outs() << "}}\n";
}

void Output::printRules(const std::vector<RuleDescriptor> &rules) const {
  if (config_.format == "json")
    outs() << formatv("{0:2}\n", json::Value(ruleCatalogJson(rules)));
  else
    for (const auto &rule : rules)
      outs() << rule.id << " [" << rule.severity << "] " << rule.coverage
             << "\n";
}

void Output::printCweCatalog(
    const std::vector<RuleDescriptor> &selectedRules) const {
  std::map<unsigned, std::vector<std::string>> mappings;
  for (const auto &rule : selectedRules)
    for (unsigned cwe : rule.cwes)
      mappings[cwe].push_back(rule.id);
  json::Array rows;
  for (const auto &mapping : mappings) {
    if (!config_.cweIds.empty() &&
        std::find(config_.cweIds.begin(), config_.cweIds.end(),
                  mapping.first) == config_.cweIds.end())
      continue;
    if (config_.format == "json") {
      json::Array ids;
      for (const auto &id : mapping.second)
        ids.push_back(id);
      rows.push_back(
          json::Object{{"cwe", static_cast<int64_t>(mapping.first)},
                       {"implemented_rule_ids", std::move(ids)},
                       {"implemented_rule_count",
                        static_cast<int64_t>(mapping.second.size())}});
    } else {
      outs() << "CWE-" << mapping.first << ":";
      for (const auto &id : mapping.second)
        outs() << " " << id;
      outs() << "\n";
    }
  }
  if (config_.format == "json")
    outs() << formatv("{0:2}\n", json::Value(std::move(rows)));
}

void Output::printResult(const PDGQueryResult &result) const {
  if (config_.format == "json")
    printResultJson(result);
  else if (config_.format == "dot")
    printResultDot(result);
  else
    printResultText(result);
}

void Output::printSummary(const SummaryQueryResult &result) const {
  if (config_.format == "json")
    printSummaryJson(result);
  else
    printSummaryText(result);
}

void Output::printImpact(const ImpactQueryResult &result) const {
  if (config_.format == "json")
    printImpactJson(result);
  else
    printImpactText(result);
}

void Output::printResourceFlow(const ResourceFlowQueryResult &result) const {
  if (config_.format == "json")
    printResourceFlowJson(result);
  else
    printResourceFlowText(result);
}

void Output::printQueryResult(CypherQueryExecutor &executor,
                              const CypherResult &result) const {
  outs() << "Result: " << result.toString() << "\n";
  if (result.getType() == CypherResult::ResultType::NODES) {
    size_t limit = config_.resultLimit > 0
                       ? std::min(result.getNodes().size(),
                                  static_cast<size_t>(config_.resultLimit))
                       : result.getNodes().size();
    for (size_t i = 0; i < limit; ++i)
      outs() << "  " << describeNode(result.getNodes()[i]) << "\n";
  } else if (result.getType() == CypherResult::ResultType::RELATIONSHIPS) {
    size_t limit = config_.resultLimit > 0
                       ? std::min(result.getRelationships().size(),
                                  static_cast<size_t>(config_.resultLimit))
                       : result.getRelationships().size();
    for (size_t i = 0; i < limit; ++i)
      outs() << "  " << describeEdge(executor, result.getRelationships()[i])
             << "\n";
  }
}

void Output::printQueryPlan(const CypherQuery &query) const {
  outs() << "Plan: " << query.getPatterns().size() << " patterns, "
         << query.getReturnItems().size() << " returns";
  if (query.hasWhere())
    outs() << ", WHERE";
  if (query.hasLimit())
    outs() << ", LIMIT " << query.getLimit();
  outs() << "\n";
}

} // namespace lotus::pdg_query
