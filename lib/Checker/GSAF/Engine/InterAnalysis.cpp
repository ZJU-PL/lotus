#include "Checker/GSAF/Engine/FunctionAnalyzer.h"

#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "Checker/GSAF/Support/Options.h"
#include "Utils/LLVM/StringUtils.h"

#include <list>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::gsaf {
using namespace llvm;

/*==----macros for debugging----==*/

#define DEBUG_TYPE "gsaf-function-analyzer"

#define SKIP_STR "\033[0;32m#Skip: \033[0m"

#define DEBUG_TRACE(X)                                                         \
  do {                                                                         \
    if (GSAFOptions::DebugTrace && F->hasName() &&                             \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

InputSummary *FunctionAnalyzer::createInputSummary(
    const SMTExprVec &Constraints, std::shared_ptr<VulnerabilityTrace> Trace,
    const std::unordered_set<const GuardedValueFlowNode *> *Inputs,
    unsigned InlineDepth) {
  InputSummary *InSmry = new InputSummary(F, std::move(Trace), InlineDepth);
  InSmry->setVulnerabilityMask(-1);
  auto NonSymDep = Constraints.toAndExpr();
  InSmry->addNonSymDeps(SummaryCacheItem(&NonSymDep, "", 0));
  if (Inputs)
    InSmry->setInputs(*Inputs);

  return InSmry;
}

OutputSummary *FunctionAnalyzer::createOutputSummary(
    const SMTExprVec &Constraints, std::shared_ptr<VulnerabilityTrace> Trace,
    const std::unordered_set<const GuardedValueFlowNode *> *Inputs,
    unsigned InlineDepth) {
  OutputSummary *OutSmry = new OutputSummary(F, std::move(Trace), InlineDepth);
  OutSmry->setVulnerabilityMask(-1);
  auto NonSymDep = Constraints.toAndExpr();
  OutSmry->addNonSymDeps(SummaryCacheItem(&NonSymDep, "", 0));
  if (Inputs)
    OutSmry->setInputs(*Inputs);

  return OutSmry;
}

void FunctionAnalyzer::addCachedConstraintsToSummary(SummaryBase *S) {
  for (const SummaryCacheItem &item : NonSymDepsCache.getCacheVector()) {
    if ((unsigned)item.depth < GSAFOptions::InlineDepth)
      S->addNonSymDeps(item);
  }

  for (const SummaryCacheItem &item : SymbDepsCache.getCacheVector()) {
    if ((unsigned)item.depth < GSAFOptions::InlineDepth)
      S->addSymbDeps(item);
  }
}

template <class T>
static void finalizeSummaryItem(
    std::map<const GuardedValueFlowNode *, std::set<T *, trace_summary_cmp>,
             ObjectLess> &SI) {
  if (GSAFOptions::MaxSummary == -1) {
    // no restriction
    return;
  }

  const int CARED_INLINE_DEPTH = 10;

  std::map<const GuardedValueFlowNode *, std::set<T *, trace_summary_cmp>,
           ObjectLess>
      TmpSmry[CARED_INLINE_DEPTH + 1];
  int Size[CARED_INLINE_DEPTH + 1] = {0};

  // partition the summaries according to inline depth
  for (auto &item : SI) {
    const GuardedValueFlowNode *Key = item.first;
    for (T *InSmry : item.second) {
      if (InSmry == nullptr) {
        continue;
      }
      unsigned int Depth = InSmry->getInlineDepth();
      if (Depth >= CARED_INLINE_DEPTH) {
        TmpSmry[CARED_INLINE_DEPTH][Key].insert(InSmry);
        Size[CARED_INLINE_DEPTH]++;
      } else {
        TmpSmry[Depth][Key].insert(InSmry);
        Size[Depth]++;
      }
    }
  }

  // Find the adaptive inline depth that ensures summary count less than
  // GSAFOptions::MaxSummary
  int Sum = 0;
  int Adaptive = 0;
  for (Adaptive = 0; Adaptive < CARED_INLINE_DEPTH + 1; Adaptive++) {
    Sum += Size[Adaptive];
    if (Sum > GSAFOptions::MaxSummary) {
      break;
    }
  }

  if (Adaptive < CARED_INLINE_DEPTH + 1) {
    SI.clear();
    for (int i = 0; i < Adaptive; i++) {
      if (Size[i] == 0) {
        // Empty set
        continue;
      }
      for (auto &item : TmpSmry[i]) {
        const GuardedValueFlowNode *Key = item.first;
        for (T *SmryItem : item.second) {
          SI[Key].insert(SmryItem);
        }
      }
    }
  }
}

void FunctionAnalyzer::finalizeSummary() {
  finalizeSummaryItem(Smry->InSmrys);
  finalizeSummaryItem(Smry->OutSmrys);
  finalizeSummaryItem(Smry->TaintSourceWrapperSmrys);
}

void FunctionAnalyzer::initSummary() {
  auto NativeRange2 = callSites(Graph);
  for (auto CSIt = NativeRange2.begin(), CSE = NativeRange2.end(); CSIt != CSE;
       ++CSIt) {
    auto *GraphCS = *CSIt;
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;
      auto NativeRange3 = callOutputs(GraphCS, Callee);
      for (auto It = NativeRange3.begin(), E = NativeRange3.end(); It != E;
           ++It) {
        initOutputSummary(GraphCS, Callee, *It);
      }

      for (auto It = GraphCS->input_begin(Callee),
                E = GraphCS->input_end(Callee);
           It != E; ++It) {
        auto &InputStruct = *It;
        initInputSummary(GraphCS, Callee, InputStruct.InputNode,
                         InputStruct.InputIndex, InputStruct.IsCommonInput);
        initSourceWrapperSummary(GraphCS, Callee, InputStruct.InputNode,
                                 InputStruct.InputIndex,
                                 InputStruct.IsCommonInput);
      }
    }
  }
}

void FunctionAnalyzer::initOutputSummary(
    const GuardedValueFlowCallSite *GraphCS, Function *Callee,
    const GuardedValueFlowCallOutputNode *Node) {
  assert(Node && Callee);
  assert((!GraphCS->getCalledFunction() ||
          GraphCS->getCalledFunction() == Callee) &&
         "Direct call has only one callee.");

  if (GraphCS->isBackEdge(Callee)) {
    return;
  }

  auto It = Parent->FuncSmryMap.find(Callee);
  if (It == Parent->FuncSmryMap.end()) {
    // This may happen in following two cases
    // (1) recursive call (2) empty function
    return;
  }

  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);

  auto *CalleeGraph = Parent->getGraph(Callee);
  auto *RetNode =
      nodeOfKind<GuardedValueFlowNode::Kind::CallSiteCommonOutput,
                 GuardedValueFlowCallOutputNode>(Node)
          ? (const GuardedValueFlowNode *)CalleeGraph->getCommonReturn()
          : (const GuardedValueFlowNode *)CalleeGraph->getPseudoReturn(
                Node->getIndex());
  assert(RetNode);
  auto SmryIt = AllSmry->OutSmrys.find(RetNode);
  if (SmryIt == AllSmry->OutSmrys.end()) {
    return;
  }

  for (auto *OutSmry : SmryIt->second) {
    auto *SrcNode = OutSmry->getSourceNode();
    if (!isa<GuardedValueFlowArgumentNode>(SrcNode)) {
      if (OutSmry->getInlineDepth() >= GSAFOptions::InlineDepth) {
        continue;
      }
      SourceOutSmryVec.emplace_back(Node, OutSmry);
    } else {
      assert(isa<GuardedValueFlowArgumentNode>(SrcNode));
      if (nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                     GuardedValueFlowNode>(SrcNode)) {
        for (size_t I = 0; I < GraphCS->getCommonInputs().size(); I++) {
          OutputSmryMap[std::make_tuple(GraphCS->getCommonInputs()[I], Node,
                                        Callee)]
              .insert(OutSmry);
        }
      } else if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                            GuardedValueFlowNode>(SrcNode)) {
        OutputSmryMap[std::make_tuple(
                          GraphCS->getCommonInput(
                              ((const GuardedValueFlowNode *)SrcNode)
                                  ->getIndex()),
                          Node, Callee)]
            .insert(OutSmry);
      } else {
        assert((nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                           GuardedValueFlowNode>(SrcNode)));
        size_t Index = ((const GuardedValueFlowNode *)SrcNode)->getIndex();

        if (auto *PseudoInput = GraphCS->getPseudoInput(Callee, Index)) {
          OutputSmryMap[std::make_tuple(PseudoInput, Node, Callee)].insert(
              OutSmry);
        }
      }
    }
  }
}

void FunctionAnalyzer::initInputSummary(const GuardedValueFlowCallSite *GraphCS,
                                        Function *Callee,
                                        const GuardedValueFlowNode *ActualArg,
                                        std::size_t ArgIndex,
                                        bool CommonActual) {
  assert(ActualArg && Callee);

  if (GraphCS->isBackEdge(Callee)) {
    return;
  }

  auto It = Parent->FuncSmryMap.find(Callee);
  if (It == Parent->FuncSmryMap.end()) {
    // This may happen in following two cases
    // (1) recursive call (2) empty function
    return;
  }

  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);

  auto *CalleeGraph = Parent->getGraph(Callee);
  size_t NumArgument = CommonActual ? CalleeGraph->getNumCommonArgument()
                                    : CalleeGraph->getNumPseudoArgument();
  if (ArgIndex < NumArgument) {
    const GuardedValueFlowNode *Formal =
        CommonActual
            ? (const GuardedValueFlowNode *)CalleeGraph->getCommonArgument(
                  ArgIndex)
            : (const GuardedValueFlowNode *)CalleeGraph->getPseudoArgument(
                  ArgIndex);

    auto SmryIt = AllSmry->InSmrys.find(Formal);
    if (SmryIt != AllSmry->InSmrys.end()) {
      for (auto *CalleeInSmry : SmryIt->second) {
        InputSmryMap[std::make_pair(ActualArg, GraphCS)].insert(CalleeInSmry);
      }
    }
  } else {
    assert(CommonActual);
    for (size_t AI = 0, AE = CalleeGraph->getNumVarArgument(); AI != AE; AI++) {
      auto *Formal = CalleeGraph->getVarArgument(AI);
      assert((nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                         GuardedValueFlowNode>(Formal)));
      auto SmryIt = AllSmry->InSmrys.find(Formal);
      if (SmryIt != AllSmry->InSmrys.end()) {
        for (auto *CalleeInSmry : SmryIt->second) {
          InputSmryMap[std::make_pair(ActualArg, GraphCS)].insert(CalleeInSmry);
        }
      }
    }
  }
}

void FunctionAnalyzer::initSourceWrapperSummary(
    const GuardedValueFlowCallSite *GraphCS, Function *Callee,
    const GuardedValueFlowNode *ActualArg, std::size_t ArgIndex,
    bool CommonActual) {
  assert(ActualArg && Callee);

  if (GraphCS->isBackEdge(Callee)) {
    return;
  }

  auto It = Parent->FuncSmryMap.find(Callee);
  if (It == Parent->FuncSmryMap.end()) {
    // This may happen in following two cases
    // (1) recursive call (2) empty function
    return;
  }

  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);

  auto *CalleeGraph = Parent->getGraph(Callee);
  size_t NumArgument = CommonActual ? CalleeGraph->getNumCommonArgument()
                                    : CalleeGraph->getNumPseudoArgument();
  if (ArgIndex < NumArgument) {
    const GuardedValueFlowNode *Formal =
        CommonActual
            ? (const GuardedValueFlowNode *)CalleeGraph->getCommonArgument(
                  ArgIndex)
            : (const GuardedValueFlowNode *)CalleeGraph->getPseudoArgument(
                  ArgIndex);

    auto SmryIt = AllSmry->TaintSourceWrapperSmrys.find(Formal);
    if (SmryIt != AllSmry->TaintSourceWrapperSmrys.end()) {
      for (auto *CalleeInSmry : SmryIt->second) {
        SourceWrapperSmryMap[GraphCS][ActualArg].insert(CalleeInSmry);
      }
    }
  } else {
    assert(CommonActual);
    for (size_t AI = 0, AE = CalleeGraph->getNumVarArgument(); AI != AE; AI++) {
      auto *Formal = CalleeGraph->getVarArgument(AI);
      assert((nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                         GuardedValueFlowNode>(Formal)));
      auto SmryIt = AllSmry->TaintSourceWrapperSmrys.find(Formal);
      if (SmryIt != AllSmry->TaintSourceWrapperSmrys.end()) {
        for (auto *CalleeInSmry : SmryIt->second) {
          SourceWrapperSmryMap[GraphCS][ActualArg].insert(CalleeInSmry);
        }
      }
    }
  }
}

void FunctionAnalyzer::matchFormalActual(
    SummaryBase *Smry, const GuardedValueFlowCallSite *GraphCS,
    const std::string &RenameSuffix) {
  // mapping actual to formal
  auto *CalleeGraph = Parent->getGraph(Smry->getFunction());
  for (auto *Formal : Smry->getInputs()) {
    const std::string &FormalDesc = detail::encodingSymbol(Formal);
    SMTExpr FormalExpr = Solver->getSMTFactory().createBitVecConst(
        FormalDesc + RenameSuffix,
        Parent->DL->getTypeSizeInBits(Formal->getType()));
    if (nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                   GuardedValueFlowNode>(Formal)) {
      unsigned VarActualArgStart = CalleeGraph->getNumCommonArgument();
      unsigned VarActualArgEnd = GraphCS->getCommonInputs().size();
      if (VarActualArgStart < VarActualArgEnd) {
        unsigned Idx = VarActualArgStart;
        SMTExpr Mapping = (FormalExpr == Solver->getOrInsertExpr(
                                             GraphCS->getCommonInputs()[Idx]));
        for (; Idx < VarActualArgEnd; Idx++) {
          auto *ActualNode = GraphCS->getCommonInputs()[Idx];
          assert(ActualNode);
          Mapping =
              Mapping || (FormalExpr == Solver->getOrInsertExpr(ActualNode));

          Solver->addAll(Solver->getDataDeps(ActualNode));
        }
        Solver->add(Mapping);
      }
    } else {
      const GuardedValueFlowNode *ActualNode = nullptr;
      if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                     GuardedValueFlowNode>(Formal)) {
        ActualNode = GraphCS->getCommonInput(Formal->getIndex());
      } else {
        ActualNode =
            GraphCS->getPseudoInput(Smry->getFunction(), Formal->getIndex());
      }
      if (!ActualNode) {
        continue;
      }

      SMTExpr Mapping = (FormalExpr == Solver->getOrInsertExpr(ActualNode));
      Solver->add(Mapping);

      Solver->addAll(Solver->getDataDeps(ActualNode));
    }
  }
}

// Helper to check if a call site has relevant summaries to inline.
bool FunctionAnalyzer::hasSummary(const GuardedValueFlowCallSite *GraphCS,
                                  const GuardedValueFlowNode *Node) {
  assert(GraphCS);
  Function *Callee = GraphCS->getCalledFunction();
  if (Callee == F) {
    return false;
  }

  auto SrcIt = Sources.find(GraphCS);
  if (SrcIt != Sources.end() &&
      SrcIt->second.count(dyn_cast<GuardedValueFlowNode>(Node))) {
    return true;
  }

  auto InSmryIt = InputSmryMap.find(std::make_pair(Node, GraphCS));
  if (InSmryIt != InputSmryMap.end() && !InSmryIt->second.empty()) {
    return true;
  }

  auto SourceWrapperIt = SourceWrapperSmryMap.find(GraphCS);
  if (SourceWrapperIt != SourceWrapperSmryMap.end()) {
    auto SmrySetIt = SourceWrapperIt->second.find(Node);
    if (SmrySetIt != SourceWrapperIt->second.end()) {
      return !SmrySetIt->second.empty();
    }
  }

  for (auto CalleeIt = GraphCS->getCallees().begin(),
            CalleeEnd = GraphCS->getCallees().end();
       CalleeIt != CalleeEnd; ++CalleeIt) {
    Function *Callee = *CalleeIt;
    auto NativeRange4 = callOutputs(GraphCS, Callee);
    for (auto It = NativeRange4.begin(), E = NativeRange4.end(); It != E;
         ++It) {
      auto OutSmryIt = OutputSmryMap.find(std::make_tuple(Node, *It, Callee));

      if (OutSmryIt != OutputSmryMap.end() && !OutSmryIt->second.empty()) {
        return true;
      }
    }
  }

  return false;
}

bool FunctionAnalyzer::inlineReturnSymbolicSummary(
    const GuardedValueFlowCallOutputNode *Node) {
  if (!GSAFOptions::EnableCSSymSummary) {
    return false;
  }

  assert(Node);
  assert(Node->getGraph() == Graph);

  if (CallSiteOutputCache.contains(Node)) {
    return false;
  }

  const GuardedValueFlowCallSite *CS = callSite(Node);
  assert(CS);
  int CSIdx = -1;
  if (nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoOutput,
                 GuardedValueFlowCallOutputNode>(Node)) {
    CSIdx = Node->getIndex();
  }

  Function *Callee = CS->getCalledFunction();
  if (!Callee || Callee == F || Callee->empty()) {
    // only consider direct call
    return false;
  }

  if (CS->isBackEdge(Callee)) {
    return false;
  }

  const GuardedValueFlowReturnNode *RetNodeInCallee = nullptr;
  if (CSIdx >= 0) {
    RetNodeInCallee = Parent->getGraph(Callee)->getPseudoReturn(CSIdx);
  } else {
    RetNodeInCallee = Parent->getGraph(Callee)->getCommonReturn();
  }

  auto It = Parent->FuncSmryMap.find(Callee);
  assert(It != Parent->FuncSmryMap.end());
  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);
  auto SymSmryPair = AllSmry->RetSmrys.find(RetNodeInCallee);
  if (SymSmryPair == AllSmry->RetSmrys.end()) {
    // This may happen in SCC.
    return false;
  }

  std::string RenamingSuffix = format_str("_CS%p", CS->getInstruction());

  for (const SummaryCacheItem &item :
       SymSmryPair->second->getNonSymDepsCache()) {
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  }
  for (const SummaryCacheItem &item : SymSmryPair->second->getSymbDepsCache()) {
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  }

  matchFormalActual(SymSmryPair->second, CS, RenamingSuffix);

  SMTExpr FormalRetExpr = Solver->getSMTFactory().createBitVecConst(
      detail::encodingSymbol(SymSmryPair->first) + RenamingSuffix,
      Parent->DL->getTypeSizeInBits(SymSmryPair->first->getType()));
  SMTExpr NodeExpr = Solver->getOrInsertExpr(Node);

  assert(FormalRetExpr.isBitVector() && NodeExpr.isBitVector());
  assert(FormalRetExpr.getBitVecSize() == NodeExpr.getBitVecSize());

  Solver->add(FormalRetExpr == NodeExpr);
  return true;
}

bool FunctionAnalyzer::inlineReturnSymbolicSummary(
    Vulnerability::ValueSitePairType Src) {
  if (!GSAFOptions::EnableCSSymSummary) {
    return false;
  }

  const GuardedValueFlowCallOutputNode *CurrentSrcSiteOutput = nullptr;
  if (Src.second) {
    if (auto *SrcSite = dyn_cast<GuardedValueFlowCallSite>(Src.second)) {
      CurrentSrcSiteOutput = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
          SrcSite->getCommonOutput());
    }
  }

  bool Inlined = false;
  unsigned NumToInline = GSAFOptions::InlineCSSymDepth;
  unsigned NumInlined = 0;

  if (NumInlined == NumToInline) {
    return false;
  }

  // Iterators in Range may become invalid, because the container
  // may become larger/smaller during inline.
  std::list<const GuardedValueFlowCallOutputNode *> CommonCSOCache;
  std::list<const GuardedValueFlowCallOutputNode *> PseudoCSOCache;

  using RangeTy = std::pair<
      std::vector<const GuardedValueFlowCallOutputNode *>::iterator,
      std::vector<const GuardedValueFlowCallOutputNode *>::iterator>;
  auto doInitialization = [&CommonCSOCache, &PseudoCSOCache,
                           CurrentSrcSiteOutput, this](RangeTy Range) {
    CommonCSOCache.clear();
    PseudoCSOCache.clear();

    for (auto &It = Range.first; It != Range.second; ++It) {
      auto *CSO = *It;
      if (Function *CSOFunc = callSite(CSO)->getCalledFunction()) {
        // No summary for an output of a library function call
        if (CSOFunc->empty()) {
          continue;
        }
      }

      if (nodeOfKind<GuardedValueFlowNode::Kind::CallSiteCommonOutput,
                     GuardedValueFlowCallOutputNode>(CSO)) {
        if (Sources.count(callSite(CSO)) ||
            SourceWrapperSmryMap.count(callSite(CSO))) {
          if (CommonCSOCache.front() == CurrentSrcSiteOutput) {
            auto It = CommonCSOCache.begin();
            ++It;
            CommonCSOCache.insert(It, CSO);
          } else {
            CommonCSOCache.push_front(CSO);
          }
        } else {
          CommonCSOCache.push_back(CSO);
        }
      } else {
        PseudoCSOCache.push_back(CSO);
      }
    }
  };

  auto doInline =
      [this, &Inlined, &NumToInline, &NumInlined](
          std::list<const GuardedValueFlowCallOutputNode *> &CSOCache) {
        for (auto *CSO : CSOCache) {
          if (inlineReturnSymbolicSummary(CSO)) {
            NumInlined++;

            if (!Inlined)
              Inlined = true;

            if (NumInlined == NumToInline) {
              break;
            }
          }
        }
      };

  doInitialization(Solver->getUsedCallSiteOutputs(false));

  while (!CommonCSOCache.empty() || !PseudoCSOCache.empty()) {
    doInline(CommonCSOCache);

    if (NumInlined == NumToInline) {
      break;
    }

    doInline(PseudoCSOCache);

    if (NumInlined == NumToInline) {
      break;
    }

    // reinit
    doInitialization(Solver->getUsedCallSiteOutputs(false));
  }

  if (NumInlined == NumToInline) {
    // The limit is reached. Get one more time to notify the remaining ones
    // will not need to be inlined.
    Solver->getUsedCallSiteOutputs(false);
  }
  return Inlined;
}

void FunctionAnalyzer::buildReturnSymbolicSummary() {
  if (!GSAFOptions::EnableCSSymSummary) {
    return;
  }

  Solver->reset();
  resetState();

  for (auto It = Graph->return_begin(), E = Graph->return_end(); It != E;
       ++It) {
    TimeChecker->check();

    const GuardedValueFlowReturnNode *Ret = *It;
    assert(Ret);

    pushState();
    Solver->push();
    Solver->addAll(Solver->getDataDeps(Ret));

    inlineReturnSymbolicSummary({nullptr, nullptr});

    SymbolicSummary *SymSmry = new SymbolicSummary(F, GSAFOptions::InlineDepth);

    addCachedConstraintsToSummary(SymSmry);

    Smry->RetSmrys.insert(std::make_pair(Ret, SymSmry));
    auto NonSymDep = Solver->assertions().toAndExpr();
    SymSmry->addNonSymDeps(SummaryCacheItem(&NonSymDep, "", 0));
    SymSmry->setInputs(Solver->getUsedFunctionArguments());
    Solver->pop();
    popState();
  }
}

// Inlines input summaries.
// Case 0: Normal input summary (backward propagation).
// Case 1: Continue forward search after inlining (used when searching from
// output to input). Case 2: Taint Source Wrapper summary (special handling).
void FunctionAnalyzer::inlineCalleeInSummary(
    const GuardedValueFlowCallSite *CS, InputSummary *CalleeInSmry,
    Vulnerability::ValueSitePairType Src,
    const GuardedValueFlowNode *ActualNode, unsigned Depth, int Case) {
  TimeChecker->check();

  if (Case != 1 && CalleeInSmry->getInlineDepth() >= GSAFOptions::InlineDepth) {
    return;
  }

  // translating, pruning, and mapping
  // copy to the current context and renaming for context sensitivity
  std::string RenamingSuffix = format_str("_CS%p", CS->getInstruction());
  std::unordered_map<std::string, SMTExpr> VariableMapping;

  pushState();
  Solver->push();

  for (SummaryCacheItem item : CalleeInSmry->getNonSymDepsCache())
    NonSymDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  for (SummaryCacheItem item : CalleeInSmry->getSymbDepsCache())
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));

  // mapping actual to formal
  matchFormalActual(CalleeInSmry, CS, RenamingSuffix);

  inlineReturnSymbolicSummary(Src);

  SMTSolver::SMTResultType Result = SMTSolver::SMTRT_Uncheck;
  Solver->push();
  Solver->addAll(CtrlConds.getCacheVector());
  if (ActualNode) {
    SMTExprVec Prereq = Solver->getSMTFactory().createEmptySMTExprVec();
    TSV->setPrerequisites(Solver, CS, TraceBuilder, Prereq);
    Solver->addAll(Prereq);
  }
  Result = Solver->check();

  if (Result == SMTSolver::SMTRT_Sat && canReport(Src) && Case == 0) {
    TraceBuilder.push();
    TraceBuilder.add(CS);
    TraceBuilder.add(CalleeInSmry->getTrace());
    auto Trace = TraceBuilder.snapshot();
    TraceBuilder.pop();

    tryReport(Trace);

    Solver->pop();
  } else {

    Solver->pop();

    if (Result == SMTSolver::SMTRT_Sat) {
      TraceBuilder.push();
      TraceBuilder.add(CS);
      TraceBuilder.add(CalleeInSmry->getTrace());

      if (Case == 1) {
        pushState();
        Solver->push();

        search(Src.first, nullptr, std::make_pair(Src.first, CS), Depth);

        Solver->pop();
        popState();
      } else if (Case == 0) {
        InputSummary *InSmry =
            createInputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                               &Solver->getUsedFunctionArguments(), Depth);

        addCachedConstraintsToSummary(InSmry);

        DEBUG_TRACE(dbgs() << *InSmry << "\n");

        Smry->InSmrys[isa<GuardedValueFlowArgumentNode>(Src.first) &&
                              !Src.second
                          ? Src.first
                          : nullptr]
            .insert(InSmry);
      } else {
        assert(Case == 2);
        InputSummary *InSmry =
            createInputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                               &Solver->getUsedFunctionArguments(), Depth);

        addCachedConstraintsToSummary(InSmry);

        DEBUG_TRACE(dbgs() << *InSmry << "\n");

        assert(isa<GuardedValueFlowArgumentNode>(Src.first) && !Src.second);
        Smry->TaintSourceWrapperSmrys[Src.first].insert(InSmry);
      }

      TraceBuilder.pop();
    } else {
      DEBUG_TRACE(dbgs() << "\t Inlining input summary leads unsat!\n");
    }
  }
  Solver->pop();
  popState();
  return;
}

// Inlines output summaries.
// Case 0: Normal output summary (forward propagation).
// Case 1: Continue backward search after inlining (used when searching from
// input to output).
void FunctionAnalyzer::inlineCalleeOutSummary(
    const GuardedValueFlowCallSite *CS, OutputSummary *OutSmry,
    const GuardedValueFlowCallOutputNode *CallSiteOutput,
    Vulnerability::ValueSitePairType Src, unsigned Depth, int Case) {
  auto *RetNode = OutSmry->getReturnNode();
  assert(RetNode);

  if (OutSmry->getInlineDepth() >= GSAFOptions::InlineDepth) {
    assert(isa<GuardedValueFlowNode>(OutSmry->getSourceNode()) &&
           "Such summaries have been removed during initialization!");
    return;
  }

  TraceBuilder.push();
  if (isa<GuardedValueFlowNode>(OutSmry->getSourceNode()) &&
      !OutSmry->getSourceSite()) {
    TraceBuilder.add(CS);
  }

  TraceBuilder.add(OutSmry->getTrace());
  if (Case == 0 && TSV->checkNode(CallSiteOutput, TraceBuilder)) {
    DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *CallSiteOutput << "...\n");
    TraceBuilder.pop();
    return;
  }

  Solver->push();
  pushState();

  // copy to the current context and renaming for context sensitivity
  std::string RenamingSuffix = format_str("_CS%p", CS->getInstruction());

  if (isa<GuardedValueFlowNode>(OutSmry->getSourceNode()) &&
      !OutSmry->getSourceSite()) {
  } else {
    // If it does not start from an argument, the initial control deps must be
    // satisfied otherwise the output summary cannot happen
    Solver->addAll(Solver->getCtrlDeps(CallSiteOutput));
  }

  for (const SummaryCacheItem &item : OutSmry->getNonSymDepsCache())
    NonSymDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  for (const SummaryCacheItem &item : OutSmry->getSymbDepsCache())
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));

  // mapping actual to formal
  matchFormalActual(OutSmry, CS, RenamingSuffix);

  // mapping callsite output and the return value of the callee
  if (RetNode) {
    SMTExpr CSOExpr = Solver->getOrInsertExpr(CallSiteOutput);
    SMTExpr RetExpr = Solver->getSMTFactory().createBitVecConst(
        detail::encodingSymbol(RetNode) + RenamingSuffix,
        CSOExpr.getBitVecSize());
    Solver->add(RetExpr == CSOExpr);
  }

  CallSiteOutputCache.add(CallSiteOutput);

  if (Case == 0) {
    bottomUpDepthFirstSearch(CallSiteOutput, nullptr, Src, Depth);
  } else {
    assert(Case == 1);

    auto *OutSmrySrc = OutSmry->getSourceNode();
    if (auto *Arg = dyn_cast<GuardedValueFlowArgumentNode>(OutSmrySrc)) {
      if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                     GuardedValueFlowNode>(OutSmrySrc)) {
        search(CS->getCommonInput(Arg->getIndex()), nullptr, Src, Depth);
      } else if (nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                            GuardedValueFlowNode>(OutSmrySrc)) {
        if (auto *PseudoInput =
                CS->getPseudoInput(OutSmry->getFunction(), Arg->getIndex())) {
          search(PseudoInput, nullptr, Src, Depth);
        }
      }
    } else {
      llvm_unreachable("When searching from output to input, the source of the "
                       "output summary trace must be an argument node!");
    }
  }

  Solver->pop();
  TraceBuilder.pop();
  popState();
  return;
}

} // namespace lotus::gsaf
