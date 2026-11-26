#pragma once

#include "Utils/Algorithms/Graph/IntervalReachability.h"

#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>

namespace ifds {
class AnalysisGraph;

/// Dense ID valid only for the FunctionIndex snapshot that issued it.
struct FunctionId {
  uint32_t value;
  bool operator==(FunctionId other) const { return value == other.value; }
  bool operator!=(FunctionId other) const { return !(*this == other); }
};

/// All module functions, including declarations, in module order. Neither IDs
/// nor LLVM pointers are valid across arbitrary IR mutations. Module outlives
/// it.
class FunctionIndex {
public:
  explicit FunctionIndex(const llvm::Module &module);
  size_t size() const { return m_functions.size(); }
  const llvm::Module &module() const { return m_module; }
  std::optional<FunctionId> id(const llvm::Function *function) const;
  const llvm::Function *function(FunctionId id) const;
  llvm::ArrayRef<const llvm::Function *> functions() const {
    return m_functions;
  }

private:
  const llvm::Module &m_module;
  std::vector<const llvm::Function *> m_functions;
  std::unordered_map<const llvm::Function *, FunctionId> m_ids;
};

/// SCCs of the known edges of one AnalysisGraph, including unreachable
/// functions and declarations. Unknown call targets do not become invented
/// graph edges.
class CallGraphSCCs {
public:
  CallGraphSCCs(std::shared_ptr<const FunctionIndex> functions,
                std::shared_ptr<const AnalysisGraph> graph);
  std::shared_ptr<const FunctionIndex> functions() const { return m_functions; }
  std::shared_ptr<const AnalysisGraph> graph() const { return m_graph; }
  std::shared_ptr<const lotus::graph::SCCGraph> components() const {
    return m_sccs;
  }
  uint32_t componentOf(FunctionId id) const {
    return m_sccs->componentOf(id.value);
  }

private:
  std::shared_ptr<const FunctionIndex> m_functions;
  std::shared_ptr<const AnalysisGraph> m_graph;
  std::shared_ptr<const lotus::graph::SCCGraph> m_sccs;
};

struct GlobalUsageSummary {
  using GlobalSet = std::set<const llvm::GlobalVariable *>;
  GlobalSet referenced;
  // Unknown/external callees or unresolved pointer-based memory accesses.
  // This is a usage summary, not a points-to or mod/ref analysis.
  bool may_access_unknown_globals = false;
  bool mayUse(const llvm::GlobalVariable *global) const {
    return global && (may_access_unknown_globals || referenced.count(global));
  }
};

/// Statically referenced globals (including constant-expression/initializer
/// references), plus conservative uncertainty. Propagates callee summaries to
/// callers in SCC order. Never excludes constants or non-pointer globals.
class UsedGlobals {
public:
  explicit UsedGlobals(std::shared_ptr<const CallGraphSCCs> sccs);
  const GlobalUsageSummary &direct(FunctionId function) const;
  const GlobalUsageSummary &transitive(FunctionId function) const;
  const GlobalUsageSummary &component(uint32_t scc) const;

private:
  std::shared_ptr<const CallGraphSCCs> m_sccs;
  std::vector<GlobalUsageSummary> m_direct, m_transitive;
};
} // namespace ifds
