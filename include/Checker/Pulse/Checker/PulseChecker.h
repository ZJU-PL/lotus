/** @file PulseChecker.h @brief Main Pulse checker driver for abstract-interpretation-based bug detection. */
#pragma once

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"
#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLatentIssue.h"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace llvm {
class BasicBlock;
class Module;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace pulse {

class PulseModels;

/**
 * PulseChecker: main bug finder using biabductive incorrectness analysis.
 */
class PulseChecker {
private:
  llvm::Module *module_;
  lotus::AliasAnalysisWrapper *aa_;
  AbstractValueFactory factory_;
  PulseOperations ops_;
  std::unique_ptr<PulseModels> models_;
  static constexpr unsigned kMaxDisjuncts = 10u;
  static constexpr unsigned kMaxCallDepth = 5u;

  int useAfterFreeTypeId_{-1};
  int nullDerefTypeId_{-1};
  int uninitializedReadTypeId_{-1};
  int unnecessaryCopyTypeId_{-1};
  int constRefableParamTypeId_{-1};
  int taintErrorTypeId_{-1};
  int stackAddressEscapeTypeId_{-1};
  int invalidFreeTypeId_{-1};
  int outOfBoundsTypeId_{-1};
  int memoryLeakTypeId_{-1};
  int resourceLeakTypeId_{-1};
  int mutualRecursionTypeId_{-1};

  std::map<const llvm::Function *, std::vector<ExecutionDomain>>
      function_states_;
  NonDisjunctiveDomain analysis_non_disj_;
  SummaryManager summary_manager_;
  SpecializationManager specialization_manager_;
  ToplProgram topl_program_;
  int toplTypeId_ = -1;
  std::set<std::pair<std::string, const llvm::Instruction *>> topl_reported_;

  std::map<const llvm::Function *, DisjunctiveDomain> disjunctive_domains_;
  std::map<const llvm::Function *, LoopAbstraction> loop_abstractions_;
  std::map<const llvm::Function *, TransitiveInfo> transitive_info_;

  // std::deque guarantees stable pointers/references for ExecutionDomain's
  // LatentIssue* across emplace_back calls.
  std::deque<LatentIssue> latent_issues_;

  // During SCC-based scheduling, calls within the current SCC are recorded as
  // recursive/unknown to prevent unbounded recursion.
  std::unordered_set<const llvm::Function *> current_scc_;

  // Functions currently on the active analysis stack (guards against recursive
  // on-demand analysis of indirectly resolved callees and Pass-2 SCC cycles).
  std::unordered_set<const llvm::Function *> active_functions_;

  // Guard against recursive on-demand specialization cycles.
  std::set<std::pair<const llvm::Function *, SpecializationKey>>
      active_specializations_;

public:
  void setToplProgram(ToplProgram program) {
    topl_program_ = std::move(program);
    topl_reported_.clear();
  }
  ToplValue toplValue(AbductiveDomain &state, const llvm::Value *value,
                      const llvm::Instruction *location,
                      const llvm::BasicBlock *pred);
  void recordToplEvent(AbductiveDomain &state, ToplEvent event);
  void reportTopl(const AbductiveDomain &state);

  explicit PulseChecker(llvm::Module *M,
                        lotus::AliasAnalysisWrapper *AA = nullptr);
  ~PulseChecker();

  void analyze();
  void analyzeFunction(const llvm::Function *F);
  const PulseSummary *
  analyzeFunctionSpecialized(const llvm::Function *F,
                             const SpecializationKey &key);

  std::vector<ExecutionDomain> executeInstruction(const llvm::Instruction *I,
                                                  ExecutionDomain exec_state,
                                                  const llvm::BasicBlock *pred,
                                                  unsigned call_depth);

  ExecutionDomain handleLoad(const llvm::LoadInst *LI,
                             ExecutionDomain exec_state,
                             const llvm::BasicBlock *pred);
  ExecutionDomain handleStore(const llvm::StoreInst *SI,
                              ExecutionDomain exec_state,
                              const llvm::BasicBlock *pred);
  std::vector<ExecutionDomain> handleCall(const llvm::CallInst *CI,
                                          ExecutionDomain exec_state,
                                          const llvm::BasicBlock *pred,
                                          unsigned call_depth);
  std::vector<ExecutionDomain> handleCallBase(const llvm::CallBase *CB,
                                              ExecutionDomain exec_state,
                                              const llvm::BasicBlock *pred,
                                              unsigned call_depth);
  ExecutionDomain handleAlloca(const llvm::AllocaInst *AI,
                               ExecutionDomain exec_state);
  ExecutionDomain handleReturn(const llvm::ReturnInst *RI,
                               ExecutionDomain exec_state);

  void reportBug(OperationResult kind, const llvm::Instruction *loc,
                 AbstractValue addr, const Trace &trace,
                 const AbductiveDomain *astate = nullptr);

  void reportDiagnostic(const llvm::Instruction *loc,
                        const std::string &message, const std::string &type,
                        int confidence);
  void reportDiagnostic(const Diagnostic &diagnostic);

  AbstractValueFactory &getFactory() { return factory_; }
  PulseOperations &getOperations() { return ops_; }
  const SummaryManager &getSummaryManager() const { return summary_manager_; }
  const SpecializationManager &getSpecializationManager() const {
    return specialization_manager_;
  }

  void registerBugTypes();
  ExecutionDomain initializeFunction(const llvm::Function *F);

  std::vector<std::pair<ExecutionDomain, std::optional<AbstractValue>>>
  runCallee(const llvm::Function *callee, const ExecutionDomain &caller_state,
            const llvm::CallInst *CI, const llvm::BasicBlock *pred,
            unsigned call_depth);

  void createSummary(const llvm::Function *F,
                     const std::vector<ExecutionDomain> &exit_states,
                     const std::vector<ExecutionDomain> &latent_exit_states,
                     const SpecializationKey *spec_key = nullptr);

  std::vector<ExecutionDomain> applySummary(const llvm::Function *callee,
                                            const ExecutionDomain &caller_state,
                                            const llvm::CallInst *CI,
                                            const llvm::BasicBlock *pred);

  std::vector<ExecutionDomain>
  applySummaryImproved(const llvm::Function *callee,
                       const ExecutionDomain &caller_state,
                       const llvm::CallBase *CI, const llvm::BasicBlock *pred,
                       const PulseSummary *summary_override = nullptr);

  ExecutionDomain handleComparison(const llvm::Instruction *I,
                                   ExecutionDomain exec_state,
                                   const llvm::BasicBlock *pred);

  std::vector<ExecutionDomain>
  handleLibraryCall(const llvm::CallBase *CI, ExecutionDomain exec_state,
                    const llvm::BasicBlock *pred,
                    const llvm::Function *resolved_callee = nullptr);

  std::optional<ExecutionDomain>
  applyBranchCondition(ExecutionDomain state, const llvm::BranchInst *BI,
                       unsigned successor_index,
                       const llvm::BasicBlock *pred_bb);

  void reportUnnecessaryCopies(const llvm::Function *F);
  void reportConstRefableParams(const llvm::Function *F);

private:
  void analyzeFunctionInternal(const llvm::Function *F,
                               ExecutionDomain init_state,
                               const SpecializationKey *spec_key);

  std::optional<ExecutionDomain>
  assumeCondition(ExecutionDomain state, const llvm::Value *condition,
                  bool assumed_true, const llvm::Instruction *location,
                  const llvm::BasicBlock *pred_bb);

  const PulseSummary *resolveSummaryForCall(const llvm::Function *callee,
                                            const ExecutionDomain &caller_state,
                                            const llvm::CallBase *CI,
                                            const llvm::BasicBlock *pred);
  void materializeSpecializedSummary(const llvm::Function *callee,
                                     const PulseSummary &base_summary,
                                     const SpecializationKey &key);
};

} // namespace pulse
