#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGUtils.h"
#include "QuerySupport.h"
/**
 * @file ResourceFlowQuery.cpp
 * @brief Implementation of built-in resource-flow analysis over the PDG.
 *
 * This file implements a conservative resource analysis for a fixed set of
 * common acquire/release APIs. It uses PDG value-flow for local matching and
 * SummaryQuery to reuse interprocedural resource behavior.
 */

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Query/DependenceQuery.h"
#include "IR/PDG/Query/ResourceFlowQuery.h"
#include "IR/PDG/Query/SliceQuery.h"
#include "IR/PDG/Query/SummaryQuery.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace pdg {
using query_detail::toLower;

std::string resourceKindName(ResourceKind kind) {
  switch (kind) {
  case ResourceKind::Heap:
    return "heap";
  case ResourceKind::File:
    return "file";
  case ResourceKind::FileDescriptor:
    return "fd";
  case ResourceKind::Directory:
    return "dir";
  case ResourceKind::Lock:
    return "lock";
  case ResourceKind::Unknown:
  default:
    return "unknown";
  }
}

namespace {

using NodeSet = PDGQueryResult::NodeSet;

static bool isCallNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_FUNCALL;
}

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

/// Called function name for a callsite node, or empty when unavailable.
static std::string calleeName(Node *node) {
  if (!isCallNode(node))
    return "";
  const CallBase *call = dyn_cast_or_null<CallBase>(node->getValue());
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return "";
  return call->getCalledFunction()->getName().str();
}

/// Built-in acquire API classification.
static ResourceKind resourceKindForAcquireName(const std::string &api_name) {
  const std::string lower = toLower(api_name);

  if (lower == "malloc" || lower == "calloc" || lower == "realloc" ||
      lower == "reallocf" || lower == "valloc" || lower == "aligned_alloc" ||
      lower == "posix_memalign" || lower == "memalign" || lower == "pvalloc")
    return ResourceKind::Heap;

  if (lower == "mmap" || lower == "mmap64")
    return ResourceKind::Heap;

  if (lower == "fopen" || lower == "fopen64" || lower == "freopen" ||
      lower == "freopen64" || lower == "tmpfile" || lower == "tmpfile64" ||
      lower == "fdopen" || lower == "popen")
    return ResourceKind::File;

  if (lower == "open" || lower == "open64" || lower == "creat" ||
      lower == "creat64" || lower == "socket" || lower == "socketpair" ||
      lower == "accept" || lower == "accept4" || lower == "dup" ||
      lower == "dup2" || lower == "dup3" || lower == "epoll_create" ||
      lower == "eventfd" || lower == "signalfd" || lower == "timerfd_create" ||
      lower == "inotify_init" || lower == "memfd_create" || lower == "shm_open")
    return ResourceKind::FileDescriptor;

  if (lower == "opendir" || lower == "fdopendir")
    return ResourceKind::Directory;

  if (lower == "pthread_mutex_lock" ||
      lower == "pthread_mutex_trylock" ||
      lower == "pthread_spin_lock" ||
      lower == "pthread_spin_trylock" ||
      lower == "pthread_rwlock_rdlock" ||
      lower == "pthread_rwlock_tryrdlock" ||
      lower == "pthread_rwlock_wrlock" ||
      lower == "pthread_rwlock_trywrlock" ||
      lower == "mtx_lock" || lower == "mtx_trylock")
    return ResourceKind::Lock;

  return ResourceKind::Unknown;
}

/// Built-in release API classification.
static ResourceKind resourceKindForReleaseName(const std::string &api_name) {
  const std::string lower = toLower(api_name);

  if (lower == "free" || lower == "cfree" ||
      lower == "munmap" || lower == "munmap64")
    return ResourceKind::Heap;

  if (lower == "fclose" || lower == "pclose")
    return ResourceKind::File;

  if (lower == "close" || lower == "closefrom" || lower == "shutdown")
    return ResourceKind::FileDescriptor;

  if (lower == "closedir")
    return ResourceKind::Directory;

  if (lower == "pthread_mutex_unlock" ||
      lower == "pthread_spin_unlock" ||
      lower == "pthread_rwlock_unlock" ||
      lower == "mtx_unlock")
    return ResourceKind::Lock;

  return ResourceKind::Unknown;
}

static ResourceKind resourceKindForNode(Node *node, bool release) {
  const std::string name = calleeName(node);
  return release ? resourceKindForReleaseName(name)
                 : resourceKindForAcquireName(name);
}

static bool resourceKindMatches(ResourceKind lhs, ResourceKind filter) {
  return filter == ResourceKind::Unknown || lhs == filter;
}

} // namespace

ResourceFlowQueryResult
ResourceFlowQuery::analyze(const PDGCriteria &criteria,
                           const ResourcePolicy &policy,
                           const PDGQueryOptions &options,
                           const Module *module) const {
  // If the caller did not provide criteria, the analysis scans the current
  // scope (or the whole graph) for built-in acquire/release APIs.
  ResourceFlowQueryResult result;
  PDGQueryOptions local_options = options;
  if (local_options.edge_preset == PDGEdgePreset::All)
    local_options.edge_preset = PDGEdgePreset::ValueFlow;

  const bool has_scope_filter =
      local_options.scope.kind != PDGQueryScope::Kind::WholeGraph;
  const NodeSet scoped_nodes =
      has_scope_filter ? nodesInScope(pdg_, local_options.scope) : NodeSet{};
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(local_options.edge_preset);

  std::map<ResourceKind, std::vector<Node *>> acquires_by_kind;
  std::map<ResourceKind, std::vector<Node *>> releases_by_kind;

  auto classifyNode = [&](Node *node) {
    if (!isCallNode(node))
      return;
    const ResourceKind acquire_kind = resourceKindForNode(node, false);
    const ResourceKind release_kind = resourceKindForNode(node, true);
    if (acquire_kind != ResourceKind::Unknown &&
        resourceKindMatches(acquire_kind, policy.resource_kind)) {
      acquires_by_kind[acquire_kind].push_back(node);
      result.acquire_sites.push_back(ResourceEvent{
          acquire_kind, ResourceEventKind::Acquire, node, calleeName(node)});
      result.resource_kind_counts[acquire_kind]++;
    }
    if (release_kind != ResourceKind::Unknown &&
        resourceKindMatches(release_kind, policy.resource_kind)) {
      releases_by_kind[release_kind].push_back(node);
      result.release_sites.push_back(ResourceEvent{
          release_kind, ResourceEventKind::Release, node, calleeName(node)});
    }
  };

  if (!criteria.empty()) {
    SliceQuery slice_query(pdg_);
    PDGQueryOptions region_opts = local_options;
    region_opts.explain = false;
    NodeSet region = slice_query.forward(criteria, region_opts, module).nodes;
    for (Node *node : region)
      classifyNode(node);
  } else if (has_scope_filter) {
    for (Node *node : scoped_nodes)
      classifyNode(node);
  } else {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it)
      classifyNode(*it);
  }

  // Precompute caller -> release-kinds map once if interprocedural fallback
  // may be needed, avoiding O(|acquires| * |functions|) SummaryQuery::All calls.
  std::unordered_map<std::string, std::set<ResourceKind>>
      callee_to_caller_release_kinds;
  if (policy.include_interprocedural && module != nullptr &&
      !acquires_by_kind.empty()) {
    for (const Function &F : *module) {
      if (F.isDeclaration())
        continue;
      std::set<ResourceKind> func_releases;
      std::vector<std::string> direct_callees;
      for (const Instruction &I : instructions(F)) {
        const auto *cb = dyn_cast<CallBase>(&I);
        if (!cb || !cb->getCalledFunction())
          continue;
        std::string name = cb->getCalledFunction()->getName().str();
        ResourceKind rk = resourceKindForReleaseName(name);
        if (rk != ResourceKind::Unknown)
          func_releases.insert(rk);
        direct_callees.push_back(std::move(name));
      }
      if (!func_releases.empty()) {
        for (const std::string &callee : direct_callees) {
          auto &kinds = callee_to_caller_release_kinds[callee];
          kinds.insert(func_releases.begin(), func_releases.end());
        }
      }
    }
  }

  for (std::map<ResourceKind, std::vector<Node *>>::const_iterator acquire_it =
           acquires_by_kind.begin();
       acquire_it != acquires_by_kind.end(); ++acquire_it) {
    const ResourceKind kind = acquire_it->first;
    const auto rel_it = releases_by_kind.find(kind);
    const std::vector<Node *> empty_releases;
    const std::vector<Node *> &kind_releases =
        rel_it != releases_by_kind.end() ? rel_it->second : empty_releases;
    llvm::DenseSet<Node *> release_set(kind_releases.begin(),
                                       kind_releases.end());

    for (Node *acquire : acquire_it->second) {
      // Single forward BFS from acquire finds shortest paths to ALL reachable
      // releases of the same kind simultaneously.
      llvm::DenseMap<Node *, std::pair<Node *, EdgeType>> pred;
      llvm::DenseSet<Node *> visited;
      std::queue<Node *> worklist;
      std::vector<Node *> candidate_releases;

      if (!release_set.empty()) {
        worklist.push(acquire);
        visited.insert(acquire);
        while (!worklist.empty()) {
          Node *cur = worklist.front();
          worklist.pop();
          for (Edge *edge : cur->getOutEdgeSet()) {
            if (!edge || !query_detail::isEdgeAllowed(edge->getEdgeType(),
                                                      edge_types))
              continue;
            Node *nbr = edge->getDstNode();
            if (!nbr)
              continue;
            if (has_scope_filter && scoped_nodes.count(nbr) == 0)
              continue;
            if (visited.insert(nbr).second) {
              pred[nbr] = {cur, edge->getEdgeType()};
              if (release_set.count(nbr))
                candidate_releases.push_back(nbr);
              worklist.push(nbr);
            }
          }
        }
      }

      bool used_interproc_fallback = false;
      if (candidate_releases.empty() && policy.include_interprocedural &&
          module != nullptr && !kind_releases.empty()) {
        const Function *owner = functionForNode(acquire);
        if (owner != nullptr) {
          auto cit =
              callee_to_caller_release_kinds.find(owner->getName().str());
          if (cit != callee_to_caller_release_kinds.end() &&
              cit->second.count(kind) != 0) {
            candidate_releases.insert(candidate_releases.end(),
                                      kind_releases.begin(),
                                      kind_releases.end());
            used_interproc_fallback = true;
          }
        }
      }

      if (candidate_releases.empty()) {
        ResourcePath path;
        path.resource_kind = kind;
        path.acquire_site = acquire;
        path.orphaned = true;
        path.released = false;
        PDGWitnessPath witness;
        witness.kind = PDGWitnessPathKind::Slice;
        witness.nodes.push_back(acquire);
        path.witness_paths.push_back(std::move(witness));
        result.orphaned_resources.push_back(path);
        result.resource_paths.push_back(std::move(path));
        result.diagnostics.notes.push_back("Detected orphaned resource");
        continue;
      }

      if (candidate_releases.size() > 1)
        result.diagnostics.notes.push_back(
            "Multiple candidate releases observed for resource");

      for (Node *release : candidate_releases) {
        ResourcePath path;
        path.resource_kind = kind;
        path.acquire_site = acquire;
        path.release_site = release;
        path.released = true;
        path.double_release_candidate = candidate_releases.size() > 1;
        if (!used_interproc_fallback && local_options.explain) {
          PDGWitnessPath witness;
          witness.kind = PDGWitnessPathKind::ShortestPath;
          Node *cur = release;
          llvm::DenseSet<Node *> seen;
          while (cur != nullptr && seen.insert(cur).second) {
            witness.nodes.push_back(cur);
            if (cur == acquire)
              break;
            auto pit = pred.find(cur);
            if (pit == pred.end())
              break;
            witness.edge_types.push_back(pit->second.second);
            cur = pit->second.first;
          }
          std::reverse(witness.nodes.begin(), witness.nodes.end());
          std::reverse(witness.edge_types.begin(), witness.edge_types.end());
          if (witness.nodes.size() >= 2)
            path.witness_paths.push_back(std::move(witness));
        }
        result.resource_paths.push_back(path);
        if (path.double_release_candidate)
          result.double_release_candidates.push_back(std::move(path));
      }
    }
  }

  return result;
}

} // namespace pdg
