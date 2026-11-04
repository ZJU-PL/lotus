#pragma once

#include "Checker/GSAF/API/Trace.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/GSAF/Engine/Summaries.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "Checker/GSAF/Support/Options.h"
#include "Utils/Parallel/ThreadPool.h"
#include "Utils/Platform/Timer.h"

#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace lotus::gsaf {
using namespace llvm;

using namespace llvm;

class FunctionAnalyzer : public GSAFFunctionWorker {
private:
  /// SMTFactory that manages symbolic expressions created in this function
  SMTFactory *Fctry = nullptr;

  /// the buggy traces to report in this function
  TraceList<std::pair<int, std::shared_ptr<VulnerabilityTrace>>> &BugTraces;

  /// the summary of this function
  GSAFSummary *Smry;

  /// dom tree and post-dom tree
  const llvm::DominatorTree *DT;
  const llvm::PostDominatorTree *PDT;

  /// the native guarded value-flow graph
  const GuardedValueFlowGraph *Graph;

  /// Push/Pop utilities
  /// @{
  /// This field collects constraints of data dependencies.
  /// It also used for checking if certain constraints
  /// are satisfiable.
  GuardedValueFlowSolver *Solver;

  /// This field collects constraints of ctrl dependencies
  PushPopVector<SMTExpr> CtrlConds;

  /// Cache inter-procedural constraints
  /// We inline inter-procedual constraints only when the full bug report is
  /// constructed
  PushPopVector<SummaryCacheItem> NonSymDepsCache;
  PushPopVector<SummaryCacheItem> SymbDepsCache;

  /// This field collects traces on the Graph IR.
  VulnerabilityTraceBuilder TraceBuilder;

  // Track each direction separately: search() deliberately starts a backward
  // search at the same node. A different taint source needs its own traversal.
  using SearchKey = std::tuple<const GuardedValueFlowNode *,
                              const GuardedValueFlowNode *,
                              const GuardedValueFlowSite *>;
  std::set<SearchKey> ActiveForwardSearch;
  std::set<SearchKey> ActiveBackwardSearch;

  /// This field records the call site output nodes whose
  /// symbolic summary is not necessarily to be inlined.
  /// That is, if we search from an output summary of
  /// a function, the symbolic summary of the functions'
  /// return value is not necessary to be inlined.
  PushPopCache<const GuardedValueFlowCallOutputNode *> CallSiteOutputCache;

  /// APIs for push/pop/reset states
  /// NOTE: the states does NOT contain TraceBuilder and Solver, please
  /// handle separately
  void pushState();
  void popState();
  void resetState();
  /// @}

  /// Cached summaries
  /// @{
  /// (actual-arg, callsite-output, callee) -> callee output summary
  std::map<std::tuple<const GuardedValueFlowNode *,
                      const GuardedValueFlowNode *, Function *>,
           std::set<OutputSummary *>>
      OutputSmryMap;

  /// callee output summary that does not start from an argument
  /// and, thus, it can be a source of the vulnerability.
  std::vector<
      std::pair<const GuardedValueFlowCallOutputNode *, OutputSummary *>>
      SourceOutSmryVec;

  /// Taint source wrappers.
  /// (actual-arg, instruction) -> summary, this summary
  /// shows that the actual-arg may be tainted at this instruction.
  std::map<const GuardedValueFlowCallSite *,
           std::map<const GuardedValueFlowNode *, std::set<InputSummary *>>>
      SourceWrapperSmryMap;

  /// Taint sources
  std::map<const GuardedValueFlowSite *, std::set<const GuardedValueFlowNode *>>
      Sources;

  /// (actual-arg, instruction) -> callee input summary
  std::map<
      std::pair<const GuardedValueFlowNode *, const GuardedValueFlowCallSite *>,
      std::set<InputSummary *>>
      InputSmryMap;
  /// @}

  /// This is to record the time executed by the function checker
  /// in case it is timeout.
  /// @{
  Timer *TimeChecker;
  Timer *SMTTimeChecker;
  /// @}

  /// The vulnerability instance
  std::shared_ptr<TaintStyleVulnerability> TSV;

public:
  FunctionAnalyzer(GSAFChecker *P, Function *F);

  virtual ~FunctionAnalyzer();

  virtual void run();

private:
  /// It is called at the constructor of the class. It iterates all call sites
  /// in the function, and caches the input/output summaries by calling
  /// FunctionAnalyzer::initOutSummary() and
  /// FunctionAnalyzer::initInSummary(). and
  /// FunctionAnalyzer::initSourceWrapperSummary().
  ///
  /// The cached summaries are stored in the fields
  /// FunctionAnalyzer::InputSmryMap, FunctionAnalyzer::OutputSmryMap,
  /// FunctionAnalyzer::SourceOutSmryVec and
  /// FunctionAnalyzer::SourceWrapperSmryMap
  void initSummary();

  /// Cache the output summaries at the call site CS for the return value (\p
  /// CallSiteOutput) of the call site, e.g. for call site "x=f(...)", it will
  /// cache the the output summary for "x".
  void initOutputSummary(const GuardedValueFlowCallSite *CS, Function *Callee,
                         const GuardedValueFlowCallOutputNode *CallSiteOutput);

  /// Cache the input summaries at the call site CS for the actual \p Actual. \p
  /// ActualIndex is the index of the actual at this call site \p CS. \p
  /// NotPseudo indicates if it is a pseudo actual. For example, for call site
  /// "x=f(y, ...)", it will cache the the input summary for "y".
  void initInputSummary(const GuardedValueFlowCallSite *CS, Function *Callee,
                        const GuardedValueFlowNode *Actual,
                        std::size_t ActualIndex, bool NotPseudo);

  /// Cache the summaries at the call site CS for the actual \p Actual. \p
  /// ActualIndex is the index of the actual at this call site \p CS. \p
  /// NotPseudo indicates if it is a pseudo actual. For example, for call site
  /// "x=f(y, ...)", it will cache the the summary for "y" if "y" is a tainted
  /// source in function f.
  void initSourceWrapperSummary(const GuardedValueFlowCallSite *CS,
                                Function *Callee,
                                const GuardedValueFlowNode *Actual,
                                std::size_t ActualIndex, bool NotPseudo);

  /// Check if there is some summary at this call site \p CS, using the node
  /// \p Node as an actual.
  bool hasSummary(const GuardedValueFlowCallSite *CS,
                  const GuardedValueFlowNode *Node);

  /// Search the Graph from every possible source (\p Src). \p CurrentNode is
  /// the node that is visiting, and \p PreviousNode is the node visited before.
  /// \p InlineDepth indicates the inline depth for the trace built during the
  /// search.
  void bottomUpDepthFirstSearch(const GuardedValueFlowNode *CurrentNode,
                                const GuardedValueFlowNode *PreviousNode,
                                Vulnerability::ValueSitePairType Src,
                                unsigned InlineDepth);

  // This is a bi-direction search
  void search(const GuardedValueFlowNode *CurrentNode,
              const GuardedValueFlowNode *PreviousNode,
              Vulnerability::ValueSitePairType Src, unsigned InlineDepth);

  void finalizeSummary();

  /// Add constraints: "formal == actual"
  void matchFormalActual(SummaryBase *Smry, const GuardedValueFlowCallSite *CS,
                         const std::string &RenameSuffix);

  /// Inline the symbolic summary of \p Symbol, which should be a
  /// CallSiteOutputNode. \p Where, the index of a basic block, indicates the
  /// basic block where we decide to inline the symbolic summary.
  bool
  inlineReturnSymbolicSummary(const GuardedValueFlowCallOutputNode *Symbol);
  bool inlineReturnSymbolicSummary(Vulnerability::ValueSitePairType Src);

  /// Build symbolic summary for each return node of the function
  void buildReturnSymbolicSummary();

  /// Inline the input summary \p Smry at call site \p CS. The source node of
  /// current trace is \p SrcNode. The current inline depth is \p InlineDepth.
  /// \p Case = 0: A tainted pointer reach a call site that has an input
  /// summary. It may report a bug. \p Case = 1: Inlining the taint source
  /// wrapper, then it starts to search from the call site of the wrapper
  /// function. \p Case = 2: Searching from an argument and reaching a call site
  /// that is a taint source (or source wrapper),
  ///              thereby producing a source wrapper summary.
  void inlineCalleeInSummary(const GuardedValueFlowCallSite *CS,
                             InputSummary *Smry,
                             Vulnerability::ValueSitePairType Src,
                             const GuardedValueFlowNode *ActualNode,
                             unsigned InlineDepth, int Case = 0);

  /// Inline the output summary \p Smry at call site \p CS. The source of
  /// current trace is \p Src. The current inline depth is \p InlineDepth. \p
  /// Case = 0: Search from input to output \p Case = 1: Search from output to
  /// input
  void inlineCalleeOutSummary(
      const GuardedValueFlowCallSite *CS, OutputSummary *Smry,
      const GuardedValueFlowCallOutputNode *CallSiteOutput,
      Vulnerability::ValueSitePairType Src, unsigned InlineDepth, int Case = 0);

  // Return true, if a trace, having *Src* as source pair (Value and Site), can
  // be reported
  bool canReport(Vulnerability::ValueSitePairType Src) const;

  /// This function is to process a use site of a node w.r.t. the vulnerability
  /// type.
  int checkUseSite(
      const GuardedValueFlowNode *Node, const GuardedValueFlowSite *UseSite,
      std::unordered_map<const BasicBlock *, SMTSolver::SMTResultType> &,
      bool UpdateCache);

  /// It processes use sites according to the type of use sites
  void processUseSite(Vulnerability::ValueSitePairType Src,
                      const GuardedValueFlowNode *Node,
                      const GuardedValueFlowSite *UseSite,
                      Vulnerability::SiteType USTy, unsigned InlineDepth);

  /// Create input summary
  InputSummary *createInputSummary(
      const SMTExprVec &Constraints, std::shared_ptr<VulnerabilityTrace> Trace,
      const std::unordered_set<const GuardedValueFlowNode *> *Inputs,
      unsigned InlineDepth);

  /// Create output summary
  OutputSummary *createOutputSummary(
      const SMTExprVec &Constraints, std::shared_ptr<VulnerabilityTrace> Trace,
      const std::unordered_set<const GuardedValueFlowNode *> *Inputs,
      unsigned InlineDepth);

  /// Add the cached constraints to build summary
  void addCachedConstraintsToSummary(SummaryBase *S);

  void tryReport(std::shared_ptr<VulnerabilityTrace> Trace);
};

} // namespace lotus::gsaf
