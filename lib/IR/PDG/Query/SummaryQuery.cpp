#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGUtils.h"
/**
 * @file SummaryQuery.cpp
 * @brief Implementation of function-scoped PDG summaries.
 *
 * This file turns a function-local PDG slice into compact summary buckets such
 * as input-to-return, input-to-callsite, and resource-kind summaries. The
 * implementation stays intentionally lightweight and reuses the shared query
 * services from the core PDG query layer instead of introducing a separate
 * summary engine.
 */

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Query/DependenceQuery.h"
#include "IR/PDG/Query/SliceQuery.h"
#include "IR/PDG/Query/SummaryQuery.h"
#include "QuerySupport.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace pdg {

namespace {

using NodeSet = PDGQueryResult::NodeSet;

/// True when @p node represents a global storage location tracked in the PDG.
static bool isGlobalNode(Node *node) {
  if (node == nullptr)
    return false;
  const GraphNodeType type = node->getNodeType();
  if (node->getValue() == nullptr) {
    return type == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
           type == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
           type == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
  }
  return type == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
         type == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
         type == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE ||
         isa<GlobalValue>(node->getValue());
}

/// Best-effort owner function for a PDG node.
static const Function *functionForNode(Node *node) {
  if (node == nullptr)
    return nullptr;
  if (node->getFunc() != nullptr)
    return node->getFunc();
  const Value *value = node->getValue();
  if (const Argument *argument = dyn_cast_or_null<Argument>(value))
    return argument->getParent();
  return dyn_cast_or_null<Function>(value);
}

static bool isInputNode(Node *node, const Function &function) {
  if (node == nullptr)
    return false;
  if (node->getNodeType() == GraphNodeType::PARAM_FORMALIN ||
      node->getNodeType() == GraphNodeType::PARAM_ACTUALIN)
    return true;
  const Argument *argument = dyn_cast_or_null<Argument>(node->getValue());
  return argument != nullptr && argument->getParent() == &function;
}

static bool isReturnNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_RET;
}

static bool isCallNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_FUNCALL;
}

static bool isControlPredicateNode(Node *node) {
  return node != nullptr &&
         (node->getNodeType() == GraphNodeType::INST_BR ||
          node->getNodeType() == GraphNodeType::FUNC_ENTRY);
}

static void collectTreeNodes(Tree *tree, NodeSet &nodes) {
  if (!tree || !tree->getRootNode())
    return;
  std::queue<TreeNode *> q;
  q.push(tree->getRootNode());
  while (!q.empty()) {
    TreeNode *cur = q.front();
    q.pop();
    if (!cur)
      continue;
    nodes.insert(cur);
    for (TreeNode *child : cur->getChildNodes())
      if (child)
        q.push(child);
  }
}

/// Collect all nodes that belong to @p function plus optionally adjacent globals.
static NodeSet collectFunctionNodes(ProgramGraph &pdg, const Function &function,
                                    bool include_connected_globals) {
  NodeSet nodes;
  Function &mutable_func = const_cast<Function &>(function);
  if (FunctionWrapper *fw = pdg.getFuncWrapper(mutable_func)) {
    if (Node *entry = fw->getEntryNode())
      nodes.insert(entry);
    if (Node *fn_node = pdg.getNode(mutable_func))
      nodes.insert(fn_node);
    for (Argument &arg : mutable_func.args()) {
      if (Node *arg_node = pdg.getNode(arg))
        nodes.insert(arg_node);
      collectTreeNodes(fw->getArgFormalInTree(arg), nodes);
      collectTreeNodes(fw->getArgFormalOutTree(arg), nodes);
    }
    collectTreeNodes(fw->getRetFormalInTree(), nodes);
    collectTreeNodes(fw->getRetFormalOutTree(), nodes);
    for (Instruction &inst : instructions(mutable_func)) {
      if (Node *n = pdg.getNode(inst))
        nodes.insert(n);
    }
    for (CallBase *cb : fw->getCallInsts()) {
      if (CallWrapper *cw = pdg.getCallWrapper(*cb)) {
        for (Value *arg : cw->getArgList()) {
          if (arg) {
            collectTreeNodes(cw->getArgActualInTree(*arg), nodes);
            collectTreeNodes(cw->getArgActualOutTree(*arg), nodes);
          }
        }
        collectTreeNodes(cw->getRetActualInTree(), nodes);
        collectTreeNodes(cw->getRetActualOutTree(), nodes);
      }
    }
  } else {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end();
         ++it) {
      Node *node = *it;
      if (node != nullptr && functionForNode(node) == &function)
        nodes.insert(node);
    }
    if (pdg.hasNode(mutable_func))
      nodes.insert(pdg.getNode(mutable_func));
  }

  if (!include_connected_globals)
    return nodes;

  std::vector<Node *> connected_globals;
  for (Node *node : nodes) {
    if (!node)
      continue;
    for (Edge *edge : node->getInEdgeSet()) {
      Node *src = edge ? edge->getSrcNode() : nullptr;
      if (src && isGlobalNode(src))
        connected_globals.push_back(src);
    }
    for (Edge *edge : node->getOutEdgeSet()) {
      Node *dst = edge ? edge->getDstNode() : nullptr;
      if (dst && isGlobalNode(dst))
        connected_globals.push_back(dst);
    }
  }
  nodes.insert(connected_globals.begin(), connected_globals.end());
  return nodes;
}

static std::string functionSummaryCacheKey(const Function &function,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options) {
  std::ostringstream os;
  os << function.getName().str() << "|" << static_cast<int>(policy.kind) << "|"
     << policy.max_witnesses_per_bucket << "|"
     << static_cast<int>(options.edge_preset) << "|"
     << static_cast<int>(options.context_mode) << "|"
     << static_cast<int>(options.slice_flavor);
  return os.str();
}

/// Resolve summary criteria to exactly one function, emitting diagnostics on ambiguity.
static bool tryResolveSingleFunction(ProgramGraph &pdg,
                                     const PDGCriteria &criteria,
                                     const PDGQueryOptions &options,
                                     const Module *module,
                                     const Function *&function,
                                     PDGQueryDiagnostics &diagnostics) {
  function = nullptr;
  if (options.scope.kind == PDGQueryScope::Kind::Function &&
      options.scope.function != nullptr) {
    function = options.scope.function;
    return true;
  }
  if (criteria.function_names.size() == 1 && module != nullptr) {
    function = module->getFunction(criteria.function_names.front());
    if (function != nullptr)
      return true;
  }
  PDGCriteriaResolver resolver(pdg);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  std::set<const Function *> functions;
  for (NodeSet::const_iterator it = resolved.nodes.begin(); it != resolved.nodes.end();
       ++it) {
    const Function *candidate = functionForNode(*it);
    if (candidate != nullptr)
      functions.insert(candidate);
  }
  if (functions.size() == 1) {
    function = *functions.begin();
    return true;
  }
  if (functions.empty())
    diagnostics.unresolved_criteria.push_back(
        "summary query did not resolve to any function");
  else
    diagnostics.unresolved_criteria.push_back(
        "summary query resolved to multiple functions");
  return false;
}

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

static std::string calleeName(Node *node) {
  if (!isCallNode(node))
    return "";
  const CallBase *call = dyn_cast_or_null<CallBase>(node->getValue());
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return "";
  return call->getCalledFunction()->getName().str();
}

static ResourceKind resourceKindForAcquireName(const std::string &api_name) {
  const std::string lower = toLower(api_name);
  if (lower == "malloc" || lower == "calloc" || lower == "realloc")
    return ResourceKind::Heap;
  if (lower == "fopen")
    return ResourceKind::File;
  if (lower == "open" || lower == "socket")
    return ResourceKind::FileDescriptor;
  if (lower == "opendir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

static ResourceKind resourceKindForReleaseName(const std::string &api_name) {
  const std::string lower = toLower(api_name);
  if (lower == "free")
    return ResourceKind::Heap;
  if (lower == "fclose")
    return ResourceKind::File;
  if (lower == "close")
    return ResourceKind::FileDescriptor;
  if (lower == "closedir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

static ResourceKind resourceKindForNode(Node *node, bool release) {
  const std::string name = calleeName(node);
  return release ? resourceKindForReleaseName(name)
                 : resourceKindForAcquireName(name);
}

static bool summaryKindEnabled(SummaryKind selected, SummaryKind candidate) {
  return selected == SummaryKind::All || selected == candidate;
}

using PairSet = llvm::DenseSet<std::pair<Node *, Node *>>;

static void appendBucketEntry(std::vector<SummaryBucketEntry> &entries,
                              PairSet &seen, Node *source, Node *target,
                              std::vector<PDGWitnessPath> witnesses) {
  if (!seen.insert({source, target}).second)
    return;
  SummaryBucketEntry entry;
  entry.source = source;
  entry.target = target;
  entry.witness_paths = std::move(witnesses);
  entries.push_back(std::move(entry));
}

static std::vector<PDGWitnessPath> reconstructShortestPaths(
    Node *source, Node *target,
    const std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
        &preds,
    size_t max_paths) {
  std::vector<PDGWitnessPath> results;
  std::unordered_set<std::string> seen_paths;
  std::vector<Node *> path_nodes{target};
  std::vector<EdgeType> path_edges;
  std::function<void(Node *)> build = [&](Node *current) {
    if (max_paths > 0 && results.size() >= max_paths)
      return;
    if (current == source) {
      PDGWitnessPath witness;
      witness.kind = PDGWitnessPathKind::AllShortestPath;
      witness.nodes.assign(path_nodes.rbegin(), path_nodes.rend());
      witness.edge_types.assign(path_edges.rbegin(), path_edges.rend());
      const std::string key = query_detail::pathKey(witness);
      if (seen_paths.insert(key).second)
        results.push_back(std::move(witness));
      return;
    }
    auto it = preds.find(current);
    if (it == preds.end())
      return;
    for (const auto &pred : it->second) {
      path_nodes.push_back(pred.first);
      path_edges.push_back(pred.second);
      build(pred.first);
      path_edges.pop_back();
      path_nodes.pop_back();
      if (max_paths > 0 && results.size() >= max_paths)
        return;
    }
  };
  build(target);
  return results;
}

} // namespace

SummaryQueryResult SummaryQuery::summarize(const Function &function,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options,
                                           const Module *module) const {
  // Function summaries are stable until the PDG graph epoch changes.
  if (cache_epoch_ != pdg_.getEpoch()) {
    cache_epoch_ = pdg_.getEpoch();
    summary_cache_.clear();
  }
  const std::string cache_key =
      functionSummaryCacheKey(function, policy, options);
  std::unordered_map<std::string, SummaryQueryResult>::const_iterator cached =
      summary_cache_.find(cache_key);
  if (options.cache_policy == PDGCachePolicy::Enabled &&
      cached != summary_cache_.end()) {
    SummaryQueryResult result = cached->second;
    result.diagnostics.summary_cache_hits++;
    return result;
  }

  SummaryQueryResult result;
  result.summary.function = &function;
  const bool need_globals =
      summaryKindEnabled(policy.kind, SummaryKind::InputToGlobalWrite) ||
      summaryKindEnabled(policy.kind, SummaryKind::GlobalReaders) ||
      summaryKindEnabled(policy.kind, SummaryKind::GlobalWriters);
  const NodeSet function_nodes =
      collectFunctionNodes(pdg_, function, need_globals);

  std::vector<Node *> input_nodes;
  std::vector<Node *> return_nodes;
  std::vector<Node *> call_nodes;
  std::vector<Node *> control_nodes;
  std::vector<Node *> global_nodes;

  for (NodeSet::const_iterator it = function_nodes.begin();
       it != function_nodes.end(); ++it) {
    Node *node = *it;
    if (isInputNode(node, function))
      input_nodes.push_back(node);
    if (isReturnNode(node))
      return_nodes.push_back(node);
    if (isCallNode(node))
      call_nodes.push_back(node);
    if (isControlPredicateNode(node))
      control_nodes.push_back(node);
    if (isGlobalNode(node))
      global_nodes.push_back(node);
  }

  const bool need_input_return =
      summaryKindEnabled(policy.kind, SummaryKind::InputToReturn) &&
      !return_nodes.empty();
  const bool need_input_call =
      summaryKindEnabled(policy.kind, SummaryKind::InputToCallsite) &&
      !call_nodes.empty();
  const bool need_input_global =
      summaryKindEnabled(policy.kind, SummaryKind::InputToGlobalWrite) &&
      !global_nodes.empty();

  if ((need_input_return || need_input_call || need_input_global) &&
      !input_nodes.empty()) {
    const std::set<EdgeType> edge_types =
        edgeTypesForPreset(options.edge_preset);
    const size_t witness_cap =
        policy.max_witnesses_per_bucket > 0
            ? policy.max_witnesses_per_bucket
            : (options.limits.max_paths > 0 ? options.limits.max_paths : 0);
    PairSet seen_ret, seen_call, seen_global;

    for (Node *input : input_nodes) {
      std::queue<Node *> worklist;
      std::unordered_map<Node *, size_t> distances;
      std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
          preds;
      worklist.push(input);
      distances[input] = 0;

      while (!worklist.empty()) {
        Node *cur = worklist.front();
        worklist.pop();
        const size_t cur_dist = distances[cur];
        if (options.limits.max_depth > 0 &&
            cur_dist >= options.limits.max_depth)
          continue;
        for (Edge *edge : cur->getOutEdgeSet()) {
          if (!edge || !query_detail::isEdgeAllowed(edge->getEdgeType(), edge_types))
            continue;
          Node *nbr = edge->getDstNode();
          if (!nbr || function_nodes.count(nbr) == 0)
            continue;
          const size_t next_dist = cur_dist + 1;
          auto dit = distances.find(nbr);
          if (dit == distances.end()) {
            distances[nbr] = next_dist;
            preds[nbr].push_back({cur, edge->getEdgeType()});
            worklist.push(nbr);
          } else if (next_dist == dit->second) {
            preds[nbr].push_back({cur, edge->getEdgeType()});
          }
        }
      }

      if (need_input_return) {
        for (Node *ret : return_nodes) {
          if (distances.count(ret) == 0)
            continue;
          auto witnesses =
              reconstructShortestPaths(input, ret, preds, witness_cap);
          appendBucketEntry(result.summary.input_to_return, seen_ret, input,
                            ret, std::move(witnesses));
        }
      }
      if (need_input_call) {
        for (Node *call : call_nodes) {
          if (distances.count(call) == 0)
            continue;
          auto witnesses =
              reconstructShortestPaths(input, call, preds, witness_cap);
          appendBucketEntry(result.summary.input_to_callsite, seen_call, input,
                            call, std::move(witnesses));
        }
      }
      if (need_input_global) {
        for (Node *glob : global_nodes) {
          if (distances.count(glob) == 0)
            continue;
          auto witnesses =
              reconstructShortestPaths(input, glob, preds, witness_cap);
          appendBucketEntry(result.summary.input_to_global_write, seen_global,
                            input, glob, std::move(witnesses));
        }
      }
    }
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::GlobalReaders) ||
      summaryKindEnabled(policy.kind, SummaryKind::GlobalWriters)) {
    PairSet seen_readers, seen_writers;
    for (size_t i = 0; i < global_nodes.size(); ++i) {
      for (Node::EdgeSet::const_iterator edge_it =
               global_nodes[i]->getOutEdgeSet().begin();
           edge_it != global_nodes[i]->getOutEdgeSet().end(); ++edge_it) {
        Edge *edge = *edge_it;
        Node *reader = edge != nullptr ? edge->getDstNode() : nullptr;
        if (reader != nullptr && functionForNode(reader) == &function)
          appendBucketEntry(result.summary.global_readers, seen_readers,
                            global_nodes[i], reader, {});
      }
      for (Node::EdgeSet::const_iterator edge_it =
               global_nodes[i]->getInEdgeSet().begin();
           edge_it != global_nodes[i]->getInEdgeSet().end(); ++edge_it) {
        Edge *edge = *edge_it;
        Node *writer = edge != nullptr ? edge->getSrcNode() : nullptr;
        if (writer != nullptr && functionForNode(writer) == &function)
          appendBucketEntry(result.summary.global_writers, seen_writers,
                            global_nodes[i], writer, {});
      }
    }
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::ControlPredicates)) {
    const std::set<EdgeType> control_edges =
        edgeTypesForPreset(PDGEdgePreset::Control);
    PairSet seen_ctrl;
    for (Node *ctrl : control_nodes) {
      std::queue<Node *> worklist;
      llvm::DenseSet<Node *> visited;
      worklist.push(ctrl);
      visited.insert(ctrl);
      while (!worklist.empty()) {
        Node *cur = worklist.front();
        worklist.pop();
        for (Edge *edge : cur->getOutEdgeSet()) {
          if (!edge || !query_detail::isEdgeAllowed(edge->getEdgeType(),
                                                    control_edges))
            continue;
          Node *nbr = edge->getDstNode();
          if (!nbr || function_nodes.count(nbr) == 0)
            continue;
          if (visited.insert(nbr).second) {
            appendBucketEntry(result.summary.control_predicates, seen_ctrl,
                              ctrl, nbr, {});
            worklist.push(nbr);
          }
        }
      }
    }
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::ReachableCalls)) {
    PairSet seen_calls;
    for (size_t i = 0; i < call_nodes.size(); ++i)
      appendBucketEntry(result.summary.reachable_calls, seen_calls, nullptr,
                        call_nodes[i], {});
  }

  if (summaryKindEnabled(policy.kind, SummaryKind::ResourceKinds) ||
      policy.kind == SummaryKind::All) {
    for (size_t i = 0; i < call_nodes.size(); ++i) {
      const ResourceKind acquire_kind = resourceKindForNode(call_nodes[i], false);
      const ResourceKind release_kind = resourceKindForNode(call_nodes[i], true);
      if (acquire_kind != ResourceKind::Unknown)
        result.summary.may_allocate_resource_kinds.insert(acquire_kind);
      if (release_kind != ResourceKind::Unknown)
        result.summary.may_release_resource_kinds.insert(release_kind);
    }
  }

  if (options.cache_policy == PDGCachePolicy::Enabled)
    summary_cache_[cache_key] = result;
  return result;
}

SummaryQueryResult SummaryQuery::summarize(const PDGCriteria &criteria,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options,
                                           const Module *module) const {
  SummaryQueryResult result;
  const Function *function = nullptr;
  if (!tryResolveSingleFunction(pdg_, criteria, options, module, function,
                                result.diagnostics) ||
      function == nullptr)
    return result;
  return summarize(*function, policy, options, module);
}

} // namespace pdg
