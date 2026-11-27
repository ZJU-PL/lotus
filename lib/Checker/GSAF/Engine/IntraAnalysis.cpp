#include "Checker/GSAF/Engine/FunctionAnalyzer.h"

#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "Checker/GSAF/Support/Options.h"
#include "Solvers/SMT/LIBSMT/SMTModel.h"
#include "Utils/LLVM/StringUtils.h"
#include "Utils/Parallel/ThreadPool.h"

#include <utility>

#include <llvm/ADT/ScopeExit.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::gsaf {
using namespace llvm;

/*==----macros for debugging----==*/

#define DEBUG_TYPE "gsaf-function-analyzer"

#define TAINT_WRAPPER_STR "\033[0;31m*Taint wrapper: \033[0m"
#define BUG_STR "\033[0;31m*Bug: \033[0m"
#define SKIP_STR "\033[0;32m#Skip: \033[0m"
#define OUT_SUMMARY_SEP_START                                                  \
  "\033[0;34m/*==----------Out-Summary Start--------==*/\033[0m"
#define OUT_SUMMARY_SEP_END                                                    \
  "\033[0;34m/*==-----------Out-Summary End---------==*/\033[0m"

#define DEBUG_CONSTRAINTS(X)                                                   \
  do {                                                                         \
    if (GSAFOptions::DebugConstraints && F->hasName() &&                       \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

#define DEBUG_TRACE(X)                                                         \
  do {                                                                         \
    if (GSAFOptions::DebugTrace && F->hasName() &&                             \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

#define DEBUG_Graph(X)                                                         \
  do {                                                                         \
    if (GSAFOptions::DotGVFGValFlow && F->hasName() &&                         \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

FunctionAnalyzer::FunctionAnalyzer(GSAFChecker *P, Function *F)
    : GSAFFunctionWorker(P, F), BugTraces(P->FuncTraceMap[F]),
      Smry(P->FuncSmryMap[F]), DT(&P->getDomTree(F)),
      PDT(&P->getPostDomTree(F)), Graph(P->getGraph(F)) {
  TimeChecker = new Timer(
      GSAFOptions::Timeout,
      [F]() { throw std::runtime_error("[TIMEOUT] " + F->getName().str()); },
      6);

  SMTTimeChecker = new Timer(
      GSAFOptions::Timeout,
      [F]() {
        throw std::runtime_error("[SMT TIMEOUT] " + F->getName().str());
      },
      1);
  SMTTimeChecker->suspend();

  initSummary();

  // TSV is the only vulnerability
  TSV = std::static_pointer_cast<TaintStyleVulnerability>(
      Parent->Vuln->getVulnerability(1));
}

void FunctionAnalyzer::ensureSolver() {
  if (Solver)
    return;
  OwnedFactory = Parent->acquireSMTFactory();
  Fctry = OwnedFactory.Factory.get();
  if (GSAFOptions::SolverVersion == 1) {
    auto *configured =
        new GSAFSolver(*Fctry, Parent->getModule()->getDataLayout());
    configured->setModels(Parent->Models, GSAFOptions::EnableHeapAllocFailure,
                          GSAFOptions::EnableFileAllocFailure);
    Solver = configured;
  } else if (GSAFOptions::SolverVersion == 2) {
    auto *configured =
        new DTGSAFSolver(*Fctry, Parent->getModule()->getDataLayout(), DT);
    configured->setModels(Parent->Models, GSAFOptions::EnableHeapAllocFailure,
                          GSAFOptions::EnableFileAllocFailure);
    Solver = configured;
  } else {
    llvm_unreachable(
        "Unsupported symbolic expression solver (can be 1 and 2).");
  }
  if (SMTSolver::GlobalTimeout <= 0 && GSAFOptions::Timeout > 0) {
    Solver->setTimeout(GSAFOptions::Timeout * 1000U);
  }
}

void FunctionAnalyzer::buildRelevantNodes() {
  if (RelevantNodesBuilt)
    return;
  RelevantNodesBuilt = true;

  CommonArgCanInlineInputSmry = false;
  auto TaintV = std::static_pointer_cast<TaintExtras>(TSV);
  for (auto ArgIt = Graph->arg_begin(), E = Graph->arg_end(); ArgIt != E;
       ++ArgIt) {
    if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                   GuardedValueFlowNode>(*ArgIt) &&
        !TaintV->isSinkFunction(Graph, *ArgIt)) {
      CommonArgCanInlineInputSmry = true;
      break;
    }
  }

  std::unordered_map<const GuardedValueFlowNode *,
                     std::vector<const GuardedValueFlowNode *>>
      RevParents;
  std::vector<const GuardedValueFlowNode *> CommonArgWorklist;
  std::vector<const GuardedValueFlowNode *> AnyWorklist;

  for (const auto &NodePtr : Graph->nodes()) {
    const GuardedValueFlowNode *Node = NodePtr.get();
    if (!Node)
      continue;
    for (const GuardedValueFlowNode *P :
         Node->getValueFlowParents(GSAFOptions::EnableArithmeticFlow)) {
      RevParents[P].push_back(Node);
    }

    bool IsCommonArgRelevant = false;
    bool IsAnyRelevant = false;
    for (const GuardedValueFlowSite *Site : Node->useSites()) {
      if (!Site)
        continue;
      if (isa<GuardedValueFlowReturnSite>(Site)) {
        IsCommonArgRelevant = true;
        IsAnyRelevant = true;
        continue;
      }
      if (auto *CS = dyn_cast<GuardedValueFlowCallSite>(Site)) {
        if (CS->getCalledFunction() != F) {
          auto SrcIt = Sources.find(CS);
          if (SrcIt != Sources.end() && SrcIt->second.count(Node)) {
            IsCommonArgRelevant = true;
            IsAnyRelevant = true;
          }
          auto SWIt = SourceWrapperSmryMap.find(CS);
          if (SWIt != SourceWrapperSmryMap.end()) {
            auto NodeIt = SWIt->second.find(Node);
            if (NodeIt != SWIt->second.end() && !NodeIt->second.empty()) {
              IsCommonArgRelevant = true;
              IsAnyRelevant = true;
            }
          }
          auto InIt = InputSmryMap.find(std::make_pair(Node, CS));
          if (InIt != InputSmryMap.end() && !InIt->second.empty()) {
            IsAnyRelevant = true;
            if (CommonArgCanInlineInputSmry)
              IsCommonArgRelevant = true;
          }
          // Output summaries transfer value flow from actual Node to OutputNode;
          // model them as graph edges rather than terminal relevant sites so
          // dead call-output chains (e.g., unused raw_ostream::operator<<) are
          // pruned exactly.
          for (Function *Callee : CS->getCallees()) {
            for (const GuardedValueFlowCallOutputNode *OutputNode :
                 callOutputs(CS, Callee)) {
              auto OutIt =
                  OutputSmryMap.find(std::make_tuple(Node, OutputNode, Callee));
              if (OutIt != OutputSmryMap.end() && !OutIt->second.empty()) {
                RevParents[OutputNode].push_back(Node);
              }
            }
          }
        }
        if (TSV->isPotentialCommonArgSinkCallSite(CS, Node)) {
          IsCommonArgRelevant = true;
          IsAnyRelevant = true;
        }
        if (TSV->isPotentialSinkCallSite(CS, Node)) {
          IsAnyRelevant = true;
        }
      } else {
        if (TSV->isPotentialCommonArgNonCallSite(Site, Node)) {
          IsCommonArgRelevant = true;
          IsAnyRelevant = true;
        }
        if (TSV->isPotentialNonCallRelevantSite(Site, Node)) {
          IsAnyRelevant = true;
        }
      }
    }
    if (IsCommonArgRelevant && CanReachCommonArgSite.insert(Node).second)
      CommonArgWorklist.push_back(Node);
    if (IsAnyRelevant && CanReachAnyRelevantSite.insert(Node).second)
      AnyWorklist.push_back(Node);
  }

  auto propagateBack =
      [&](std::vector<const GuardedValueFlowNode *> &Worklist,
          std::unordered_set<const GuardedValueFlowNode *> &ReachSet) {
        while (!Worklist.empty()) {
          const GuardedValueFlowNode *Cur = Worklist.back();
          Worklist.pop_back();
          auto It = RevParents.find(Cur);
          if (It == RevParents.end())
            continue;
          for (const GuardedValueFlowNode *Pred : It->second) {
            if (ReachSet.insert(Pred).second)
              Worklist.push_back(Pred);
          }
        }
      };

  propagateBack(CommonArgWorklist, CanReachCommonArgSite);
  propagateBack(AnyWorklist, CanReachAnyRelevantSite);
}

void FunctionAnalyzer::clearSrcGoalTracking() {
  ActiveSrcGoalTracking = false;
  SrcCanReachReturn = false;
  ExpectedDirectSinks.clear();
  ExpectedCallSinks.clear();
  ReportedDirectSinks.clear();
  ReportedCallSinks.clear();
}

bool FunctionAnalyzer::allGoalsReportedForCurrentSrc() const {
  return ActiveSrcGoalTracking && !SrcCanReachReturn &&
         ExpectedDirectSinks.empty() && ExpectedCallSinks.empty();
}

void FunctionAnalyzer::markDirectSinkReported(
    const GuardedValueFlowNode *Node, const GuardedValueFlowSite *Site) {
  if (!ActiveSrcGoalTracking)
    return;
  DirectSinkGoalKey Key{Node, Site};
  ReportedDirectSinks.insert(Key);
  ExpectedDirectSinks.erase(Key);
}

void FunctionAnalyzer::markCallSinkReported(
    const GuardedValueFlowNode *Node, const GuardedValueFlowCallSite *CS,
    InputSummary *Smry) {
  if (!ActiveSrcGoalTracking)
    return;
  CallSinkGoalKey Key{Node, CS, Smry};
  ReportedCallSinks.insert(Key);
  ExpectedCallSinks.erase(Key);
}

void FunctionAnalyzer::initSrcGoalTracking(
    const GuardedValueFlowNode *SrcNode, const GuardedValueFlowSite *SrcSite,
    bool IncludeBackwardSearch) {
  clearSrcGoalTracking();
  if (ThreadPool::get()->hasWorkers() || !SrcNode)
    return;

  ActiveSrcGoalTracking = true;
  const bool HasCaller = Parent->hasNonBackEdgeCaller(F);

  std::unordered_set<const GuardedValueFlowNode *> UpVisited;
  std::vector<const GuardedValueFlowNode *> UpWorklist;

  if (IncludeBackwardSearch) {
    std::unordered_set<const GuardedValueFlowNode *> BackVisited;
    std::vector<const GuardedValueFlowNode *> BackWorklist;
    BackVisited.insert(SrcNode);
    BackWorklist.push_back(SrcNode);
    while (!BackWorklist.empty()) {
      const GuardedValueFlowNode *Cur = BackWorklist.back();
      BackWorklist.pop_back();
      if (UpVisited.insert(Cur).second)
        UpWorklist.push_back(Cur);

      if (const auto *CSO = dyn_cast<GuardedValueFlowCallOutputNode>(Cur)) {
        auto *GraphCS = callSite(CSO);
        for (Function *Callee : GraphCS->getCallees()) {
          for (auto InputIt = GraphCS->input_begin(Callee),
                    E = GraphCS->input_end(Callee);
               InputIt != E; ++InputIt) {
            const GuardedValueFlowNode *Actual = (*InputIt).InputNode;
            auto OutIt = OutputSmryMap.find(std::make_tuple(Actual, CSO, Callee));
            if (OutIt == OutputSmryMap.end())
              continue;
            for (OutputSummary *OutSmry : OutIt->second) {
              if (OutSmry->getInlineDepth() >= GSAFOptions::InlineDepth)
                continue;
              auto *OutSrc = OutSmry->getSourceNode();
              if (nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                             GuardedValueFlowNode>(OutSrc)) {
                for (const GuardedValueFlowNode *Arg : GraphCS->getCommonInputs()) {
                  if (Arg && BackVisited.insert(Arg).second)
                    BackWorklist.push_back(Arg);
                }
              } else if (Actual && BackVisited.insert(Actual).second) {
                BackWorklist.push_back(Actual);
              }
            }
          }
        }
      } else {
        for (size_t I = 0; I < Cur->getNumChildren(); ++I) {
          const GuardedValueFlowNode *Child = Cur->getChild(I);
          if (!Child || isa<GuardedValueFlowRegionNode>(Child))
            continue;
          if (const auto *Opcode = dyn_cast<GuardedValueFlowOpcodeNode>(Child)) {
            if (!(Opcode->getOpcodeKind() ==
                  GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr) &&
                !(Opcode->getKind() == GuardedValueFlowNode::Kind::CastOpcode) &&
                !(Opcode->getOpcodeKind() ==
                  GuardedValueFlowOpcodeNode::OpcodeKind::Select)) {
              continue;
            }
          }
          if (const auto *Opcode = dyn_cast<GuardedValueFlowOpcodeNode>(Cur)) {
            if (Opcode->getOpcodeKind() ==
                    GuardedValueFlowOpcodeNode::OpcodeKind::Select &&
                Opcode->getOperand(0) == Child) {
              continue;
            }
            if (Opcode->getOpcodeKind() ==
                    GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr &&
                Opcode->getOperand(0) != Child) {
              continue;
            }
          }
          if (BackVisited.insert(Child).second)
            BackWorklist.push_back(Child);
        }
      }
    }
  } else {
    UpVisited.insert(SrcNode);
    UpWorklist.push_back(SrcNode);
  }

  Instruction *SrcInst = SrcSite ? SrcSite->getInstruction() : nullptr;
  while (!UpWorklist.empty()) {
    const GuardedValueFlowNode *Cur = UpWorklist.back();
    UpWorklist.pop_back();

    for (const GuardedValueFlowNode *ParentNode :
         Cur->getValueFlowParents(GSAFOptions::EnableArithmeticFlow)) {
      if (ParentNode && UpVisited.insert(ParentNode).second)
        UpWorklist.push_back(ParentNode);
    }

    for (const GuardedValueFlowSite *UseSite : Cur->useSites()) {
      if (!UseSite)
        continue;
      if (isa<GuardedValueFlowReturnSite>(UseSite)) {
        if (HasCaller)
          SrcCanReachReturn = true;
        continue;
      }
      if (const auto *CS = dyn_cast<GuardedValueFlowCallSite>(UseSite)) {
        if (CS->getCalledFunction() != F) {
          for (Function *Callee : CS->getCallees()) {
            for (const GuardedValueFlowCallOutputNode *OutputNode :
                 callOutputs(CS, Callee)) {
              auto OutIt =
                  OutputSmryMap.find(std::make_tuple(Cur, OutputNode, Callee));
              if (OutIt != OutputSmryMap.end() && !OutIt->second.empty() &&
                  UpVisited.insert(OutputNode).second) {
                UpWorklist.push_back(OutputNode);
              }
            }
          }
        }
        if (CS == SrcSite)
          continue;
        if (SrcInst && !Parent->isReachable(SrcInst, CS->getInstruction()))
          continue;
        if (TSV->isPotentialSinkCallSite(CS, Cur)) {
          ExpectedDirectSinks.emplace(Cur, CS);
        } else {
          auto InIt = InputSmryMap.find(std::make_pair(Cur, CS));
          if (InIt != InputSmryMap.end()) {
            for (InputSummary *CalleeInSmry : InIt->second) {
              if (CalleeInSmry->getInlineDepth() < GSAFOptions::InlineDepth)
                ExpectedCallSinks.emplace(Cur, CS, CalleeInSmry);
            }
          }
        }
      } else {
        if (UseSite == SrcSite)
          continue;
        if (!TSV->isPotentialNonCallRelevantSite(UseSite, Cur))
          continue;
        if (SrcInst && !Parent->isReachable(SrcInst, UseSite->getInstruction()))
          continue;
        ExpectedDirectSinks.emplace(Cur, UseSite);
      }
    }
  }
}

FunctionAnalyzer::~FunctionAnalyzer() {
  delete Solver;
  Solver = nullptr;
  ReportSolverStack.clear();
  ReportSolverScopeMarks.clear();
  ReportSolver.reset();
  CtrlConds.reset();
  RenamedSummaryCache.clear();
  TryReportSatCache.clear();
  TryReportPinnedExprs.clear();
  delete TimeChecker;
  delete SMTTimeChecker;
  Fctry = nullptr;
  if (OwnedFactory.Factory)
    Parent->releaseSMTFactory(std::move(OwnedFactory));
}

void FunctionAnalyzer::pushState() {
  NonSymDepsCache.push();
  SymbDepsCache.push();
  CallSiteOutputCache.push();
  UsedRenamingCallSites.push();
  UnsatBlocks.push();
}

void FunctionAnalyzer::resetState() {
  NonSymDepsCache.reset();
  SymbDepsCache.reset();
  CallSiteOutputCache.reset();
  UsedRenamingCallSites.reset();
  UnsatBlocks.reset();
}

void FunctionAnalyzer::popState() {
  NonSymDepsCache.pop();
  SymbDepsCache.pop();
  CallSiteOutputCache.pop();
  UsedRenamingCallSites.pop();
  UnsatBlocks.pop();
}

// Main entry point for taint analysis.
// 1. Identifies taint sources.
// 2. Performs forward search from sources to identify reachable sinks or
// propagators.
// 3. Performs search from "Taint Source Wrappers" (special sources).
// 4. Performs search from arguments (potential external sources).
// 5. Performs search from call site outputs (sources from callees).
void FunctionAnalyzer::run() {
  DEBUG_WITH_TYPE("smt-solver",
                  dbgs() << "\nFunction Name: " << F->getName() << "\n");

  std::vector<Vulnerability::ValueSitePairType> SrcVec;
  TSV->setSources(Graph, SrcVec);

  for (auto &SrcIt : SrcVec) {
    const auto *SrcNode = SrcIt.first;
    const auto *SrcSite = SrcIt.second;

    Sources[SrcSite].insert(SrcNode);
  }

  DEBUG_Graph(
      std::vector<const GuardedValueFlowNode *> Srcs;
      std::for_each(SrcVec.begin(), SrcVec.end(),
                    [&Srcs](const Vulnerability::ValueSitePairType &Src) {
                      Srcs.push_back(Src.first);
                    });
      gvfg::GuardedValueFlowSerializer::writeDot(
          *Graph, (F->getName().str() + "-gsaf-value-flow.dot").c_str()););

  const bool HasCaller = Parent->hasNonBackEdgeCaller(F);
  if (SrcVec.empty() && SourceWrapperSmryMap.empty() &&
      SourceOutSmryVec.empty() && !HasCaller) {
    return;
  }

  buildRelevantNodes();

  DEBUG_TRACE(dbgs() << "\t [Search from vulnerability-specified sources]\n");
  for (auto &SrcIt : SrcVec) {
    const auto *SrcNode = SrcIt.first;
    const auto *SrcSite = SrcIt.second;

    assert(SrcNode && SrcSite);

    DEBUG_TRACE(
        dbgs() << "\t [Search from vulnerability-specified sources (node) -> "
               << *SrcNode << "]\n");
    DEBUG_TRACE(
        dbgs() << "\t [Search from vulnerability-specified sources (site) -> "
               << *SrcSite << "]\n");

    if (TSV->checkNode(SrcNode, TraceBuilder)) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *SrcNode << "...\n");
      continue;
    }

    ensureSolver();
    pushState();
    TraceBuilder.push();
    Solver->push();
    // the init constraints of the node
    // for npd, this constraints is true.
    Solver->addAll(Solver->getDataDeps(SrcNode));
    Solver->addAll(Solver->getCtrlDeps(SrcNode));

    TraceBuilder.add(SrcNode);
    if (SrcNode->getParentBasicBlock() != SrcSite->getParentBasicBlock())
      Solver->addAll(
          Solver->getCtrlDeps(SrcSite->getParentBasicBlock(), Graph));

    TraceBuilder.add(SrcSite);

    initSrcGoalTracking(SrcNode, SrcSite, /*IncludeBackwardSearch=*/true);
    runForwardSearch(SrcNode, std::make_pair(SrcNode, SrcSite), 0);
    clearSrcGoalTracking();

    Solver->pop();
    TraceBuilder.pop();
    popState();
  }

  // search from TaintInputSmryMap
  DEBUG_TRACE(dbgs() << "\t [Search from taint-function wrapper]\n");
  for (auto &SrcSiteIt : SourceWrapperSmryMap) {
    auto *SrcSite = SrcSiteIt.first;

    for (auto &SrcNoteIt : SrcSiteIt.second) {
      auto *SrcNode = dyn_cast<GuardedValueFlowNode>(SrcNoteIt.first);

      assert(SrcNode && SrcSite);

      DEBUG_TRACE(dbgs() << "\t [Search from taint-function wrapper (node) -> "
                         << *SrcNode << "]\n");
      DEBUG_TRACE(dbgs() << "\t [Search from taint-function wrapper (site) -> "
                         << *SrcSite << "]\n");

      if (TSV->checkNode(SrcNode, TraceBuilder)) {
        DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *SrcNode << "...\n");
        continue;
      }

      ensureSolver();
      pushState();
      TraceBuilder.push();
      Solver->push();
      // the init constraints of the node
      Solver->addAll(Solver->getDataDeps(SrcNode));
      Solver->addAll(Solver->getCtrlDeps(SrcNode));
      if (SrcNode->getParentBasicBlock() != SrcSite->getParentBasicBlock())
        Solver->addAll(
            Solver->getCtrlDeps(SrcSite->getParentBasicBlock(), Graph));

      TraceBuilder.add(SrcNode);

      auto &SmrySet = SrcNoteIt.second;
      InSummaryCheckCache CheckCache;
      for (InputSummary *Smry : SmrySet) {
        initSrcGoalTracking(SrcNode, SrcSite, /*IncludeBackwardSearch=*/true);
        inlineCalleeInSummary(SrcSite, Smry, std::make_pair(SrcNode, SrcSite),
                              (const GuardedValueFlowNode *)SrcNode, 0, 1,
                              &CheckCache, nullptr);
        clearSrcGoalTracking();
      }

      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }

  // search from arguments (only produces interprocedural summaries for callers)
  DEBUG_TRACE(dbgs() << "\t [Search from arguments]\n");
  if (HasCaller) {
    for (auto ArgIt = Graph->arg_begin(), E = Graph->arg_end(); ArgIt != E;
         ++ArgIt) {
      auto *ArgNode = *ArgIt;

      if (!GSAFOptions::EnableSideEffectSource &&
          nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                     GuardedValueFlowNode>(ArgNode)) {
        continue;
      }

      const bool IsCommonArg =
          nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                     GuardedValueFlowNode>(ArgNode);
      const auto &ReachSet =
          IsCommonArg ? CanReachCommonArgSite : CanReachAnyRelevantSite;
      if (!ReachSet.count(ArgNode)) {
        continue;
      }

      DEBUG_TRACE(dbgs() << "\t [Search from arguments -> " << *ArgNode
                         << "]\n");

      if (TSV->checkNode(ArgNode, TraceBuilder)) {
        DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ArgNode << "...\n");
        continue;
      }

      ensureSolver();
      pushState();
      TraceBuilder.push();
      Solver->push();

      TraceBuilder.add(ArgNode);
      TraceBuilder.add(nullptr);

      if (!summarizeArgumentDAG(ArgNode)) {
        bottomUpDepthFirstSearch(ArgNode, nullptr,
                                 std::make_pair(ArgNode, nullptr), 0);
      }

      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }

  // search from call site outputs
  DEBUG_TRACE(dbgs() << "\t [Search from SourceOutSmryVec]\n");
  for (auto &SrcOutSmryIt : SourceOutSmryVec) {
    auto *CallSiteOutput = SrcOutSmryIt.first;
    auto *OutSmry = SrcOutSmryIt.second;
    assert(!isa<GuardedValueFlowArgumentNode>(OutSmry->getSourceNode()));

    if (!CanReachAnyRelevantSite.count(CallSiteOutput))
      continue;

    DEBUG_TRACE(dbgs() << "\t [Search from SourceOutSmryVec -> "
                       << *CallSiteOutput << "]\n");

    ensureSolver();
    auto Src = std::make_pair(CallSiteOutput, callSite(CallSiteOutput));
    initSrcGoalTracking(CallSiteOutput, callSite(CallSiteOutput),
                        /*IncludeBackwardSearch=*/false);
    inlineCalleeOutSummary(callSite(CallSiteOutput), OutSmry, CallSiteOutput,
                           Src, OutSmry->getInlineDepth() + 1);
    clearSrcGoalTracking();
  }

  buildReturnSymbolicSummary();

  finalizeSummary();
}

static SMTExpr alignAndEqualExpr(SMTFactory &F, SMTExpr Lhs, SMTExpr Rhs) {
  auto toBool = [&](SMTExpr E) -> SMTExpr {
    if (E.isBool())
      return E;
    if (!E.isBitVector())
      return F.createBoolVal(true);
    uint64_t W = E.getBitVecSize();
    return W == 1 ? E.bv12bool() : (E != F.createBitVecVal(0, W));
  };
  if (Lhs.isBitVector() && Rhs.isBitVector()) {
    uint64_t LW = Lhs.getBitVecSize();
    uint64_t RW = Rhs.getBitVecSize();
    if (LW != RW) {
      uint64_t Target = std::max(LW, RW);
      if (LW < Target)
        Lhs = Lhs.basic_zext(Target - LW);
      else if (LW > Target)
        Lhs = Lhs.basic_extract(static_cast<unsigned>(Target - 1), 0);
      if (RW < Target)
        Rhs = Rhs.basic_zext(Target - RW);
      else if (RW > Target)
        Rhs = Rhs.basic_extract(static_cast<unsigned>(Target - 1), 0);
    }
    return Lhs == Rhs;
  }
  if (Lhs.isBool() && Rhs.isBool())
    return Lhs == Rhs;
  return toBool(Lhs) == toBool(Rhs);
}

bool FunctionAnalyzer::summarizeArgumentDAG(
    const GuardedValueFlowNode *ArgNode) {
  const bool IsCommonArgSrc =
      nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                 GuardedValueFlowNode>(ArgNode) != nullptr;
  const auto &ReachSet =
      IsCommonArgSrc ? CanReachCommonArgSite : CanReachAnyRelevantSite;
  const auto Src =
      std::make_pair(ArgNode, static_cast<const GuardedValueFlowSite *>(nullptr));

  struct DagEdge {
    size_t From = 0;
    size_t To = 0;
    const GuardedValueFlowCallSite *CS = nullptr;
    OutputSummary *OutSmry = nullptr;
  };
  enum TargetKind {
    TK_Return,
    TK_Sink,
    TK_CallInput,
    TK_SourceWrapper,
    TK_DirectSource,
  };
  struct DagTarget {
    TargetKind Kind;
    size_t NodeIdx;
    const GuardedValueFlowSite *Site;
    InputSummary *CalleeInSmry = nullptr;
  };

  std::vector<const GuardedValueFlowNode *> Nodes;
  std::unordered_map<const GuardedValueFlowNode *, size_t> NodeId;
  std::vector<DagEdge> Edges;
  std::set<std::tuple<size_t, size_t, const GuardedValueFlowCallSite *,
                      OutputSummary *>>
      SeenEdges;
  std::vector<DagTarget> Targets;
  std::set<std::tuple<int, size_t, const GuardedValueFlowSite *, InputSummary *>>
      SeenTargets;

  auto getOrAddNode = [&](const GuardedValueFlowNode *N) -> size_t {
    auto It = NodeId.find(N);
    if (It != NodeId.end())
      return It->second;
    size_t Id = Nodes.size();
    Nodes.push_back(N);
    NodeId.emplace(N, Id);
    return Id;
  };

  auto addEdge = [&](size_t From, size_t To,
                     const GuardedValueFlowCallSite *CS,
                     OutputSummary *OutSmry) {
    if (SeenEdges.emplace(From, To, CS, OutSmry).second)
      Edges.push_back(DagEdge{From, To, CS, OutSmry});
  };

  auto addTarget = [&](TargetKind Kind, size_t NIdx,
                       const GuardedValueFlowSite *Site,
                       InputSummary *InSmry = nullptr) {
    if (SeenTargets.emplace(static_cast<int>(Kind), NIdx, Site, InSmry).second)
      Targets.push_back(DagTarget{Kind, NIdx, Site, InSmry});
  };

  const bool IsSinkFunc =
      std::static_pointer_cast<TaintExtras>(TSV)->isSinkFunction(
          Graph, dyn_cast<GuardedValueFlowArgumentNode>(ArgNode));

  getOrAddNode(ArgNode);
  for (size_t QueueIdx = 0; QueueIdx < Nodes.size(); ++QueueIdx) {
    const GuardedValueFlowNode *U = Nodes[QueueIdx];
    TraceBuilder.push();
    if (TraceBuilder.recentObj() != U)
      TraceBuilder.add(U);

    for (const GuardedValueFlowSite *UseSite : U->useSites()) {
      if (!UseSite)
        continue;
      if (const auto *RN = dyn_cast<GuardedValueFlowReturnNode>(U)) {
        bool MatchesAnyIncoming = std::any_of(
            RN->incomingReturns().begin(), RN->incomingReturns().end(),
            [&](const GuardedValueFlowReturnNode::ReturnIncoming &Inc) {
              return Inc.site == UseSite;
            });
        if (!MatchesAnyIncoming)
          continue;
      }

      auto UTy = TSV->checkSite(UseSite, TraceBuilder);
      if (UTy == Vulnerability::ST_Others)
        continue;
      if (UTy == Vulnerability::ST_Call &&
          !hasSummary(dyn_cast<GuardedValueFlowCallSite>(UseSite), U,
                      IsCommonArgSrc)) {
        continue;
      }

      if (UTy & Vulnerability::ST_Return)
        addTarget(TK_Return, QueueIdx, UseSite);
      if (UTy & Vulnerability::ST_Sink)
        addTarget(TK_Sink, QueueIdx, UseSite);
      if (UTy & Vulnerability::ST_Call) {
        const auto *GraphCS = cast<GuardedValueFlowCallSite>(UseSite);
        if (!IsSinkFunc) {
          auto InIt = InputSmryMap.find(std::make_pair(U, GraphCS));
          if (InIt != InputSmryMap.end()) {
            for (InputSummary *InSmry : InIt->second) {
              if (InSmry->getInlineDepth() < GSAFOptions::InlineDepth)
                addTarget(TK_CallInput, QueueIdx, GraphCS, InSmry);
            }
          }
        }
        auto SWIt = SourceWrapperSmryMap.find(GraphCS);
        if (SWIt != SourceWrapperSmryMap.end()) {
          auto NodeIt = SWIt->second.find(U);
          if (NodeIt != SWIt->second.end()) {
            for (InputSummary *WrapSmry : NodeIt->second) {
              if (WrapSmry->getInlineDepth() < GSAFOptions::InlineDepth)
                addTarget(TK_SourceWrapper, QueueIdx, GraphCS, WrapSmry);
            }
          }
        }
        auto SrcIt = Sources.find(GraphCS);
        if (SrcIt != Sources.end() && SrcIt->second.count(U))
          addTarget(TK_DirectSource, QueueIdx, GraphCS);

        for (Function *Callee : GraphCS->getCallees()) {
          for (const GuardedValueFlowCallOutputNode *OutputNode :
               callOutputs(GraphCS, Callee)) {
            if (RelevantNodesBuilt && !ReachSet.count(OutputNode))
              continue;
            auto OutIt =
                OutputSmryMap.find(std::make_tuple(U, OutputNode, Callee));
            if (OutIt == OutputSmryMap.end())
              continue;
            for (OutputSummary *OutSmry : OutIt->second) {
              if (OutSmry->getInlineDepth() >= GSAFOptions::InlineDepth)
                continue;
              if (TSV->checkNode(OutputNode, TraceBuilder))
                continue;
              size_t ToIdx = getOrAddNode(OutputNode);
              addEdge(QueueIdx, ToIdx, GraphCS, OutSmry);
            }
          }
        }
      }
    }

    auto FlowParents = valueFlowParents(U, GSAFOptions::EnableArithmeticFlow);
    for (const GuardedValueFlowNode *P : FlowParents) {
      if (!P)
        continue;
      if (RelevantNodesBuilt && !ReachSet.count(P))
        continue;
      if (TSV->checkNode(P, TraceBuilder))
        continue;
      size_t ToIdx = getOrAddNode(P);
      addEdge(QueueIdx, ToIdx, nullptr, nullptr);
    }

    TraceBuilder.pop();
  }

  if (Targets.empty())
    return true;

  auto computeSCC = [](size_t N, const std::vector<std::vector<size_t>> &Adj,
                       std::vector<size_t> &SccId,
                       std::vector<size_t> &SccSizes) {
    SccId.assign(N, static_cast<size_t>(-1));
    SccSizes.clear();
    std::vector<size_t> Disc(N, 0), Low(N, 0), Stack;
    std::vector<bool> OnStack(N, false);
    size_t TimerVal = 0;

    struct Frame {
      size_t U;
      size_t NextEdge;
    };
    std::vector<Frame> CallStack;
    for (size_t Start = 0; Start < N; ++Start) {
      if (Disc[Start] != 0)
        continue;
      Disc[Start] = Low[Start] = ++TimerVal;
      Stack.push_back(Start);
      OnStack[Start] = true;
      CallStack.push_back({Start, 0});

      while (!CallStack.empty()) {
        auto &Top = CallStack.back();
        size_t U = Top.U;
        if (Top.NextEdge < Adj[U].size()) {
          size_t V = Adj[U][Top.NextEdge++];
          if (Disc[V] == 0) {
            Disc[V] = Low[V] = ++TimerVal;
            Stack.push_back(V);
            OnStack[V] = true;
            CallStack.push_back({V, 0});
          } else if (OnStack[V]) {
            Low[U] = std::min(Low[U], Disc[V]);
          }
        } else {
          if (Low[U] == Disc[U]) {
            size_t Id = SccSizes.size();
            size_t Count = 0;
            while (true) {
              size_t W = Stack.back();
              Stack.pop_back();
              OnStack[W] = false;
              SccId[W] = Id;
              ++Count;
              if (W == U)
                break;
            }
            SccSizes.push_back(Count);
          }
          CallStack.pop_back();
          if (!CallStack.empty()) {
            size_t Parent = CallStack.back().U;
            Low[Parent] = std::min(Low[Parent], Low[U]);
          }
        }
      }
    }
  };

  // Trigger structurally when the argument's reachable graph has a cycle or
  // > 64 distinct paths (where path-by-path DFS explodes exponentially).
  std::vector<std::vector<size_t>> FullAdj(Nodes.size());
  bool HasSelfLoop = false;
  for (const auto &E : Edges) {
    FullAdj[E.From].push_back(E.To);
    if (E.From == E.To)
      HasSelfLoop = true;
  }
  std::vector<size_t> FullSccId, FullSccSizes;
  computeSCC(Nodes.size(), FullAdj, FullSccId, FullSccSizes);
  bool HasCycle = HasSelfLoop;
  for (size_t Sz : FullSccSizes) {
    if (Sz > 1) {
      HasCycle = true;
      break;
    }
  }
  if (!HasCycle) {
    // Tarjan assigns SCC IDs in reverse topological order; reverse iterate.
    std::vector<std::vector<size_t>> SccNodes(FullSccSizes.size());
    for (size_t I = 0; I < Nodes.size(); ++I)
      SccNodes[FullSccId[I]].push_back(I);
    std::vector<uint64_t> PathCount(Nodes.size(), 0);
    PathCount[0] = 1;
    for (size_t S = SccNodes.size(); S-- > 0;) {
      for (size_t U : SccNodes[S]) {
        if (PathCount[U] == 0)
          continue;
        for (size_t V : FullAdj[U]) {
          PathCount[V] = std::min<uint64_t>(1000, PathCount[V] + PathCount[U]);
        }
      }
    }
    uint64_t TotalPaths = 0;
    for (const auto &T : Targets) {
      TotalPaths = std::min<uint64_t>(1000, TotalPaths + PathCount[T.NodeIdx]);
    }
    if (TotalPaths <= 64)
      return false;
  }

  // Build incoming edge lists for backward reachability filtering per target.
  std::vector<std::vector<size_t>> InEdges(Nodes.size());
  for (size_t EIdx = 0; EIdx < Edges.size(); ++EIdx)
    InEdges[Edges[EIdx].To].push_back(EIdx);

  auto appendFormalActualEqs = [&](SummaryBase *CalleeSmry,
                                   const GuardedValueFlowCallSite *GraphCS,
                                   const std::string &RenameSuffix,
                                   SMTExprVec &DestEqs) {
    auto *CalleeGraph = Parent->getGraph(CalleeSmry->getFunction());
    if (!CalleeGraph)
      return;
    for (auto *Formal : CalleeSmry->getInputs()) {
      const std::string &FormalDesc = detail::encodingSymbol(Formal);
      SMTExpr FormalExpr = Fctry->createBitVecConst(
          FormalDesc + RenameSuffix,
          detail::encodingTypeSize(Parent->getModule()->getDataLayout(),
                                   Formal->getType()));
      if (nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                     GuardedValueFlowNode>(Formal)) {
        unsigned VarStart = CalleeGraph->getNumCommonArgument();
        unsigned VarEnd = GraphCS->getCommonInputs().size();
        if (VarStart < VarEnd) {
          SMTExpr Mapping = Fctry->createEmptySMTExpr();
          bool HasMapping = false;
          for (unsigned Idx = VarStart; Idx < VarEnd; ++Idx) {
            auto *ActualNode = GraphCS->getCommonInputs()[Idx];
            if (!ActualNode)
              continue;
            SMTExpr ActualExpr = Solver->getOrInsertExpr(ActualNode);
            if (!ActualExpr.isBitVector() ||
                ActualExpr.getBitVecSize() != FormalExpr.getBitVecSize())
              continue;
            SMTExpr Eq = (FormalExpr == ActualExpr);
            Mapping = HasMapping ? (Mapping || Eq) : Eq;
            HasMapping = true;
            Solver->addAll(Solver->getDataDeps(ActualNode));
          }
          if (HasMapping)
            DestEqs.push_back(Mapping);
        }
      } else {
        const GuardedValueFlowNode *ActualNode = nullptr;
        if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                       GuardedValueFlowNode>(Formal)) {
          if (Formal->getIndex() < GraphCS->getCommonInputs().size())
            ActualNode = GraphCS->getCommonInput(Formal->getIndex());
        } else {
          ActualNode = GraphCS->getPseudoInput(CalleeSmry->getFunction(),
                                               Formal->getIndex());
        }
        if (!ActualNode)
          continue;
        SMTExpr ActualExpr = Solver->getOrInsertExpr(ActualNode);
        if (FormalExpr.getBitVecSize() != ActualExpr.getBitVecSize())
          continue;
        DestEqs.push_back(FormalExpr == ActualExpr);
        Solver->addAll(Solver->getDataDeps(ActualNode));
      }
    }
  };

  for (const auto &T : Targets) {
    TimeChecker->check();
    const GuardedValueFlowNode *TargetNode = Nodes[T.NodeIdx];
    const auto *TargetRetNode =
        (T.Kind == TK_Return)
            ? dyn_cast<GuardedValueFlowReturnNode>(TargetNode)
            : nullptr;

    auto edgeAllowedIntoTarget = [&](size_t EIdx) -> bool {
      const auto &E = Edges[EIdx];
      if (E.To != T.NodeIdx || !TargetRetNode)
        return true;
      if (E.CS != nullptr)
        return false;
      const GuardedValueFlowNode *FromNode = Nodes[E.From];
      return std::any_of(
          TargetRetNode->incomingReturns().begin(),
          TargetRetNode->incomingReturns().end(),
          [&](const GuardedValueFlowReturnNode::ReturnIncoming &Inc) {
            return Inc.value == FromNode && Inc.site == T.Site;
          });
    };

    // Backward BFS from T.NodeIdx to keep only nodes/edges on ArgNode -> T.
    std::vector<bool> CanReachTarget(Nodes.size(), false);
    std::vector<size_t> Work;
    CanReachTarget[T.NodeIdx] = true;
    Work.push_back(T.NodeIdx);
    while (!Work.empty()) {
      size_t Cur = Work.back();
      Work.pop_back();
      for (size_t EIdx : InEdges[Cur]) {
        if (!edgeAllowedIntoTarget(EIdx))
          continue;
        size_t Pred = Edges[EIdx].From;
        if (!CanReachTarget[Pred]) {
          CanReachTarget[Pred] = true;
          Work.push_back(Pred);
        }
      }
    }
    if (!CanReachTarget[0])
      continue;

    std::vector<size_t> RelNodes;
    std::vector<size_t> GlobalToLocal(Nodes.size(), static_cast<size_t>(-1));
    for (size_t I = 0; I < Nodes.size(); ++I) {
      if (CanReachTarget[I]) {
        GlobalToLocal[I] = RelNodes.size();
        RelNodes.push_back(I);
      }
    }
    const size_t LocalArg = GlobalToLocal[0];
    const size_t LocalTarget = GlobalToLocal[T.NodeIdx];

    std::vector<size_t> RelEdgeGlobal;
    std::vector<std::vector<size_t>> RelAdj(RelNodes.size());
    std::vector<std::vector<size_t>> RelInEdges(RelNodes.size());
    for (size_t EIdx = 0; EIdx < Edges.size(); ++EIdx) {
      const auto &E = Edges[EIdx];
      if (!CanReachTarget[E.From] || !CanReachTarget[E.To])
        continue;
      if (E.From == E.To)
        continue; // Self-loops cannot appear in any simple path.
      if (!edgeAllowedIntoTarget(EIdx))
        continue;
      size_t LU = GlobalToLocal[E.From];
      size_t LV = GlobalToLocal[E.To];
      size_t LocalEIdx = RelEdgeGlobal.size();
      RelEdgeGlobal.push_back(EIdx);
      RelAdj[LU].push_back(LV);
      RelInEdges[LV].push_back(LocalEIdx);
    }

    std::vector<size_t> LocalSccId, LocalSccSizes;
    computeSCC(RelNodes.size(), RelAdj, LocalSccId, LocalSccSizes);
    size_t MaxSccSize = 1;
    for (size_t Sz : LocalSccSizes)
      MaxSccSize = std::max(MaxSccSize, Sz);
    unsigned RankBits = 2;
    while ((1ULL << RankBits) <= MaxSccSize)
      ++RankBits;

    Solver->push();
    pushState();
    const unsigned Tag = ++DagEncodeCounter;

    // Ensure ArgNode's expression is registered in UsedFunctionArguments.
    (void)Solver->getOrInsertExpr(ArgNode);

    std::vector<SMTExpr> ReachExpr;
    std::vector<SMTExpr> RankExpr;
    ReachExpr.reserve(RelNodes.size());
    RankExpr.reserve(RelNodes.size());
    for (size_t LI = 0; LI < RelNodes.size(); ++LI) {
      if (LI == LocalArg) {
        ReachExpr.push_back(Fctry->createBoolVal(true));
      } else {
        ReachExpr.push_back(Fctry->createBoolConst(
            "dag_r_" + std::to_string(Tag) + "_" + std::to_string(LI)));
      }
      if (LocalSccSizes[LocalSccId[LI]] > 1) {
        RankExpr.push_back(Fctry->createBitVecConst(
            "dag_k_" + std::to_string(Tag) + "_" + std::to_string(LI),
            RankBits));
      } else {
        RankExpr.push_back(Fctry->createEmptySMTExpr());
      }
    }

    std::vector<SMTExpr> EdgeBranchExpr(RelEdgeGlobal.size(),
                                        Fctry->createBoolVal(false));

    for (size_t LV = 0; LV < RelNodes.size(); ++LV) {
      if (LV == LocalArg)
        continue;
      const GuardedValueFlowNode *V = Nodes[RelNodes[LV]];
      SMTExprVec InDisj = Fctry->createEmptySMTExprVec();

      for (size_t LocalEIdx : RelInEdges[LV]) {
        const auto &E = Edges[RelEdgeGlobal[LocalEIdx]];
        size_t LU = GlobalToLocal[E.From];
        const GuardedValueFlowNode *U = Nodes[E.From];

        SMTExprVec EdgeConj = Fctry->createEmptySMTExprVec();
        if (!E.CS) {
          auto Pair = Solver->getDepsPair(V, U);
          const bool IsSelect =
              isa<GuardedValueFlowOpcodeNode>(V) &&
              cast<GuardedValueFlowOpcodeNode>(V)->getOpcodeKind() ==
                  GuardedValueFlowOpcodeNode::OpcodeKind::Select;
          const bool HasEdgeEq =
              IsSelect || (!isa<GuardedValueFlowOpcodeNode>(V) &&
                           V->getKind() != GuardedValueFlowNode::Kind::Unknown);

          Z3_ast EdgeEqAst = nullptr;
          if (HasEdgeEq) {
            SMTExpr EdgeEq = alignAndEqualExpr(
                *Fctry, Solver->getOrInsertExpr(V), Solver->getOrInsertExpr(U));
            EdgeEqAst = static_cast<Z3_ast>(EdgeEq.getZ3Expr());
            EdgeConj.push_back(EdgeEq);
          }
          for (unsigned I = 0; I < Pair.first.size(); ++I)
            EdgeConj.push_back(Pair.first[I]);
          for (unsigned I = 0; I < Pair.second.size(); ++I) {
            SMTExpr D = Pair.second[I];
            if (HasEdgeEq && static_cast<Z3_ast>(D.getZ3Expr()) == EdgeEqAst)
              continue;
            Solver->add(D);
          }
          if (TargetRetNode && LV == LocalTarget &&
              U->getParentBasicBlock() != T.Site->getParentBasicBlock()) {
            auto RetDeps = ctrlDepsPair(*Solver, T.Site->getParentBasicBlock(),
                                        Graph, U->getParentBasicBlock());
            for (unsigned I = 0; I < RetDeps.first.size(); ++I)
              EdgeConj.push_back(RetDeps.first[I]);
            Solver->addAll(RetDeps.second);
          }
        } else {
          // Interprocedural OutputSummary edge: U -> CallSiteOutput via E.CS.
          const auto *CSO = cast<GuardedValueFlowCallOutputNode>(V);
          if (E.CS->getParentBasicBlock() != U->getParentBasicBlock()) {
            auto Deps = ctrlDepsPair(*Solver, E.CS->getParentBasicBlock(),
                                     Graph, U->getParentBasicBlock());
            for (unsigned I = 0; I < Deps.first.size(); ++I)
              EdgeConj.push_back(Deps.first[I]);
            Solver->addAll(Deps.second);
          }
          TraceBuilder.push();
          if (TraceBuilder.recentObj() != U)
            TraceBuilder.add(U);
          SMTExprVec CallPrereq = Fctry->createEmptySMTExprVec();
          TSV->setPrerequisites(Solver, E.CS, TraceBuilder, CallPrereq);
          TraceBuilder.pop();
          for (unsigned I = 0; I < CallPrereq.size(); ++I)
            EdgeConj.push_back(CallPrereq[I]);

          std::string Suffix = format_str("_CS%p", E.CS->getInstruction());
          appendFormalActualEqs(E.OutSmry, E.CS, Suffix, EdgeConj);
          if (auto *RetNode = E.OutSmry->getReturnNode()) {
            SMTExpr CSOExpr = Solver->getOrInsertExpr(CSO);
            SMTExpr RetExpr = Fctry->createBitVecConst(
                detail::encodingSymbol(RetNode) + Suffix,
                CSOExpr.getBitVecSize());
            EdgeConj.push_back(RetExpr == CSOExpr);
          }
          for (const SummaryCacheItem &Item : E.OutSmry->getNonSymDepsCache()) {
            SMTExpr Renamed = getTranslatedRenamedSummary(SummaryCacheItem(
                Item.constraints, Item.suffix + Suffix, Item.depth + 1));
            EdgeConj.push_back(Renamed);
          }
          for (const SummaryCacheItem &Item : E.OutSmry->getSymbDepsCache()) {
            SMTExpr Renamed = getTranslatedRenamedSummary(SummaryCacheItem(
                Item.constraints, Item.suffix + Suffix, Item.depth + 1));
            EdgeConj.push_back(Renamed);
          }
        }

        if (LocalSccId[LU] == LocalSccId[LV])
          EdgeConj.push_back(RankExpr[LU].basic_ult(RankExpr[LV]));

        SMTExpr EdgeCond = EdgeConj.toAndExpr();
        SMTExpr Branch =
            (LU == LocalArg) ? EdgeCond : (ReachExpr[LU] && EdgeCond);
        EdgeBranchExpr[LocalEIdx] = Branch;
        InDisj.push_back(Branch);
      }

      if (InDisj.empty())
        Solver->add(!ReachExpr[LV]);
      else
        Solver->add((!ReachExpr[LV]) || InDisj.toOrExpr());
    }

    if (LocalTarget != LocalArg)
      Solver->add(ReachExpr[LocalTarget]);

    if (!TargetRetNode &&
        T.Site->getParentBasicBlock() != TargetNode->getParentBasicBlock()) {
      auto Deps = ctrlDepsPair(*Solver, T.Site->getParentBasicBlock(), Graph,
                               TargetNode->getParentBasicBlock());
      Solver->addAll(Deps.first);
      Solver->addAll(Deps.second);
    }

    inlineReturnSymbolicSummary(Src);

    TraceBuilder.push();
    if (TraceBuilder.recentObj() != TargetNode)
      TraceBuilder.add(TargetNode);
    SMTExprVec Prereq = Fctry->createEmptySMTExprVec();
    TSV->setPrerequisites(Solver, T.Site, TraceBuilder, Prereq);
    TraceBuilder.pop();
    Solver->addAll(Prereq);

    unsigned BaseDepth = 0;
    if (T.Kind == TK_CallInput || T.Kind == TK_SourceWrapper) {
      const auto *CS = cast<GuardedValueFlowCallSite>(T.Site);
      std::string Suffix = format_str("_CS%p", CS->getInstruction());
      for (const SummaryCacheItem &Item :
           T.CalleeInSmry->getNonSymDepsCache()) {
        NonSymDepsCache.push_back(SummaryCacheItem(
            Item.constraints, Item.suffix + Suffix, Item.depth + 1));
      }
      for (const SummaryCacheItem &Item : T.CalleeInSmry->getSymbDepsCache()) {
        SymbDepsCache.push_back(SummaryCacheItem(
            Item.constraints, Item.suffix + Suffix, Item.depth + 1));
      }
      matchFormalActual(T.CalleeInSmry, CS, Suffix);
      inlineReturnSymbolicSummary(Src);
      BaseDepth = T.CalleeInSmry->getInlineDepth() + 1;
    }

    auto DagRes = Solver->check();
    if (DagRes == SMTSolver::SMTRT_Unknown) {
      Solver->pop();
      popState();
      TimeChecker->checkNow();
      continue;
    }
    if (DagRes == SMTSolver::SMTRT_Sat) {
      // Extract a concrete verified simple path from the Z3 model.
      SMTModel Model = Solver->getSMTModel();
      std::vector<size_t> WitnessEdges;
      std::vector<bool> SeenInWitness(RelNodes.size(), false);
      size_t Cur = LocalTarget;
      SeenInWitness[Cur] = true;
      bool Extracted = (Cur == LocalArg);

      while (Cur != LocalArg) {
        size_t ChosenEdge = static_cast<size_t>(-1);
        for (size_t LocalEIdx : RelInEdges[Cur]) {
          size_t LU = GlobalToLocal[Edges[RelEdgeGlobal[LocalEIdx]].From];
          if (SeenInWitness[LU])
            continue;
          auto Eval = Model.getBoolValue(EdgeBranchExpr[LocalEIdx], true);
          if (Eval.first && Eval.second) {
            ChosenEdge = LocalEIdx;
            break;
          }
        }
        if (ChosenEdge == static_cast<size_t>(-1))
          break;
        WitnessEdges.push_back(ChosenEdge);
        Cur = GlobalToLocal[Edges[RelEdgeGlobal[ChosenEdge]].From];
        SeenInWitness[Cur] = true;
        if (Cur == LocalArg)
          Extracted = true;
      }

      if (!Extracted) {
        // Defensive fallback: shortest structural path in (RelNodes, RelInEdges).
        WitnessEdges.clear();
        std::vector<size_t> PrevNode(RelNodes.size(), static_cast<size_t>(-1));
        std::vector<size_t> PrevEdge(RelNodes.size(), static_cast<size_t>(-1));
        std::vector<size_t> Q{LocalArg};
        PrevNode[LocalArg] = LocalArg;
        for (size_t QI = 0; QI < Q.size() && PrevNode[LocalTarget] == static_cast<size_t>(-1);
             ++QI) {
          size_t U = Q[QI];
          for (size_t LocalEIdx = 0; LocalEIdx < RelEdgeGlobal.size(); ++LocalEIdx) {
            const auto &E = Edges[RelEdgeGlobal[LocalEIdx]];
            if (GlobalToLocal[E.From] != U)
              continue;
            size_t V = GlobalToLocal[E.To];
            if (PrevNode[V] == static_cast<size_t>(-1)) {
              PrevNode[V] = U;
              PrevEdge[V] = LocalEIdx;
              Q.push_back(V);
            }
          }
        }
        if (PrevNode[LocalTarget] != static_cast<size_t>(-1)) {
          for (size_t C = LocalTarget; C != LocalArg; C = PrevNode[C])
            WitnessEdges.push_back(PrevEdge[C]);
          Extracted = true;
        }
      }

      if (Extracted) {
        std::reverse(WitnessEdges.begin(), WitnessEdges.end());
        unsigned SummaryDepth = BaseDepth;
        TraceBuilder.push();
        for (size_t LocalEIdx : WitnessEdges) {
          const auto &E = Edges[RelEdgeGlobal[LocalEIdx]];
          const GuardedValueFlowNode *ToNode = Nodes[E.To];
          if (E.CS && E.OutSmry) {
            SummaryDepth = std::max<unsigned>(
                SummaryDepth, E.OutSmry->getInlineDepth() + 1);
            if (isa<GuardedValueFlowArgumentNode>(E.OutSmry->getSourceNode()) &&
                !E.OutSmry->getSourceSite()) {
              TraceBuilder.add(E.CS);
            }
            TraceBuilder.add(E.OutSmry->getTrace());
          }
          if (TraceBuilder.recentObj() != ToNode)
            TraceBuilder.add(ToNode);
        }
        if (TraceBuilder.recentObj() != TargetNode)
          TraceBuilder.add(TargetNode);

        if (T.Kind == TK_Return) {
          TraceBuilder.add(T.Site);
          OutputSummary *OutSmry = createOutputSummary(
              Solver->assertions(), TraceBuilder.snapshot(),
              &Solver->getUsedFunctionArguments(), SummaryDepth);
          addCachedConstraintsToSummary(OutSmry);
          Smry->OutSmrys[TargetNode].insert(OutSmry);
        } else if (T.Kind == TK_Sink) {
          TraceBuilder.add(T.Site);
          InputSummary *InSmry = createInputSummary(
              Solver->assertions(), TraceBuilder.snapshot(),
              &Solver->getUsedFunctionArguments(), SummaryDepth);
          addCachedConstraintsToSummary(InSmry);
          Smry->InSmrys[ArgNode].insert(InSmry);
        } else if (T.Kind == TK_CallInput) {
          TraceBuilder.add(T.Site);
          TraceBuilder.add(T.CalleeInSmry->getTrace());
          InputSummary *InSmry = createInputSummary(
              Solver->assertions(), TraceBuilder.snapshot(),
              &Solver->getUsedFunctionArguments(), SummaryDepth);
          addCachedConstraintsToSummary(InSmry);
          Smry->InSmrys[ArgNode].insert(InSmry);
        } else if (T.Kind == TK_SourceWrapper) {
          TraceBuilder.add(T.Site);
          TraceBuilder.add(T.CalleeInSmry->getTrace());
          InputSummary *InSmry = createInputSummary(
              Solver->assertions(), TraceBuilder.snapshot(),
              &Solver->getUsedFunctionArguments(), SummaryDepth);
          addCachedConstraintsToSummary(InSmry);
          Smry->TaintSourceWrapperSmrys[ArgNode].insert(InSmry);
        } else {
          assert(T.Kind == TK_DirectSource);
          TraceBuilder.add(T.Site);
          InputSummary *InSmry = createInputSummary(
              Solver->assertions(), TraceBuilder.snapshot(),
              &Solver->getUsedFunctionArguments(), SummaryDepth);
          addCachedConstraintsToSummary(InSmry);
          Smry->TaintSourceWrapperSmrys[ArgNode].insert(InSmry);
        }
        TraceBuilder.pop();
      }
    }

    Solver->pop();
    popState();
  }

  return true;
}

FunctionAnalyzer::ForwardVisitState
FunctionAnalyzer::captureForwardState(unsigned InlineDepth) const {
  ForwardVisitState State;
  State.InlineDepth = InlineDepth;
  const auto &Base = Solver->pendingAssertions();
  if (Base.size() > ForwardRootBaseCount) {
    State.ExtraBaseAsts.reserve(Base.size() - ForwardRootBaseCount);
    for (size_t I = ForwardRootBaseCount; I < Base.size(); ++I) {
      State.ExtraBaseAsts.push_back(static_cast<Z3_ast>(
          const_cast<SMTExpr &>(Base[I]).getZ3Expr()));
    }
    std::sort(State.ExtraBaseAsts.begin(), State.ExtraBaseAsts.end());
    State.ExtraBaseAsts.erase(
        std::unique(State.ExtraBaseAsts.begin(), State.ExtraBaseAsts.end()),
        State.ExtraBaseAsts.end());
  }
  const auto &NonSym = NonSymDepsCache.getCacheVector();
  const auto &Symb = SymbDepsCache.getCacheVector();
  if (NonSym.size() > ForwardRootNonSymCount ||
      Symb.size() > ForwardRootSymbCount) {
    State.ExtraDeferred.reserve(
        (NonSym.size() > ForwardRootNonSymCount
             ? NonSym.size() - ForwardRootNonSymCount
             : 0) +
        (Symb.size() > ForwardRootSymbCount
             ? Symb.size() - ForwardRootSymbCount
             : 0));
    SummaryBase::SMTReadLock();
    for (size_t I = ForwardRootNonSymCount; I < NonSym.size(); ++I) {
      if (!NonSym[I].constraints)
        continue;
      if (NonSym[I].constraints->isApp() && NonSym[I].constraints->isTrue())
        continue;
      State.ExtraDeferred.emplace_back(
          false, static_cast<Z3_ast>(NonSym[I].constraints->getZ3Expr()),
          NonSym[I].suffix);
    }
    for (size_t I = ForwardRootSymbCount; I < Symb.size(); ++I) {
      if (!Symb[I].constraints)
        continue;
      if (Symb[I].constraints->isApp() && Symb[I].constraints->isTrue())
        continue;
      State.ExtraDeferred.emplace_back(
          true, static_cast<Z3_ast>(Symb[I].constraints->getZ3Expr()),
          Symb[I].suffix);
    }
    SummaryBase::SMTReadUnlock();
    std::sort(State.ExtraDeferred.begin(), State.ExtraDeferred.end());
    State.ExtraDeferred.erase(
        std::unique(State.ExtraDeferred.begin(), State.ExtraDeferred.end()),
        State.ExtraDeferred.end());
  }
  return State;
}

bool FunctionAnalyzer::forwardStateSubsumes(const ForwardVisitState &Earlier,
                                            const ForwardVisitState &Current,
                                            bool StrictOnly) const {
  if (Earlier.InlineDepth > Current.InlineDepth)
    return false;
  if (Earlier.ExtraBaseAsts.size() > Current.ExtraBaseAsts.size())
    return false;
  if (Earlier.ExtraDeferred.size() > Current.ExtraDeferred.size())
    return false;
  for (Z3_ast Ast : Earlier.ExtraBaseAsts) {
    if (!Solver->containsAssertion(Ast))
      return false;
  }
  if (!std::includes(Current.ExtraDeferred.begin(), Current.ExtraDeferred.end(),
                     Earlier.ExtraDeferred.begin(),
                     Earlier.ExtraDeferred.end())) {
    return false;
  }
  if (!StrictOnly)
    return true;
  return Earlier.InlineDepth < Current.InlineDepth ||
         Earlier.ExtraBaseAsts.size() < Current.ExtraBaseAsts.size() ||
         Earlier.ExtraDeferred.size() < Current.ExtraDeferred.size();
}

bool FunctionAnalyzer::forwardStateEquals(const ForwardVisitState &A,
                                          const ForwardVisitState &B) const {
  return A.InlineDepth == B.InlineDepth &&
         A.ExtraBaseAsts == B.ExtraBaseAsts &&
         A.ExtraDeferred == B.ExtraDeferred;
}

void FunctionAnalyzer::runForwardSearch(const GuardedValueFlowNode *StartNode,
                                        Vulnerability::ValueSitePairType Src,
                                        unsigned InlineDepth) {
  ForwardBottomUpVisited.clear();
  ForwardCSOPlanned.clear();
  ForwardRootBaseCount = Solver->pendingAssertions().size();
  ForwardRootNonSymCount = NonSymDepsCache.size();
  ForwardRootSymbCount = SymbDepsCache.size();
  ForwardSearchSubsumptionActive = true;
  auto guard = make_scope_exit([&] {
    ForwardSearchSubsumptionActive = false;
    ForwardBottomUpVisited.clear();
    ForwardCSOPlanned.clear();
  });

  // Pass 1: explore the intraprocedural downward DAG first so all direct
  // bottomUpDepthFirstSearch visits and minimal CallSiteOutput states are
  // recorded before expanding callee OutputSummary chains.
  pushState();
  TraceBuilder.push();
  Solver->push();
  search(StartNode, nullptr, Src, InlineDepth, /*AllowCSOInline=*/false);
  Solver->pop();
  TraceBuilder.pop();
  popState();

  if (allGoalsReportedForCurrentSrc() || ForwardCSOPlanned.empty())
    return;

  // Pass 2: expand planned CallSiteOutput summaries while skipping any revisit
  // subsumed by an earlier strictly weaker or already-explored state.
  search(StartNode, nullptr, Src, InlineDepth, /*AllowCSOInline=*/true);
}

// General search function that can handle both forward and backward traversal.
// Node: Current node.
// PrevNode: Previous node in the traversal.
// Src: The source of the taint being tracked.
// InlineDepth: Current inline depth.
void FunctionAnalyzer::search(const GuardedValueFlowNode *Node,
                              const GuardedValueFlowNode *PrevNode,
                              Vulnerability::ValueSitePairType Src,
                              unsigned InlineDepth, bool AllowCSOInline) {
  if (allGoalsReportedForCurrentSrc())
    return;
  if (UnsatBlocks.contains(Node->getParentBasicBlock()))
    return;
  auto key = std::make_tuple(Node, Src.first, Src.second);
  if (!ActiveForwardSearch.insert(key).second)
    return;
  auto active_guard =
      make_scope_exit([&] { ActiveForwardSearch.erase(key); });

  DEBUG_TRACE(dbgs() << "\t >Visiting[dw]: " << *Node << "...\n");
  if (TSV->checkNode(Node, TraceBuilder)) {
    DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *Node
                       << " by filtering nodes...\n");
    return;
  }

  if (PrevNode) {
    auto DepsCond = Solver->getDepsPair(PrevNode, Node);
    bool AddedDeps = !DepsCond.first.empty() || !DepsCond.second.empty();
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
    const bool BlockChanged =
        Node->getParentBasicBlock() != PrevNode->getParentBasicBlock();
    if (BlockChanged) {
      auto NodeCtrl = Solver->getCtrlDeps(Node);
      if (!NodeCtrl.empty())
        AddedDeps = true;
      Solver->addAll(NodeCtrl);
    }
    if (AddedDeps &&
        (BlockChanged ||
         PrevNode->getKind() == GuardedValueFlowNode::Kind::LoadMemory ||
         isa<GuardedValueFlowPhiNode>(PrevNode))) {
      if (Solver->check() == SMTSolver::SMTRT_Unsat)
        return;
    }
  }
  inlineReturnSymbolicSummary(Src);
  TraceBuilder.add(Node);

  bool ShouldRunBottomUp = true;
  ForwardVisitState CurState;
  bool HasCurState = false;
  if (ForwardSearchSubsumptionActive) {
    CurState = captureForwardState(InlineDepth);
    HasCurState = true;
    auto &Visited = ForwardBottomUpVisited[Node];
    for (const auto &Earlier : Visited) {
      if (forwardStateSubsumes(Earlier, CurState, /*StrictOnly=*/false)) {
        ShouldRunBottomUp = false;
        break;
      }
    }
    if (ShouldRunBottomUp && Visited.size() < 64)
      Visited.push_back(CurState);
  }

  // search up
  if (ShouldRunBottomUp) {
    pushState();
    TraceBuilder.push();
    Solver->push();

    bottomUpDepthFirstSearch(Node, PrevNode, Src, InlineDepth);

    Solver->pop();
    TraceBuilder.pop();
    popState();
  }

  if (allGoalsReportedForCurrentSrc())
    return;

  if (const GuardedValueFlowCallOutputNode *CSO =
          dyn_cast<GuardedValueFlowCallOutputNode>(Node)) {
    if (ForwardSearchSubsumptionActive) {
      if (!HasCurState) {
        CurState = captureForwardState(InlineDepth);
        HasCurState = true;
      }
      auto &Planned = ForwardCSOPlanned[CSO];
      if (!AllowCSOInline) {
        bool Subsumed = false;
        for (const auto &Earlier : Planned) {
          if (forwardStateSubsumes(Earlier, CurState, /*StrictOnly=*/false)) {
            Subsumed = true;
            break;
          }
        }
        if (!Subsumed && Planned.size() < 64) {
          CurState.Expanded = false;
          Planned.push_back(std::move(CurState));
        }
        return;
      }

      for (const auto &Earlier : Planned) {
        if (forwardStateSubsumes(Earlier, CurState, /*StrictOnly=*/true))
          return;
      }
      bool FoundEqual = false;
      for (auto &Earlier : Planned) {
        if (forwardStateEquals(Earlier, CurState)) {
          if (Earlier.Expanded)
            return;
          Earlier.Expanded = true;
          FoundEqual = true;
          break;
        }
      }
      if (!FoundEqual && Planned.size() < 64) {
        CurState.Expanded = true;
        Planned.push_back(std::move(CurState));
      }
    } else if (!AllowCSOInline) {
      return;
    }

    DEBUG_TRACE(dbgs() << "\t >It is a cso, inlining cso summaries...\n");

    // Using alias summary, continue to search down
    auto *GraphCS = callSite(CSO);
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;

      for (auto InputIt = GraphCS->input_begin(Callee),
                E = GraphCS->input_end(Callee);
           InputIt != E; ++InputIt) {
        auto *ActualNode = (*InputIt).InputNode;

        auto OutSmryIt =
            OutputSmryMap.find(std::make_tuple(ActualNode, CSO, Callee));
        if (OutSmryIt == OutputSmryMap.end()) {
          continue;
        }

        auto &OutSmryVec = OutSmryIt->second;
        for (auto *CalleeOutSmry : OutSmryVec) {
          inlineCalleeOutSummary(
              GraphCS, CalleeOutSmry, CSO, Src,
              std::max<unsigned>(CalleeOutSmry->getInlineDepth() + 1,
                                 InlineDepth + 1),
              1);
        }
      }
    }
  } else {
    // for each of Node's child
    //      search child
    std::set<const GuardedValueFlowNode *> VisitedChildren;
    for (size_t I = 0; I < Node->getNumChildren(); ++I) {
      auto *Child = Node->getChild(I);

      if (VisitedChildren.count(Child)) {
        // children may repeat
        continue;
      }

      if (isa<GuardedValueFlowRegionNode>(Child)) {
        continue;
      }

      if (const GuardedValueFlowOpcodeNode *Opcode =
              dyn_cast<GuardedValueFlowOpcodeNode>(Child)) {
        if (!(Opcode->getOpcodeKind() ==
              GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr) &&
            !(Opcode->getKind() == GuardedValueFlowNode::Kind::CastOpcode) &&
            !(Opcode->getOpcodeKind() ==
              GuardedValueFlowOpcodeNode::OpcodeKind::Select)) {
          continue;
        }
      }

      if (const GuardedValueFlowOpcodeNode *Opcode =
              dyn_cast<GuardedValueFlowOpcodeNode>(Node)) {
        if ((Opcode->getOpcodeKind() ==
             GuardedValueFlowOpcodeNode::OpcodeKind::Select) &&
            Opcode->getOperand(0) == Child) {
          continue;
        }

        if ((Opcode->getOpcodeKind() ==
             GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr) &&
            Opcode->getOperand(0) != Child) {
          continue;
        }
      }

      VisitedChildren.insert(Child);

      pushState();
      TraceBuilder.push();
      Solver->push();

      search(Child, Node, Src, InlineDepth, AllowCSOInline);

      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }
}

// Backward search to verify if a node can reach a source or sink under
// constraints.
void FunctionAnalyzer::bottomUpDepthFirstSearch(
    const GuardedValueFlowNode *Node, const GuardedValueFlowNode *PrevNode,
    Vulnerability::ValueSitePairType Src, unsigned InlineDepth) {
  if (allGoalsReportedForCurrentSrc())
    return;
  const bool IsCommonArgSrc =
      !Src.second && nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                                GuardedValueFlowNode>(Src.first);
  const auto &ReachSet =
      IsCommonArgSrc ? CanReachCommonArgSite : CanReachAnyRelevantSite;
  if (RelevantNodesBuilt && !ReachSet.count(Node))
    return;

  auto key = std::make_tuple(Node, Src.first, Src.second);
  if (!ActiveBackwardSearch.insert(key).second)
    return;
  auto active_guard =
      make_scope_exit([&] { ActiveBackwardSearch.erase(key); });

  TimeChecker->check();
  DEBUG_TRACE(dbgs() << "\t >Visiting[up]: " << *Node << "...\n");
  DEBUG_CONSTRAINTS(dbgs() << smtText(Solver->assertions()) << "\n");

  if (PrevNode && PrevNode->containsParent(Node)) {
    auto DepsCond = Solver->getDepsPair(Node, PrevNode);
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
  } else {
    auto DepsCond = Solver->getDepsPair(Node, nullptr);
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
  }
  inlineReturnSymbolicSummary(Src);

  if (TraceBuilder.recentObj() != Node) {
    TraceBuilder.add(Node);
  }

  bool BasePrefixVerifiedSat = false;
  std::unordered_map<const BasicBlock *, BlockCheckEntry> BlockResultMap;
  for (auto UIt = Node->useSites().begin(), UE = Node->useSites().end();
       UIt != UE; ++UIt) {
    auto *UseSite = *UIt;
    // special handling for return node: only the corresponding return site
    // is taken into consideration
    if (const GuardedValueFlowReturnNode *RN =
            dyn_cast<GuardedValueFlowReturnNode>(Node)) {
      assert(PrevNode && PrevNode->containsParent(Node));
      if (!std::any_of(
              RN->incomingReturns().begin(), RN->incomingReturns().end(),
              [&](const GuardedValueFlowReturnNode::ReturnIncoming &incoming) {
                return incoming.value == PrevNode && incoming.site == UseSite;
              })) {
        continue;
      }
    }

    DEBUG_TRACE(dbgs() << "\t\t >Detected User: " << *UseSite << "\n");

    // test use site type
    auto UTy = TSV->checkSite(UseSite, TraceBuilder);

    if (UTy == Vulnerability::ST_Others) {
      DEBUG_TRACE(dbgs() << "\t\t\t >Insensitive!\n");
      continue;
    }

    if (UTy == Vulnerability::ST_Call &&
        !hasSummary(dyn_cast<GuardedValueFlowCallSite>(UseSite), Node,
                    IsCommonArgSrc)) {
      DEBUG_TRACE(dbgs() << "\t\t\t >No summary! \n");
      continue;
    }

    if (ActiveSrcGoalTracking && canReport(Src) &&
        UTy == Vulnerability::ST_Sink &&
        ReportedDirectSinks.count(std::make_pair(Node, UseSite))) {
      continue;
    }

    if (UnsatBlocks.contains(UseSite->getParentBasicBlock())) {
      continue;
    }

    Solver->push();
    pushState();

    // ensure that the block of the use site is reachable
    BasicBlock *AnchorBB = Node->getParentBasicBlock();
    if (isa<GuardedValueFlowReturnNode>(Node) && PrevNode &&
        PrevNode->getParentBasicBlock()) {
      AnchorBB = PrevNode->getParentBasicBlock();
    }
    if (UseSite->getParentBasicBlock() != AnchorBB) {
      auto Deps =
          ctrlDepsPair(*Solver, UseSite->getParentBasicBlock(), Graph, AnchorBB);
      Solver->addAll(Deps.first);
      Solver->addAll(Deps.second);
    }
    // inline sym summary, all callee rets should be replaced (map ret/args)
    inlineReturnSymbolicSummary(Src);

    const std::vector<Z3_ast> *VerifiedPrereqAsts = nullptr;
    bool BlockUnreachable = false;
    int PostUTyMask = checkUseSite((const GuardedValueFlowNode *)Node, UseSite,
                                   BlockResultMap, true, &VerifiedPrereqAsts,
                                   &BlockUnreachable);

    Solver->push();

    if (PostUTyMask && canReport(Src) && UTy == Vulnerability::ST_Sink) {
      TraceBuilder.push();
      TraceBuilder.add(UseSite);
      auto Trace = TraceBuilder.snapshot();
      TraceBuilder.pop();

      if (tryReport(Trace))
        markDirectSinkReported(Node, UseSite);

      Solver->pop();
    } else {

      Solver->pop();

      if (PostUTyMask) {
        BasePrefixVerifiedSat = true;
        processUseSite(Src, (const GuardedValueFlowNode *)Node, UseSite, UTy,
                       InlineDepth, VerifiedPrereqAsts);
      } else {
        DEBUG_TRACE(dbgs() << "\t\t\t >Checking unsat!\n");
        if (Node->getParentBasicBlock() == UseSite->getParentBasicBlock()) {
          Solver->pop();
          popState();

          return;
        }
      }
    }

    Solver->pop();
    popState();

    if (BlockUnreachable) {
      BasicBlock *DeadBB = UseSite->getParentBasicBlock();
      UnsatBlocks.add(DeadBB);
      for (BasicBlock *DomBB : properlyDominatedBlocks(DT, DeadBB))
        UnsatBlocks.add(DomBB);
    }

    if (!PostUTyMask && !BasePrefixVerifiedSat) {
      auto BaseRes = Solver->check();
      if (BaseRes == SMTSolver::SMTRT_Unsat)
        return;
      if (BaseRes == SMTSolver::SMTRT_Sat)
        BasePrefixVerifiedSat = true;
    }

    if (allGoalsReportedForCurrentSrc())
      return;
  }

  // continue to search
  auto FlowParents = valueFlowParents(Node, GSAFOptions::EnableArithmeticFlow);
  if (RelevantNodesBuilt) {
    FlowParents.erase(
        std::remove_if(FlowParents.begin(), FlowParents.end(),
                       [&](const GuardedValueFlowNode *P) {
                         return P == PrevNode || !ReachSet.count(P);
                       }),
        FlowParents.end());
  }
  for (auto It = FlowParents.begin(), Next = It, E = FlowParents.end(); It != E;
       It = Next) {
    ++Next;
    const GuardedValueFlowNode *ParentNode = *It;
    if (ParentNode == PrevNode) {
      // this is special for bi-direction search
      // in case of repeating searching the same path.
      continue;
    }

    if (UnsatBlocks.contains(ParentNode->getParentBasicBlock())) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ParentNode
                         << " by scoped unsat block...\n");
      continue;
    }

    auto ResIt = BlockResultMap.find(ParentNode->getParentBasicBlock());
    if (ResIt != BlockResultMap.end() &&
        ResIt->second.Result != SMTSolver::SMTRT_Sat) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ParentNode
                         << " by cached results...\n");
      continue;
    }

    if (TSV->checkNode(ParentNode, TraceBuilder)) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ParentNode
                         << " by filtering nodes...\n");
      continue;
    }

    if (Next != E) {
      TraceBuilder.push();
      Solver->push();
      pushState();
    }

    bottomUpDepthFirstSearch(ParentNode, Node, Src, InlineDepth);

    if (Next != E) {
      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }
  return;
}

static std::vector<Z3_ast> canonicalExprAsts(const SMTExprVec &Vec) {
  std::vector<Z3_ast> Asts;
  Asts.reserve(Vec.size());
  for (unsigned I = 0; I < Vec.size(); ++I) {
    SMTExpr E = Vec[I];
    if (E.isApp() && E.isTrue())
      continue;
    Asts.push_back(static_cast<Z3_ast>(E.getZ3Expr()));
  }
  std::sort(Asts.begin(), Asts.end());
  Asts.erase(std::unique(Asts.begin(), Asts.end()), Asts.end());
  return Asts;
}

int FunctionAnalyzer::checkUseSite(
    const GuardedValueFlowNode *Node, const GuardedValueFlowSite *UseSite,
    std::unordered_map<const BasicBlock *, BlockCheckEntry> &BlockResultMap,
    bool UseCache, const std::vector<Z3_ast> **VerifiedPrereqAsts,
    bool *OutBlockUnreachable) {
  if (VerifiedPrereqAsts)
    *VerifiedPrereqAsts = nullptr;
  if (OutBlockUnreachable)
    *OutBlockUnreachable = false;
  BasicBlock *UseSiteBlock = UseSite->getParentBasicBlock();

  if (UnsatBlocks.contains(UseSiteBlock))
    return 0;

  auto Result = SMTSolver::SMTRT_Uncheck;
  BlockCheckEntry *CachedEntry = nullptr;

  if (UseCache) {
    auto ResultIt = BlockResultMap.find(UseSiteBlock);
    if (ResultIt != BlockResultMap.end()) {
      CachedEntry = &ResultIt->second;
      Result = CachedEntry->Result;
    }
  }

  int SiteCanReach = 1;

  switch (Result) {
  case SMTSolver::SMTRT_Uncheck: {
    Solver->push();

    SMTExprVec Prereq = Solver->getSMTFactory().createEmptySMTExprVec();
    TSV->setPrerequisites(Solver, UseSite, TraceBuilder, Prereq);
    const bool HasPrereq = !Prereq.empty();
    Solver->addAll(Prereq);
    auto CheckRes = Solver->check();
    if (CheckRes == SMTSolver::SMTRT_Unknown) {
      Solver->pop();
      TimeChecker->checkNow();
      return 0;
    }

    auto &Entry = BlockResultMap[UseSiteBlock];
    Entry.Result =
        (CheckRes == SMTSolver::SMTRT_Sat ? SMTSolver::SMTRT_Sat
                                          : SMTSolver::SMTRT_Unsat);
    if (CheckRes == SMTSolver::SMTRT_Sat) {
      Entry.HasPrereqAsts = true;
      Entry.PrereqAsts = canonicalExprAsts(Prereq);
      if (VerifiedPrereqAsts)
        *VerifiedPrereqAsts = &Entry.PrereqAsts;
    } else {
      Entry.HasPrereqAsts = false;
      Entry.PrereqAsts.clear();
      SiteCanReach = 0;
    }

    if (Node->getParentBasicBlock() == UseSiteBlock) {
      auto DominanceRange5 = properlyDominatedBlocks(PDT, UseSiteBlock);
      for (auto I = DominanceRange5.begin(), E = DominanceRange5.end(); I != E;
           I++) {
        auto &DomEntry = BlockResultMap[*I];
        DomEntry.Result =
            (CheckRes == SMTSolver::SMTRT_Sat ? SMTSolver::SMTRT_Sat
                                              : SMTSolver::SMTRT_Unsat);
        DomEntry.HasPrereqAsts = false;
        DomEntry.PrereqAsts.clear();
      }
    }

    if (CheckRes != SMTSolver::SMTRT_Sat) {
      auto DominanceRange6 = properlyDominatedBlocks(DT, UseSiteBlock);
      for (auto I = DominanceRange6.begin(), E = DominanceRange6.end(); I != E;
           I++) {
        auto &DomEntry = BlockResultMap[*I];
        DomEntry.Result = SMTSolver::SMTRT_Unsat;
        DomEntry.HasPrereqAsts = false;
        DomEntry.PrereqAsts.clear();
      }
    }
    Solver->pop();

    if (CheckRes == SMTSolver::SMTRT_Unsat && OutBlockUnreachable) {
      if (!HasPrereq || Solver->check() == SMTSolver::SMTRT_Unsat)
        *OutBlockUnreachable = true;
    }
  } break;
  case SMTSolver::SMTRT_Sat:
    if (VerifiedPrereqAsts && CachedEntry && CachedEntry->HasPrereqAsts)
      *VerifiedPrereqAsts = &CachedEntry->PrereqAsts;
    break;
  case SMTSolver::SMTRT_Unsat:
  case SMTSolver::SMTRT_Unknown:
    SiteCanReach = 0;
    break;
  }
  return SiteCanReach;
}

void FunctionAnalyzer::processUseSite(
    Vulnerability::ValueSitePairType Src, const GuardedValueFlowNode *Node,
    const GuardedValueFlowSite *UseSite, Vulnerability::SiteType USTy,
    unsigned InlineDepth, const std::vector<Z3_ast> *VerifiedPrereqAsts) {
  auto *SrcNode = Src.first;
  auto *SrcSite = Src.second;

  if ((USTy & Vulnerability::ST_Return)) {
    DEBUG_TRACE(dbgs() << "\t\t *Detected Ret-Inst: " << *UseSite << "\n");
    DEBUG_CONSTRAINTS(dbgs() << OUT_SUMMARY_SEP_START << "\n");
    DEBUG_CONSTRAINTS(dbgs() << smtText(Solver->assertions()) << "\n");
    DEBUG_CONSTRAINTS(dbgs() << OUT_SUMMARY_SEP_END << "\n");

    TraceBuilder.push();
    TraceBuilder.add(UseSite);

    OutputSummary *OutSmry =
        createOutputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                            &Solver->getUsedFunctionArguments(), InlineDepth);

    addCachedConstraintsToSummary(OutSmry);

    DEBUG_TRACE(dbgs() << *OutSmry << "\n");

    Smry->OutSmrys[Node].insert(OutSmry);

    TraceBuilder.pop();
  }

  if ((USTy & Vulnerability::ST_Sink)) {
    DEBUG_TRACE(dbgs() << "\t\t *Detected Sink-Inst: " << *UseSite << "\n");

    TraceBuilder.push();
    TraceBuilder.add(UseSite);

    InputSummary *InSmry =
        createInputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                           &Solver->getUsedFunctionArguments(), InlineDepth);

    addCachedConstraintsToSummary(InSmry);

    DEBUG_TRACE(dbgs() << *InSmry << "\n");

    Smry->InSmrys[(isa<GuardedValueFlowArgumentNode>(SrcNode) && !SrcSite)
                      ? SrcNode
                      : nullptr]
        .insert(InSmry);

    TraceBuilder.pop();
  }

  if ((USTy & Vulnerability::ST_Call)) {
    DEBUG_TRACE(dbgs() << "\t\t *Detected Call-Inst: " << *UseSite << "\n");

    auto *GraphCS = dyn_cast<GuardedValueFlowCallSite>(UseSite);
    if (!GraphCS) {
      errs() << *UseSite << "\n";
      llvm_unreachable("ST_Call is not a call site");
    }

    // input summary of the instruction from Node
    if (!std::static_pointer_cast<TaintExtras>(TSV)->isSinkFunction(
            Graph, dyn_cast<GuardedValueFlowArgumentNode>(SrcNode))) {
      if (!SrcSite || Parent->isReachable(SrcSite->getInstruction(),
                                          UseSite->getInstruction())) {

        auto InSmryIt = InputSmryMap.find(std::make_pair(Node, GraphCS));
        if (InSmryIt != InputSmryMap.end()) {
          auto &InSmryVec = InSmryIt->second;
          InSummaryCheckCache CheckCache;
          for (auto *CalleeInSmry : InSmryVec) {
            inlineCalleeInSummary(
                GraphCS, CalleeInSmry, Src, (const GuardedValueFlowNode *)Node,
                std::max<unsigned>(CalleeInSmry->getInlineDepth() + 1,
                                   InlineDepth),
                0, &CheckCache, VerifiedPrereqAsts);
          }
        }
        DEBUG_TRACE(
            dbgs() << "\t\t ---------------------------------------------\n");
      }
    }

    // if Node + UseSite -> taint site : produce a new kind of output summary
    if (isa<GuardedValueFlowArgumentNode>(SrcNode) && !SrcSite) {
      auto SourceWrapperIt = SourceWrapperSmryMap.find(GraphCS);
      if (SourceWrapperIt != SourceWrapperSmryMap.end()) {
        auto SmryIt = SourceWrapperIt->second.find(Node);
        if (SmryIt != SourceWrapperIt->second.end()) {
          auto &InSmryVec = SmryIt->second;
          InSummaryCheckCache CheckCache;
          for (auto *CalleeInSmry : InSmryVec) {
            DEBUG_TRACE(dbgs() << "\t\t " << TAINT_WRAPPER_STR << "[A]\n");
            inlineCalleeInSummary(
                GraphCS, CalleeInSmry, Src, (const GuardedValueFlowNode *)Node,
                std::max<unsigned>(CalleeInSmry->getInlineDepth() + 1,
                                   InlineDepth),
                2, &CheckCache, VerifiedPrereqAsts);
          }
        }
      }

      auto SrcIt = Sources.find(GraphCS);
      if (SrcIt != Sources.end() && SrcIt->second.count(Node)) {
        TraceBuilder.push();
        TraceBuilder.add(UseSite);

        DEBUG_TRACE(dbgs() << "\t\t " << TAINT_WRAPPER_STR << "[B]\n");

        InputSummary *InSmry = createInputSummary(
            Solver->assertions(), TraceBuilder.snapshot(),
            &Solver->getUsedFunctionArguments(), InlineDepth);

        addCachedConstraintsToSummary(InSmry);

        DEBUG_TRACE(dbgs() << *InSmry << "\n");

        Smry->TaintSourceWrapperSmrys[SrcNode].insert(InSmry);

        TraceBuilder.pop();
      }
    }

    // output summary of the instruction from Node
    const bool IsCommonArgSrc =
        !Src.second && nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                                  GuardedValueFlowNode>(Src.first);
    const auto &ReachSet =
        IsCommonArgSrc ? CanReachCommonArgSite : CanReachAnyRelevantSite;
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;
      auto NativeRange1 = callOutputs(GraphCS, Callee);
      for (auto OutputIt = NativeRange1.begin(), E = NativeRange1.end();
           OutputIt != E; ++OutputIt) {
        auto *OutputNode = *OutputIt;
        if (RelevantNodesBuilt && !ReachSet.count(OutputNode)) {
          continue;
        }

        auto OutSmryIt =
            OutputSmryMap.find(std::make_tuple(Node, OutputNode, Callee));
        if (OutSmryIt == OutputSmryMap.end()) {
          continue;
        }

        auto &OutSmryVec = OutSmryIt->second;
        for (auto *CalleeOutSmry : OutSmryVec) {
          inlineCalleeOutSummary(
              GraphCS, CalleeOutSmry, OutputNode, Src,
              std::max<unsigned>(CalleeOutSmry->getInlineDepth() + 1,
                                 InlineDepth));
        }
      }
    }

    DEBUG_TRACE(
        dbgs() << "\t\t *********************************************\n");
  }

  return;
}

bool FunctionAnalyzer::canReport(Vulnerability::ValueSitePairType Src) const {
  return !(isa<GuardedValueFlowArgumentNode>(Src.first) && !Src.second);
}

SMTExpr
FunctionAnalyzer::getTranslatedRenamedSummary(const SummaryCacheItem &Item) {
  Z3_ast SummaryAst = nullptr;
  bool IsLiteralTrue = false;
  bool IsLiteralFalse = false;
  {
    SummaryBase::SMTReadLock();
    SummaryAst = static_cast<Z3_ast>(Item.constraints->getZ3Expr());
    if (Item.constraints->isApp()) {
      IsLiteralTrue = Item.constraints->isTrue();
      IsLiteralFalse = Item.constraints->isFalse();
    }
    SummaryBase::SMTReadUnlock();
  }
  if (IsLiteralTrue)
    return Fctry->createBoolVal(true);
  if (IsLiteralFalse)
    return Fctry->createBoolVal(false);

  RenamedSummaryKey Key{SummaryAst, Item.suffix};
  auto It = RenamedSummaryCache.find(Key);
  if (It != RenamedSummaryCache.end())
    return It->second;

  SummaryBase::SMTReadLock();
  SMTExpr Translated = Fctry->translate(*Item.constraints);
  SummaryBase::SMTReadUnlock();

  SMTExprVec TempVec = Fctry->createEmptySMTExprVec();
  TempVec.push_back(Translated);
  std::unordered_map<std::string, SMTExpr> Mapping;
  SMTExpr Renamed =
      Fctry->rename(TempVec, Item.suffix, Mapping, &Parent->SMTAdvisor)
          .first.toAndExpr();
  auto Inserted = RenamedSummaryCache.emplace(std::move(Key), Renamed);
  return Inserted.first->second;
}

Z3_ast FunctionAnalyzer::pinTryReportExpr(const SMTExpr &Expr) {
  Z3_ast Ast = static_cast<Z3_ast>(const_cast<SMTExpr &>(Expr).getZ3Expr());
  TryReportPinnedExprs.emplace(Ast, Expr);
  return Ast;
}

// Try to report a vulnerability if a complete trace is found.
// It uses SMT solver to verify the feasibility of the trace.
// Returns true iff the trace was synchronously verified SAT and recorded.
bool FunctionAnalyzer::tryReport(std::shared_ptr<VulnerabilityTrace> Trace) {
  TimeChecker->check();
  if (SMTTimeChecker->isTimeOut())
    return false;

  // Fast path: when there are no deferred callee summary constraints,
  // Solver->check() already proved the current assertions satisfiable, so no
  // additional SMT check or cross-context translation is needed.
  if (NonSymDepsCache.empty() && SymbDepsCache.empty()) {
    SummaryBase::SMTWriteLock();
    Trace->setReported(true);
    BugTraces.push_back(std::make_pair(1, Trace));
    SummaryBase::SMTWriteUnlock();
    return true;
  }

  auto recordTrace = [&]() {
    SummaryBase::SMTWriteLock();
    Trace->setReported(true);
    BugTraces.push_back(std::make_pair(1, Trace));
    SummaryBase::SMTWriteUnlock();
  };

  std::vector<Z3_ast> OrderedAsts;
  SmallPtrSet<Z3_ast, 32> SeenOrderedAsts;
  bool HasNonTrueDeferred = false;

  auto appendOrderedAst = [&](const SMTExpr &Expr, bool IsDeferred) -> bool {
    if (Expr.isApp()) {
      if (Expr.isTrue())
        return true;
      if (Expr.isFalse())
        return false;
    }
    if (IsDeferred)
      HasNonTrueDeferred = true;
    Z3_ast Ast = pinTryReportExpr(Expr);
    if (SeenOrderedAsts.insert(Ast).second)
      OrderedAsts.push_back(Ast);
    return true;
  };

  const auto &BaseAssertions = Solver->pendingAssertions();
  const auto &BaseScopeMarks = Solver->scopeAssertionMarks();
  OrderedAsts.reserve(NonSymDepsCache.size() + SymbDepsCache.size() +
                      BaseAssertions.size());
  std::vector<size_t> TargetScopeMarks;
  TargetScopeMarks.reserve(BaseScopeMarks.size() + 3);
  TargetScopeMarks.push_back(0);

  for (const SummaryCacheItem &Item : NonSymDepsCache.getCacheVector()) {
    SMTExpr Renamed = getTranslatedRenamedSummary(Item);
    if (!appendOrderedAst(Renamed, true))
      return false;
  }

  for (const SummaryCacheItem &Item : SymbDepsCache.getCacheVector()) {
    SMTExpr Renamed = getTranslatedRenamedSummary(Item);
    if (!appendOrderedAst(Renamed, true))
      return false;
  }

  // If all deferred summary constraints simplified/pruned to true, Solver
  // already proved BaseAssertions satisfiable.
  if (!HasNonTrueDeferred) {
    recordTrace();
    return true;
  }

  if (OrderedAsts.size() > TargetScopeMarks.back())
    TargetScopeMarks.push_back(OrderedAsts.size());

  size_t NextBaseMarkIdx = 0;
  for (size_t I = 0; I < BaseAssertions.size(); ++I) {
    while (NextBaseMarkIdx < BaseScopeMarks.size() &&
           BaseScopeMarks[NextBaseMarkIdx] <= I) {
      if (OrderedAsts.size() > TargetScopeMarks.back())
        TargetScopeMarks.push_back(OrderedAsts.size());
      ++NextBaseMarkIdx;
    }
    if (!appendOrderedAst(BaseAssertions[I], false))
      return false;
  }

  std::vector<Z3_ast> FullAsts = OrderedAsts;
  std::sort(FullAsts.begin(), FullAsts.end());

  auto FullIt = TryReportSatCache.find(FullAsts);
  if (FullIt != TryReportSatCache.end()) {
    if (FullIt->second) {
      recordTrace();
      return true;
    }
    return false;
  }

  // Synchronous fast path when ThreadPool has no worker threads (default):
  // maintain a lazy incremental QF_BV z3::solver synced to chunked scope
  // boundaries along the longest common prefix of OrderedAsts (deferred summary
  // batch followed by Solver scope batches). Grouping assertions into scope
  // batches avoids calling inc_sat_solver::internalize_formulas() once per
  // individual assertion.
  if (!ThreadPool::get()->hasWorkers()) {
    TimeChecker->suspend();
    SMTTimeChecker->resume();

    if (!ReportSolver) {
      z3::context &Ctx = Fctry->getZ3Ctx();
      ReportSolver = std::make_unique<z3::solver>(
          Ctx,
          Z3_mk_solver_for_logic(Ctx, Z3_mk_string_symbol(Ctx, "QF_BV")));
      unsigned ReportTimeoutMs =
          (SMTSolver::GlobalTimeout > 0)
              ? static_cast<unsigned>(SMTSolver::GlobalTimeout)
              : (GSAFOptions::Timeout * 1000U);
      if (ReportTimeoutMs > 0) {
        z3::params Z3Params(Ctx);
        Z3Params.set("timeout", ReportTimeoutMs);
        ReportSolver->set(Z3Params);
      }
    }

    size_t LCP = 0;
    const size_t MinLen = std::min(ReportSolverStack.size(), OrderedAsts.size());
    while (LCP < MinLen && ReportSolverStack[LCP] == OrderedAsts[LCP])
      ++LCP;

    if (LCP < ReportSolverStack.size()) {
      unsigned NumPops = 0;
      size_t NewLen = ReportSolverStack.size();
      while (!ReportSolverScopeMarks.empty() && NewLen > LCP) {
        NewLen = ReportSolverScopeMarks.back();
        ReportSolverScopeMarks.pop_back();
        ++NumPops;
      }
      if (NewLen > LCP) {
        ReportSolver->reset();
        ReportSolverStack.clear();
        ReportSolverScopeMarks.clear();
      } else {
        if (NumPops > 0)
          ReportSolver->pop(NumPops);
        ReportSolverStack.resize(NewLen);
      }
    }

    size_t CurIdx = ReportSolverStack.size();
    const size_t TargetLen = OrderedAsts.size();
    if (CurIdx < TargetLen) {
      std::vector<size_t> PushPoints;
      PushPoints.push_back(CurIdx);
      for (size_t M : TargetScopeMarks) {
        if (M > CurIdx && M < TargetLen)
          PushPoints.push_back(M);
      }
      for (size_t PIdx = 0; PIdx < PushPoints.size(); ++PIdx) {
        const size_t Start = PushPoints[PIdx];
        const size_t End =
            (PIdx + 1 < PushPoints.size()) ? PushPoints[PIdx + 1] : TargetLen;
        ReportSolver->push();
        ReportSolverScopeMarks.push_back(Start);
        for (size_t I = Start; I < End; ++I) {
          ReportSolver->add(
              TryReportPinnedExprs.at(OrderedAsts[I]).getZ3Expr());
          ReportSolverStack.push_back(OrderedAsts[I]);
        }
      }
    }

    z3::check_result CheckRes = z3::unknown;
    try {
      CheckRes = ReportSolver->check();
    } catch (const z3::exception &) {
      CheckRes = z3::unknown;
    }

    const bool IsSat = (CheckRes == z3::sat);
    if (CheckRes != z3::unknown)
      TryReportSatCache.emplace(std::move(FullAsts), IsSat);

    if (IsSat)
      recordTrace();

    SMTTimeChecker->suspend();
    TimeChecker->resume();
    return IsSat;
  }

  SMTFactory *F = new SMTFactory;
  SMTSolver *S = new SMTSolver(F->createSMTSolverWithTactic("qfbv"));
  SMTExprVec *FullDep = new SMTExprVec(F->createEmptySMTExprVec());

  for (Z3_ast Ast : FullAsts)
    FullDep->push_back(F->translate(TryReportPinnedExprs.at(Ast)));

  TraceList<std::pair<int, std::shared_ptr<VulnerabilityTrace>>> *Reports =
      &BugTraces;

  TimeChecker->suspend();
  SMTTimeChecker->resume();
  Parent->ReportTasks.async([Trace, Reports, FullDep, F, S] {
    bool ShouldReport = true;

    if (!FullDep->empty()) {
      S->addAll(*FullDep);
      if (S->check() != SMTSolver::SMTRT_Sat)
        ShouldReport = false;
    }

    if (ShouldReport) {
      SummaryBase::SMTWriteLock();
      Trace->setReported(true);
      Reports->push_back(std::make_pair(1, Trace));
      SummaryBase::SMTWriteUnlock();
    }

    delete FullDep;
    delete S;
    delete F;
  });
  SMTTimeChecker->suspend();
  TimeChecker->resume();
  return false;
}

} // namespace lotus::gsaf
