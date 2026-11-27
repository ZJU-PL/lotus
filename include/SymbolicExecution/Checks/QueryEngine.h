/** @file QueryEngine.h @brief Bug query generation, summary inlining, and SMT discharge engine. */
#pragma once

#include "SymbolicExecution/Checks/BugTypes.h"
#include "SymbolicExecution/Checks/Query.h"
#include "SymbolicExecution/Engine/AnalysisExtension.h"
#include "SymbolicExecution/Engine/AnalysisState.h"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SymbolicExecution {

using QueryTraceList = std::vector<std::pair<QuerySet, std::vector<TraceStep>>>;

/// Summary payload carrying unresolved bug queries across function boundaries.
class QuerySummary : public SummaryExtension {
public:
  explicit QuerySummary(QueryTraceList Queries)
      : QueryToTraces(std::move(Queries)) {}

  const QueryTraceList &getQueries() const { return QueryToTraces; }
  void translate(PathCondSolver *NewSolver) override;

private:
  QueryTraceList QueryToTraces;
};

/// StateExtension implementing bug query construction, interprocedural inlining,
/// MPA batch discharging, and report emission without coupling Core to Checks.
class QueryEngine : public StateExtension {
public:
  QueryEngine(unsigned EnabledBugs, BugReportCollector &Collector);

  void onPreInstruction(llvm::Instruction *Inst,
                        const AnalysisState &State) override;
  void onPostInstruction(llvm::Instruction *Inst,
                         const AnalysisState &State) override;
  void onCallSummary(llvm::CallInst *Inst, llvm::Function *Callee,
                     const SummaryExtension *CalleeSummary,
                     const AnalysisState &State) override;
  std::unique_ptr<SummaryExtension>
  finalizeSummary(const AnalysisState &State) override;

private:
  unsigned EnabledBugs = BUG_TY_UNDEF;
  BugReportCollector &Collector;

  QueryTraceList QueryToTraces;
  std::unordered_map<ProgramValuePtr, GuardedProgramValSet> ExtraDepsMap;
  mutable std::map<NumericalQuery *,
                   std::unordered_map<llvm::Instruction *, QuerySet>>
      InlineQueryCache;
  unsigned QueryCount = 0;

  // Shared query bookkeeping & dependency helpers
  void addExtraDeps(const ProgramValuePtr &Dst, const ProgramValuePtr &Src,
                    const Condition &Cond);
  GuardedProgramValSet getDepsVals(const PropertyValuePtr &V) const;
  void addQueryTrace(const NumericalQueryPtr &Q, const Condition &Cond,
                     const TraceStep &Step, const AnalysisState &State);
  void addQueryTrace(const NumericalQueryPtr &Q, const Condition &Cond,
                     const std::vector<TraceStep> &Trace,
                     const AnalysisState &State);

  // Unified query inlining & reporting
  QuerySet inlineQuerySet(const QuerySet &Vals, llvm::Instruction *CS,
                          llvm::Function *Callee,
                          const AnalysisState &State) const;
  QuerySet inlineQuery(const NumericalQueryPtr &Q, llvm::Instruction *CS,
                       llvm::Function *Callee,
                       const AnalysisState &State) const;
  QuerySet inlineDirectQuery(const DirectNumericalQuery *DQ,
                             llvm::Instruction *CS, llvm::Function *Callee,
                             const AnalysisState &State) const;
  QuerySet inlineIndirectBofQuery(const IndirectNumericalQuery *IDQ,
                                  llvm::Instruction *CS, llvm::Function *Callee,
                                  const AnalysisState &State) const;
  bool tryReportQuery(const NumericalQueryPtr &QPtr, const Condition &QDepCond,
                      llvm::Function *SinkFun,
                      const std::vector<TraceStep> &Trace,
                      const AnalysisState &State,
                      bool KnownContextAndQuerySat = false);
  void dischargeQueries(const AnalysisState &State);

  // Memory safety check builders (MemoryChecks.cpp)
  static bool needsMemoryCheck(const PTItem &Pt);
  std::vector<NumericalQueryPtr>
  createBofQuery(const PTItem &Pt, const PropertyValuePtr &AccSz,
                 const GuardedProgramValSet &Deps = GuardedProgramValSet()) const;
  void buildBofQueryLoadStore(llvm::Instruction *Inst,
                              const ProgramValuePtr &Ptr, llvm::Type *AccTy,
                              const AnalysisState &State);
  void buildBofQueryLibCall(llvm::CallInst *Inst, const AnalysisState &State);
  void buildNullDerefQuery(llvm::Instruction *Inst, const AnalysisState &State);
  void buildArrayIndexOOBQuery(llvm::Instruction *Inst,
                               const AnalysisState &State);
  void buildNegativeArrayIndexQuery(llvm::Instruction *Inst,
                                    const AnalysisState &State);
  void buildUninitializedReadQuery(llvm::Instruction *Inst,
                                   const AnalysisState &State);
  void buildUafQuery(llvm::Instruction *Inst, const AnalysisState &State);
  void buildDoubleFreeQuery(llvm::Instruction *Inst,
                            const AnalysisState &State);

  // Arithmetic safety check builders (ArithmeticChecks.cpp)
  void buildDbzQuery(llvm::Instruction *Inst, const AnalysisState &State);
  void buildIntBoundQuery(llvm::Instruction *Inst, SymexBugType BugTy,
                          bool CheckUpperBound, const AnalysisState &State);
  void buildShiftOverflowQuery(llvm::Instruction *Inst,
                               const AnalysisState &State);
  void buildIntTruncationQuery(llvm::Instruction *Inst,
                               const AnalysisState &State);
};

} // namespace SymbolicExecution
