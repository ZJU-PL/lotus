#pragma once

#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Core/QueryDiagnostics.h"

#include <functional>
#include <map>
#include <set>
#include <utility>

namespace pdg {

class ProgramGraph;

struct TaintPolicy {
  bool include_nonconstant_sources = false;
  size_t max_steps = 200000; // 0 means unbounded
  // Extend/override the shared models without modifying individual rules.
  std::function<CallTaintModel(const llvm::CallBase &)> call_model;
};

struct TaintOrigin {
  const llvm::Value *source = nullptr;
  const llvm::Instruction *concatenation = nullptr;
  bool nonconstant_only = false;
  bool sql_sanitized = false;
  bool allocation_bounded = false;
  unsigned sink_argument = 0;
};

struct TaintFlowResult {
  using Key = std::pair<const llvm::CallBase *, unsigned>;
  std::map<Key, std::vector<TaintOrigin>> string_arguments;
  std::map<Key, std::vector<TaintOrigin>> value_arguments;
  std::map<Key, std::vector<TaintOrigin>> object_arguments;
  /// Scalar SSA provenance joined over analyzed call contexts. This is an
  /// existential flow fact, not a feasible-path or range proof.
  std::map<const llvm::Value *, std::vector<TaintOrigin>> value_origins;
  /// Memory facts immediately before a pointer operand is used. Mutable
  /// contents are deliberately not joined across different program points.
  std::map<std::pair<const llvm::Instruction *, const llvm::Value *>,
           std::vector<TaintOrigin>>
      memory_uses;
  std::map<std::pair<const llvm::Instruction *, const llvm::Value *>,
           std::vector<TaintOrigin>>
      object_uses;
  std::map<std::pair<const llvm::Function *, TaintDomain>, std::set<unsigned>>
      wrapper_arguments;
  std::map<std::pair<const llvm::Function *, TaintDomain>, std::set<unsigned>>
      unconditional_wrapper_arguments;
  std::map<std::pair<const llvm::Function *, TaintDomain>,
           std::vector<TaintSinkGuard>>
      conditional_wrapper_sinks;
  PDGQueryDiagnostics diagnostics;

  std::vector<TaintOrigin>
  origins(const llvm::CallBase &call, unsigned argument,
          TaintChannel channel = TaintChannel::Memory) const;
  std::vector<TaintOrigin> origins(const llvm::Value &value) const;
  std::vector<TaintOrigin>
  originsAt(const llvm::Instruction &site, const llvm::Value &value,
            TaintChannel channel = TaintChannel::Memory) const;
  std::vector<unsigned> sinkArguments(const llvm::CallBase &call,
                                      TaintDomain domain,
                                      bool include_wrappers = true) const;
  bool forwardedParameter(const llvm::CallBase &call, unsigned argument,
                          TaintDomain domain) const;
};

/// PDG-owned context worklist with memoized call/return summaries and
/// separate SSA-value/memory-content facts. No arbitrary PDG dependency edge
/// is treated as taint. Fixed field offsets remain distinct; unknown aliases,
/// indirect calls, unmodeled C++ APIs, character-storage epochs and path feasibility remain partial.
class TaintAnalysis {
public:
  explicit TaintAnalysis(ProgramGraph &graph) : graph_(graph) {}
  TaintFlowResult analyze(const llvm::Module &module,
                          const TaintPolicy &policy = TaintPolicy()) const;

private:
  ProgramGraph &graph_;
};

} // namespace pdg
