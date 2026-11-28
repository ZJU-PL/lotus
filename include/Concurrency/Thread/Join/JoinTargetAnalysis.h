/**
 * @file JoinTargetAnalysis.h
 * @brief Join-target set: which fork(s) may be joined by each pthread_join
 *
 * For each join site, computes the set of fork (pthread_create) instructions
 * whose thread ID may be the one waited on (join's arg0 may alias fork's arg0).
 * Used to improve MHP precision (unambiguous join) and thread-flow reasoning.
 *
 * @author rainoftime
 * @date 2026
 */

#pragma once

#include "Concurrency/Thread/Join/JoinTargetState.h"
#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/Thread/ThreadMultiplicity.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace mhp {

enum class JoinAmbiguityReason : uint8_t {
  None = 0,
  NoFeasibleInstance,
  MultipleFeasibleInstances,
  RepeatedForkSite,
  UnknownExternalEffect,
  PathMergedAlternatives,
  WildcardLocation,
};

struct JoinPathAlternative {
  const llvm::BasicBlock *incoming_block = nullptr;
  std::vector<ThreadInstance> possible_instances;
  std::vector<ThreadInstance> feasible_instances;
  std::vector<const llvm::Instruction *> possible_forks;
  std::vector<const llvm::Instruction *> feasible_forks;
  std::vector<const llvm::Value *> related_handle_roots;
  bool has_unknown_live_instance = false;
};

struct JoinResolution {
  std::vector<ThreadInstance> possible_instances;
  std::vector<ThreadInstance> feasible_instances;
  std::vector<const llvm::Instruction *> possible_forks;
  std::vector<const llvm::Instruction *> feasible_forks;
  std::vector<const llvm::Value *> related_handle_roots;
  std::vector<JoinPathAlternative> path_alternatives;
  std::vector<JoinAmbiguityReason> ambiguity_reasons;
  bool has_unknown_live_instance = false;
  bool is_path_sensitive = false;
  bool unambiguous = false;
};

/**
 * @brief For each pthread_join, the set of pthread_create calls that may be
 * joined
 */
class JoinTargetAnalysis {
public:
  explicit JoinTargetAnalysis(
      llvm::Module &module,
      lotus::AliasAnalysisWrapper *aliasAnalysis = nullptr);

  void analyze();

  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    m_aliasAnalysis = aa;
  }

  /**
   * @brief Fork instructions that may be joined by this join (join's arg0 may
   * alias fork's arg0)
   */
  std::vector<const llvm::Instruction *>
  getPossibleJoinedForks(const llvm::Instruction *joinInst) const;

  /**
   * @brief Fork instructions that remain temporally feasible for this join.
   *
   * This refines alias/root matching with intra-procedural CFG feasibility to
   * avoid binding a join to creates that only reuse the same storage later.
   */
  std::vector<const llvm::Instruction *>
  getFeasibleJoinedForks(const llvm::Instruction *joinInst) const;

  std::vector<ThreadInstance>
  getPossibleJoinedInstances(const llvm::Instruction *joinInst) const;

  std::vector<ThreadInstance>
  getFeasibleJoinedInstances(const llvm::Instruction *joinInst) const;

  /**
   * @brief Return the single definite feasible fork for joinInst when provable.
   *
   * Returns nullptr unless joinInst is proven unambiguous and has exactly one
   * feasible target fork.
   */
  const llvm::Instruction *
  getDefiniteFeasibleJoinedFork(const llvm::Instruction *joinInst) const;

  /**
   * @brief True if this join has exactly one possible target fork (unambiguous
   * join)
   */
  bool isUnambiguousJoin(const llvm::Instruction *joinInst) const;

  JoinResolution getJoinResolution(const llvm::Instruction *joinInst) const;

  /**
   * @brief Trace an SSA pthread_t handle back to a stable origin when possible.
   *
   * Supports load/phi/select/bitcast/gep forwarding and, when a module is
   * provided, walks direct callers to map formal arguments back to actuals.
   */
  static const llvm::Value *
  traceThreadHandleRoot(const llvm::Value *value,
                        const llvm::Module *module = nullptr);

  /// Collect all allocas/globals reachable from value (for phi/select gives
  /// multiple roots).
  static void
  traceThreadHandleRoots(const llvm::Value *value, const llvm::Module *module,
                         std::unordered_set<const llvm::Value *> &roots);

private:
  using StateMap = JoinTargetStateMap;

  enum class CandidateCountKind { Zero, One, Many };

  void buildFunctionSummaries();
  void collectForksAndJoins();
  void analyzeFunction(const llvm::Function &func);
  StateMap getEntryStateForFunction(const llvm::Function &func) const;
  StateMap mergePredecessorStates(const llvm::BasicBlock *block) const;
  bool transferInstruction(const llvm::Instruction &inst, StateMap &state) const;
  bool applyCallEffect(const llvm::Instruction &inst, StateMap &state) const;
  bool applyDirectCallSummary(const llvm::CallBase &call,
                              const llvm::Function &callee,
                              StateMap &state) const;
  void recordJoinState(const llvm::Instruction *join_inst, const StateMap &state);
  std::unordered_set<HandleLocation, HandleLocationHash>
  resolveReadLocations(const llvm::Value *value) const;
  std::unordered_set<HandleLocation, HandleLocationHash>
  resolveWriteLocations(const llvm::Value *value) const;
  bool overwriteLocation(StateMap &state, const HandleLocation &location,
                         const HandleState &new_state) const;
  bool weakUpdateLocation(StateMap &state, const HandleLocation &location,
                          const HandleState &new_state) const;
  bool locationsMayAlias(const HandleLocation &lhs,
                         const HandleLocation &rhs) const;
  bool weakenMayAliasLocations(StateMap &state,
                               const HandleLocation &written_location,
                               const HandleState &written_state) const;
  bool transitionMayAliasLocations(StateMap &state, const llvm::Value *pointer,
                                   ThreadLifecycle terminal) const;
  HandleState getStateForValue(const llvm::Value *value,
                               const StateMap &state) const;
  bool mergeStateInto(StateMap &dst, const StateMap &src) const;
  bool mergeHandleState(HandleState &dst, const HandleState &src) const;
  ThreadInstance makeThreadInstance(const llvm::Instruction &forkInst) const;
  JoinResolution buildResolutionFromState(const HandleState &state) const;
  JoinResolution buildJoinResolution(const llvm::Instruction *joinInst,
                                     const StateMap &state) const;
  void finalizeJoinResolution(const llvm::Instruction *joinInst,
                              JoinResolution &resolution) const;
  bool addAmbiguityReason(JoinResolution &resolution,
                          JoinAmbiguityReason reason) const;
  CandidateCountKind
  classifyJoinForks(const std::vector<const llvm::Instruction *> &forks) const;

  llvm::Module &m_module;
  ThreadAPI *m_threadAPI;
  lotus::AliasAnalysisWrapper *m_aliasAnalysis;

  std::vector<const llvm::Instruction *> m_forkInsts;
  std::vector<const llvm::Instruction *> m_joinInsts;
  std::unordered_map<const llvm::Instruction *, JoinResolution>
      m_joinResolutions;
  std::unordered_map<const llvm::Function *, FunctionSummary> m_functionSummaries;
  mutable std::unordered_map<const llvm::BasicBlock *, StateMap> m_blockInStates;
  mutable std::unordered_map<const llvm::BasicBlock *, StateMap> m_blockOutStates;
  mutable std::unique_ptr<concurrency::ThreadMultiplicityAnalysis>
      m_threadMultiplicity;
};

} // namespace mhp

