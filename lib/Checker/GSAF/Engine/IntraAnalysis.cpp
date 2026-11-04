#include "Checker/GSAF/Engine/FunctionAnalyzer.h"

#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "Checker/GSAF/Support/Options.h"
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

  Fctry = new SMTFactory;

  if (GSAFOptions::SolverVersion == 1) {
    auto *configured = new GSAFSolver(*Fctry, P->getModule()->getDataLayout());
    configured->setModels(P->Models, GSAFOptions::EnableHeapAllocFailure,
                          GSAFOptions::EnableFileAllocFailure);
    Solver = configured;
  } else if (GSAFOptions::SolverVersion == 2) {
    auto *configured =
        new DTGSAFSolver(*Fctry, P->getModule()->getDataLayout(), DT);
    configured->setModels(P->Models, GSAFOptions::EnableHeapAllocFailure,
                          GSAFOptions::EnableFileAllocFailure);
    Solver = configured;
  } else {
    llvm_unreachable(
        "Unsupported symbolic expression solver (can be 1 and 2).");
  }

  initSummary();

  // TSV is the only vulnerability
  TSV = std::static_pointer_cast<TaintStyleVulnerability>(
      Parent->Vuln->getVulnerability(1));
}

FunctionAnalyzer::~FunctionAnalyzer() {
  delete Solver;
  delete TimeChecker;
  delete SMTTimeChecker;
  delete Fctry;
}

void FunctionAnalyzer::pushState() {
  NonSymDepsCache.push();
  SymbDepsCache.push();
  CallSiteOutputCache.push();
}

void FunctionAnalyzer::resetState() {
  NonSymDepsCache.reset();
  SymbDepsCache.reset();
  CallSiteOutputCache.reset();
}

void FunctionAnalyzer::popState() {
  NonSymDepsCache.pop();
  SymbDepsCache.pop();
  CallSiteOutputCache.pop();
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

    search(SrcNode, nullptr, std::make_pair(SrcNode, SrcSite), 0);

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
      for (InputSummary *Smry : SmrySet) {
        inlineCalleeInSummary(SrcSite, Smry, std::make_pair(SrcNode, SrcSite),
                              (const GuardedValueFlowNode *)SrcNode, 0, 1);
      }

      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }

  // search from arguments
  DEBUG_TRACE(dbgs() << "\t [Search from arguments]\n");
  for (auto ArgIt = Graph->arg_begin(), E = Graph->arg_end(); ArgIt != E;
       ++ArgIt) {
    auto *ArgNode = *ArgIt;

    if (!GSAFOptions::EnableSideEffectSource &&
        nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                   GuardedValueFlowNode>(ArgNode)) {
      continue;
    }

    DEBUG_TRACE(dbgs() << "\t [Search from arguments -> " << *ArgNode << "]\n");

    if (TSV->checkNode(ArgNode, TraceBuilder)) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ArgNode << "...\n");
      continue;
    }

    pushState();
    TraceBuilder.push();
    Solver->push();

    TraceBuilder.add(ArgNode);
    TraceBuilder.add(nullptr);

    bottomUpDepthFirstSearch(ArgNode, nullptr, std::make_pair(ArgNode, nullptr),
                             0);

    Solver->pop();
    TraceBuilder.pop();
    popState();
  }

  // search from call site outputs
  DEBUG_TRACE(dbgs() << "\t [Search from SourceOutSmryVec]\n");
  for (auto &SrcOutSmryIt : SourceOutSmryVec) {
    auto *CallSiteOutput = SrcOutSmryIt.first;
    auto *OutSmry = SrcOutSmryIt.second;
    assert(!isa<GuardedValueFlowNode>(OutSmry->getSourceNode()));

    DEBUG_TRACE(dbgs() << "\t [Search from SourceOutSmryVec -> "
                       << *CallSiteOutput << "]\n");

    auto Src = std::make_pair(CallSiteOutput, callSite(CallSiteOutput));
    inlineCalleeOutSummary(callSite(CallSiteOutput), OutSmry, CallSiteOutput,
                           Src, OutSmry->getInlineDepth() + 1);
  }

  buildReturnSymbolicSummary();

  finalizeSummary();
}

// General search function that can handle both forward and backward traversal.
// Node: Current node.
// PrevNode: Previous node in the traversal.
// Src: The source of the taint being tracked.
// InlineDepth: Current inline depth.
void FunctionAnalyzer::search(const GuardedValueFlowNode *Node,
                              const GuardedValueFlowNode *PrevNode,
                              Vulnerability::ValueSitePairType Src,
                              unsigned InlineDepth) {
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
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
  }
  inlineReturnSymbolicSummary(Src);
  TraceBuilder.add(Node);

  // search up
  pushState();
  TraceBuilder.push();
  Solver->push();

  bottomUpDepthFirstSearch(Node, PrevNode, Src, InlineDepth);

  Solver->pop();
  TraceBuilder.pop();
  popState();

  if (const GuardedValueFlowCallOutputNode *CSO =
          dyn_cast<GuardedValueFlowCallOutputNode>(Node)) {
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
                                 InlineDepth),
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

      search(Child, Node, Src, InlineDepth);

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

  std::unordered_map<const BasicBlock *, SMTSolver::SMTResultType>
      BlockResultMap;
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
        !hasSummary(dyn_cast<GuardedValueFlowCallSite>(UseSite), Node)) {
      DEBUG_TRACE(dbgs() << "\t\t\t >No summary! \n");
      continue;
    }

    Solver->push();
    pushState();

    // ensure that the block of the use site is reachable
    if (UseSite->getParentBasicBlock() != Node->getParentBasicBlock()) {
      auto Deps = ctrlDepsPair(*Solver, UseSite->getParentBasicBlock(), Graph,
                               Node->getParentBasicBlock());
      Solver->addAll(Deps.first);
      Solver->addAll(Deps.second);
    }
    // inline sym summary, all callee rets should be replaced (map ret/args)
    inlineReturnSymbolicSummary(Src);

    int PostUTyMask = checkUseSite((const GuardedValueFlowNode *)Node, UseSite,
                                   BlockResultMap, true);

    Solver->push();

    if (PostUTyMask && canReport(Src) && UTy == Vulnerability::ST_Sink) {
      TraceBuilder.push();
      TraceBuilder.add(UseSite);
      auto Trace = TraceBuilder.snapshot();
      TraceBuilder.pop();

      tryReport(Trace);

      Solver->pop();
    } else {

      Solver->pop();

      if (PostUTyMask) {
        processUseSite(Src, (const GuardedValueFlowNode *)Node, UseSite, UTy,
                       InlineDepth);
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
  }

  // continue to search
  auto FlowParents = valueFlowParents(Node, GSAFOptions::EnableArithmeticFlow);
  for (auto It = FlowParents.begin(), Next = It, E = FlowParents.end(); It != E;
       It = Next) {
    ++Next;
    const GuardedValueFlowNode *ParentNode = *It;
    if (ParentNode == PrevNode) {
      // this is special for bi-direction search
      // in case of repeating searching the same path.
      continue;
    }

    auto ResIt = BlockResultMap.find(ParentNode->getParentBasicBlock());
    if (ResIt != BlockResultMap.end() &&
        ResIt->second != SMTSolver::SMTRT_Sat) {
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

int FunctionAnalyzer::checkUseSite(
    const GuardedValueFlowNode *Node, const GuardedValueFlowSite *UseSite,
    std::unordered_map<const BasicBlock *, SMTSolver::SMTResultType>
        &BlockResultMap,
    bool UseCache) {
  BasicBlock *UseSiteBlock = UseSite->getParentBasicBlock();

  auto Result = SMTSolver::SMTRT_Uncheck;

  if (UseCache) {
    auto ResultIt = BlockResultMap.find(UseSiteBlock);
    if (ResultIt != BlockResultMap.end()) {
      Result = ResultIt->second;
    }
  }

  int SiteCanReach = 1;

  switch (Result) {
  case SMTSolver::SMTRT_Uncheck: {
    Solver->push();

    SMTExprVec Prereq = Solver->getSMTFactory().createEmptySMTExprVec();
    TSV->setPrerequisites(Solver, UseSite, TraceBuilder, Prereq);
    Solver->addAll(Prereq);
    auto Result = Solver->check();

    BlockResultMap[UseSiteBlock] =
        (Result == SMTSolver::SMTRT_Sat ? SMTSolver::SMTRT_Sat
                                        : SMTSolver::SMTRT_Unsat);

    if (Result != SMTSolver::SMTRT_Sat) {
      SiteCanReach = 0;
    }

    if (Node->getParentBasicBlock() == UseSiteBlock &&
        !properlyDominatedBlocks(PDT, UseSiteBlock).empty()) {
      // Caching Result to all BasicBlocks that post-dom InstBlock,
      auto DominanceRange5 = properlyDominatedBlocks(PDT, UseSiteBlock);
      for (auto I = DominanceRange5.begin(), E = DominanceRange5.end(); I != E;
           I++) {
        BlockResultMap[*I] =
            (Result == SMTSolver::SMTRT_Sat ? SMTSolver::SMTRT_Sat
                                            : SMTSolver::SMTRT_Unsat);
      }
    }

    if (Result != SMTSolver::SMTRT_Sat &&
        !properDominators(DT, UseSiteBlock).empty()) {
      auto DominanceRange6 = properDominators(DT, UseSiteBlock);
      for (auto I = DominanceRange6.begin(), E = DominanceRange6.end(); I != E;
           I++) {
        BlockResultMap[*I] = (SMTSolver::SMTRT_Unsat);
      }
    }
    Solver->pop();
  } break;
  case SMTSolver::SMTRT_Sat:
    break;
  case SMTSolver::SMTRT_Unsat:
  case SMTSolver::SMTRT_Unknown:
    SiteCanReach = 0;
    break;
  }
  return SiteCanReach;
}

void FunctionAnalyzer::processUseSite(Vulnerability::ValueSitePairType Src,
                                      const GuardedValueFlowNode *Node,
                                      const GuardedValueFlowSite *UseSite,
                                      Vulnerability::SiteType USTy,
                                      unsigned InlineDepth) {
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
          for (auto *CalleeInSmry : InSmryVec) {
            inlineCalleeInSummary(
                GraphCS, CalleeInSmry, Src, (const GuardedValueFlowNode *)Node,
                std::max<unsigned>(CalleeInSmry->getInlineDepth() + 1,
                                   InlineDepth));
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
          for (auto *CalleeInSmry : InSmryVec) {
            DEBUG_TRACE(dbgs() << "\t\t " << TAINT_WRAPPER_STR << "[A]\n");
            inlineCalleeInSummary(
                GraphCS, CalleeInSmry, Src, (const GuardedValueFlowNode *)Node,
                std::max<unsigned>(CalleeInSmry->getInlineDepth() + 1,
                                   InlineDepth),
                2);
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
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;
      auto NativeRange1 = callOutputs(GraphCS, Callee);
      for (auto OutputIt = NativeRange1.begin(), E = NativeRange1.end();
           OutputIt != E; ++OutputIt) {
        auto *OutputNode = *OutputIt;

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

// Try to report a vulnerability if a complete trace is found.
// It uses SMT solver to verify the feasibility of the trace.
void FunctionAnalyzer::tryReport(std::shared_ptr<VulnerabilityTrace> Trace) {
  TimeChecker->check();
  if (SMTTimeChecker->isTimeOut())
    return;

  SMTFactory *F = new SMTFactory;
  SMTSolver *S = new SMTSolver(F->createSMTSolver());

  SMTAdvisor4Inlining *SMTAdviser = &Parent->SMTAdvisor;

  SMTExprVec *NonSymDep = new SMTExprVec(F->createEmptySMTExprVec());
  SMTExprVec *SymbDep = new SMTExprVec(F->createEmptySMTExprVec());

  for (SummaryCacheItem item : NonSymDepsCache.getCacheVector()) {
    SummaryBase::SMTReadLock();
    SMTExpr Translate = F->translate(*item.constraints);
    SummaryBase::SMTReadUnlock();

    SMTExprVec TempVec = F->createEmptySMTExprVec();
    TempVec.push_back(Translate);
    std::unordered_map<std::string, SMTExpr> Mapping;
    NonSymDep->push_back(
        F->rename(TempVec, item.suffix, Mapping, SMTAdviser).first.toAndExpr());
  }

  for (SummaryCacheItem item : SymbDepsCache.getCacheVector()) {
    SummaryBase::SMTReadLock();
    SMTExpr Translate = F->translate(*item.constraints);
    SummaryBase::SMTReadUnlock();

    SMTExprVec TempVec = F->createEmptySMTExprVec();
    TempVec.push_back(Translate);
    std::unordered_map<std::string, SMTExpr> Mapping;
    SymbDep->push_back(
        F->rename(TempVec, item.suffix, Mapping, SMTAdviser).first.toAndExpr());
  }

  S->addAll(F->translate(Solver->assertions()));

  TraceList<std::pair<int, std::shared_ptr<VulnerabilityTrace>>> *Reports =
      &BugTraces;

  TimeChecker->suspend();
  SMTTimeChecker->resume();
  Parent->ReportTasks.async([Trace, Reports, NonSymDep, SymbDep, F, S] {
    bool ShouldReport = true;

    if (!NonSymDep->empty()) {
      S->addAll(*NonSymDep);

      auto Result = S->check();

      if (Result != SMTSolver::SMTRT_Sat) {
        ShouldReport = false;
      }
    }

    if (ShouldReport) {
      if (!SymbDep->empty()) {
        S->addAll(*SymbDep);
        auto Result = S->check();

        if (Result != SMTSolver::SMTRT_Sat) {
          ShouldReport = false;
        }
      }

      if (ShouldReport) {
        // report
        SummaryBase::SMTWriteLock();
        Trace->setReported(true);
        Reports->push_back(std::make_pair(1, Trace));
        SummaryBase::SMTWriteUnlock();
      }
    }

    delete NonSymDep;
    delete SymbDep;
    delete S;
    delete F;
  });
  SMTTimeChecker->suspend();
  TimeChecker->resume();
}

} // namespace lotus::gsaf
