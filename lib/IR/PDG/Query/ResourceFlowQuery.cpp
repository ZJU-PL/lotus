#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Query/QuerySupport.h"
#include "IR/PDG/Support/PDGUtils.h"
/**
 * @file ResourceFlowQuery.cpp
 * @brief Implementation of built-in resource-flow analysis over the PDG.
 *
 * This file implements a conservative resource analysis for a fixed set of
 * common acquire/release APIs. It uses PDG value-flow for local matching and
 * SummaryQuery to reuse interprocedural resource behavior.
 */

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Query/DependenceQuery.h"
#include "IR/PDG/Query/ResourceFlowQuery.h"
#include "IR/PDG/Query/SliceQuery.h"
#include "IR/PDG/Query/SummaryQuery.h"

#include <algorithm>

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

  NodeSet region;
  if (!criteria.empty()) {
    SliceQuery slice_query(pdg_);
    region = slice_query.forward(criteria, local_options, module).nodes;
  } else {
    region = nodesInScope(pdg_, local_options.scope);
  }

  SummaryQuery summary_query(pdg_);
  std::map<ResourceKind, std::vector<Node *>> acquires_by_kind;
  std::map<ResourceKind, std::vector<Node *>> releases_by_kind;

  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    Node *node = *it;
    if (!isCallNode(node))
      continue;
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
  }

  DependenceQuery dependence_query(pdg_);
  for (std::map<ResourceKind, std::vector<Node *>>::const_iterator acquire_it =
           acquires_by_kind.begin();
       acquire_it != acquires_by_kind.end(); ++acquire_it) {
    const ResourceKind kind = acquire_it->first;
    for (size_t i = 0; i < acquire_it->second.size(); ++i) {
      Node *acquire = acquire_it->second[i];
      std::vector<Node *> candidate_releases;

      if (releases_by_kind.count(kind) != 0) {
        for (size_t j = 0; j < releases_by_kind.find(kind)->second.size(); ++j) {
          Node *release = releases_by_kind.find(kind)->second[j];
          PDGCriteria source;
          source.nodes.insert(acquire);
          PDGCriteria target;
          target.nodes.insert(release);
          PDGQueryResult path =
              dependence_query.shortestPath(source, target, local_options, module);
          if (!path.witness_paths.empty())
            candidate_releases.push_back(release);
        }
      }

      if (candidate_releases.empty() && policy.include_interprocedural &&
          module != nullptr) {
        const Function *owner = functionForNode(acquire);
        if (owner != nullptr) {
          for (Module::const_iterator fit = module->begin(); fit != module->end();
               ++fit) {
            if (fit->isDeclaration())
              continue;
            SummaryQueryResult summary =
                summary_query.summarize(*fit, SummaryPolicy(), local_options, module);
            if (summary.summary.may_release_resource_kinds.count(kind) == 0)
              continue;
            for (size_t j = 0; j < summary.summary.reachable_calls.size(); ++j) {
              Node *callsite = summary.summary.reachable_calls[j].target;
              if (calleeName(callsite) == owner->getName().str() &&
                  releases_by_kind.count(kind) != 0) {
                candidate_releases.insert(candidate_releases.end(),
                    releases_by_kind.find(kind)->second.begin(),
                    releases_by_kind.find(kind)->second.end());
                break;
              }
            }
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
        path.witness_paths.push_back(witness);
        result.orphaned_resources.push_back(path);
        result.resource_paths.push_back(path);
        result.diagnostics.notes.push_back("Detected orphaned resource");
        continue;
      }

      if (candidate_releases.size() > 1)
        result.diagnostics.notes.push_back(
            "Multiple candidate releases observed for resource");

      for (size_t j = 0; j < candidate_releases.size(); ++j) {
        PDGCriteria source;
        source.nodes.insert(acquire);
        PDGCriteria target;
        target.nodes.insert(candidate_releases[j]);
        PDGQueryResult path_result =
            dependence_query.shortestPath(source, target, local_options, module);
        ResourcePath path;
        path.resource_kind = kind;
        path.acquire_site = acquire;
        path.release_site = candidate_releases[j];
        path.released = true;
        path.double_release_candidate = candidate_releases.size() > 1;
        path.witness_paths = path_result.witness_paths;
        result.resource_paths.push_back(path);
        if (path.double_release_candidate)
          result.double_release_candidates.push_back(path);
      }
    }
  }

  return result;
}

} // namespace pdg
