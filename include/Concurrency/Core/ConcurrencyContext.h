/**
 * @file ConcurrencyContext.h
 * @brief Shared per-module context and pre-indexed infrastructure for
 * concurrency analyses.
 *
 * Provides a single per-module home for shared call graphs, direct caller/callee
 * indices, module-wide runtime presence flags, cached per-function LLVM
 * analyses (DominatorTree, PostDominatorTree, LoopInfo), shared alias analysis,
 * and ThreadMultiplicityAnalysis so sibling concurrency analyses do not
 * duplicate expensive whole-module passes.
 */

#pragma once

#include "Concurrency/API/ThreadAPI.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace concurrency {

class ThreadMultiplicityAnalysis;

/**
 * @brief Single-pass summary of concurrency features present in a module.
 */
struct ModuleConcurrencySummary {
  bool has_forks = false;
  bool has_joins = false;
  bool has_locks = false;
  bool has_atomics = false;
  bool has_condvars = false;
  bool has_barriers = false;
  bool has_openmp = false;
  bool has_mpi = false;
  bool has_cuda = false;
  bool has_linux_kernel = false;

  std::vector<const llvm::Instruction *> fork_sites;
  std::vector<const llvm::Instruction *> join_sites;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      direct_callees;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      direct_callers;

  bool hasSharedMemoryConcurrency() const {
    return has_forks || has_joins || has_locks || has_atomics || has_condvars ||
           has_barriers || has_openmp || has_linux_kernel;
  }
};

/**
 * @brief Shared per-module analysis context for Concurrency subsystems.
 */
class ConcurrencyContext {
public:
  /// Result of AA-augmented indirect call target resolution at a call site.
  struct ResolvedCallees {
    std::vector<const llvm::Function *> targets;
    bool has_unresolved = false;
  };

  explicit ConcurrencyContext(
      llvm::Module &module,
      lotus::AliasAnalysisWrapper *borrowed_aa = nullptr);
  ~ConcurrencyContext();

  ConcurrencyContext(const ConcurrencyContext &) = delete;
  ConcurrencyContext &operator=(const ConcurrencyContext &) = delete;

  llvm::Module &getModule() const { return m_module; }
  ThreadAPI *getThreadAPI() const { return m_thread_api; }

  /// Set a caller-owned alias analysis wrapper to share across analyses.
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa);

  /// Return the shared alias analysis if already configured/built, else null.
  lotus::AliasAnalysisWrapper *getAliasAnalysisIfPresent() const {
    return m_alias_analysis;
  }

  /// Return the shared alias analysis, lazily creating SparrowAA_NoCtx once if
  /// no wrapper was provided.
  lotus::AliasAnalysisWrapper *getOrCreateAliasAnalysis();

  /// Lazily build and return the module's LLVM CallGraph.
  llvm::CallGraph &getCallGraph();

  /// Resolve possible defined callees for a direct or indirect call site using
  /// IR def-use tracking, LLVM CallGraph, and (when available) alias analysis
  /// points-to sets filtered by compatible call signature.
  const ResolvedCallees &resolveCallees(const llvm::CallBase *call);

  /// Resolve possible defined target functions for an arbitrary function-valued
  /// operand (such as an indirect thread-fork start routine) using IR def-use
  /// tracking and alias analysis points-to sets.
  ResolvedCallees resolveFunctionTargets(const llvm::Value *fnVal) const;

  /// Return true if @p module contains any thread/task/kernel creation site or
  /// an address-taken thread-creation API declaration.
  static bool moduleHasPotentialThreadCreation(const llvm::Module &module,
                                               ThreadAPI *api);

  /// Cached check of whether this context's module may create concurrent
  /// threads or tasks.
  bool hasPotentialThreadCreation();

  /// Return true if @p func is a well-known external runtime/libc function that
  /// never acquires/releases user locks or spawns user threads.
  static bool isKnownSyncFreeExternalFunction(const llvm::Function *func);

  /// Return true if @p func is a well-known external function that never
  /// captures/publishes pointer arguments into thread-shared state.
  static bool isKnownNonCapturingExternalFunction(const llvm::Function *func);

  /// Lazily scan the module once and return the concurrency summary.
  const ModuleConcurrencySummary &getSummary();

  /// Lazily build and cache per-function LLVM analyses.
  const llvm::DominatorTree &getDominatorTree(const llvm::Function &func);
  const llvm::PostDominatorTree &
  getPostDominatorTree(const llvm::Function &func);
  const llvm::LoopInfo &getLoopInfo(const llvm::Function &func);

  /// Lazily build and return a shared ThreadMultiplicityAnalysis.
  ThreadMultiplicityAnalysis &getThreadMultiplicity();

  /// Invalidate cached structural summaries if the module is mutated.
  void invalidate();

private:
  void buildSummary();

  llvm::Module &m_module;
  ThreadAPI *m_thread_api;

  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  std::unique_ptr<lotus::AliasAnalysisWrapper> m_owned_alias_analysis;

  std::unique_ptr<llvm::CallGraph> m_call_graph;
  std::unique_ptr<ThreadMultiplicityAnalysis> m_thread_multiplicity;

  bool m_summary_built = false;
  ModuleConcurrencySummary m_summary;

  std::unordered_map<const llvm::Function *,
                     std::unique_ptr<llvm::DominatorTree>>
      m_dom_trees;
  std::unordered_map<const llvm::Function *,
                     std::unique_ptr<llvm::PostDominatorTree>>
      m_post_dom_trees;
  std::unordered_map<const llvm::Function *, std::unique_ptr<llvm::LoopInfo>>
      m_loop_infos;
  std::unordered_map<const llvm::CallBase *, ResolvedCallees>
      m_resolved_callees;
};

} // namespace concurrency
