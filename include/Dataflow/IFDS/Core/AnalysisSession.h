#pragma once

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/CFG/SparseControlFlow.h"
#include "Analysis/CallGraph/CallGraphAnalysisType.h"
#include "Annotation/Taint/TaintConfigParser.h"
#include "Dataflow/ControlFlow/InterCFG.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {
class DIBasedTypeHierarchy;
class LLVMVFTableProvider;
} // namespace lotus

namespace ifds {

struct CalleeTargets {
  std::vector<const llvm::Function *> functions;
  // Incomplete target sets also carry an unknown callee to bypass flows.
  bool complete = false;
};
using CalleeProvider = std::function<CalleeTargets(const llvm::CallBase &)>;

/// Reusable graph snapshot. The module and context must outlive it.
class AnalysisGraph {
public:
  using Instructions = std::vector<const llvm::Instruction *>;
  using Callees = std::vector<const llvm::Function *>;
  using Calls = std::vector<const llvm::CallBase *>;
  AnalysisGraph(llvm::Module &module,
                std::shared_ptr<dataflow::controlflow::InterCFG> icfg = {},
                CalleeProvider provider = {},
                lotus::SparseLLVMControlFlow::AliasCheckFn may_alias = {});
  llvm::Module &module() const { return m_module; }
  const Instructions &successors(const llvm::Instruction *inst) const;
  const Instructions &return_sites(const llvm::CallBase *call) const;
  const std::unordered_map<const llvm::CallBase *, Callees> &callees() const {
    return m_callees;
  }
  const std::unordered_map<const llvm::Function *, Calls> &callers() const {
    return m_callers;
  }
  const lotus::SparseLLVMBasedICFG &sparse_cfg() const { return m_sparse; }

private:
  llvm::Module &m_module;
  std::shared_ptr<dataflow::controlflow::InterCFG> m_icfg;
  lotus::SparseLLVMBasedICFG m_sparse;
  std::unordered_map<const llvm::Instruction *, Instructions> m_successors;
  std::unordered_map<const llvm::CallBase *, Instructions> m_return_sites;
  std::unordered_map<const llvm::CallBase *, Callees> m_callees;
  std::unordered_map<const llvm::Function *, Calls> m_callers;
};

/// Lazy services for one module. Not thread-safe. Invalidate after IR mutation
/// and solve again. Retained snapshots keep their services alive, but results
/// from before mutation must not be queried. Module/context must outlive users.
class AnalysisSession {
public:
  struct Options {
    lotus::AAConfig alias_config = lotus::AAConfig::SparrowAA_NoCtx();
    lotus::CallGraphAnalysisType call_graph = lotus::CallGraphAnalysisType::OTF;
    std::vector<std::string> entry_points = {"main"};
    // Explicit preprocessing: these options can rewrite the module. Construct
    // the session before creating other analyses or retaining IR pointers.
    bool model_global_initializers = false;
    bool model_external_callbacks = false;
  };
  explicit AnalysisSession(llvm::Module &module);
  AnalysisSession(llvm::Module &module, Options options,
                  const TaintConfig *taint = nullptr);
  ~AnalysisSession();
  AnalysisSession(const AnalysisSession &) = delete;
  AnalysisSession &operator=(const AnalysisSession &) = delete;
  llvm::Module &module() const { return m_module; }
  const Options &options() const { return m_options; }
  std::shared_ptr<lotus::AliasAnalysisWrapper> alias_analysis();
  std::shared_ptr<const lotus::DIBasedTypeHierarchy> type_hierarchy();
  std::shared_ptr<const AnalysisGraph> graph();
  std::shared_ptr<const TaintConfig> taint_config() const { return m_taint; }
  void invalidate();

private:
  llvm::Module &m_module;
  const Options m_options;
  const std::shared_ptr<const TaintConfig> m_taint;
  std::shared_ptr<lotus::AliasAnalysisWrapper> m_alias;
  std::shared_ptr<lotus::DIBasedTypeHierarchy> m_hierarchy;
  std::shared_ptr<lotus::LLVMVFTableProvider> m_vtables;
  std::shared_ptr<const AnalysisGraph> m_graph;
};
} // namespace ifds
