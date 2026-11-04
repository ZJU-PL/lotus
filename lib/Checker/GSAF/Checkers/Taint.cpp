#include "Checker/Framework/Subcommands.h"
#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/API/VulnerabilityRegistry.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/GSAF/Report/ReportDecorator.h"
#include "Checker/GSAF/Support/CheckerServices.h"

#include <llvm/IR/Function.h>

#include <stack>

namespace lotus {
namespace gsaf {
using namespace llvm;
using namespace std;
using namespace lotus::reporting;

using namespace llvm;

static cl::opt<bool> EnableExceptionChecking(
    "gsaf.taint-enable-exception-checking",
    cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc(
        "Enable to check if a source-sink pair is excluded as an exception."),
    cl::Hidden, cl::init(true));

RegisterVulnerabilityDescriptionEvent(
    GSAFTaintDesc, "Taint-Style Vulnerability",
    "CWE-15, CWE-23, CWE-78, CWE-90, CWE-123, CWE-256, CWE-319, CWE-426, "
    "CWE-427, CWE-591");
class GSAFTaintBugReportDecorator : public GSAFTaintReportDecorator {
protected:
  virtual void postProcess() override {
    appendLastStepDescription(ReportDecorator::getGSAFTaintDescEvent());
  }
};

class TaintAnalysis : public TaintStyleVulnerability {
private:
  // FalconAA* Falcon = nullptr;
  GSAFChecker *CRA = nullptr;
  GSAFModels *TaintSpec = nullptr;
  GSAFModels *MemorySpec = nullptr;

public:
  TaintAnalysis() : TaintStyleVulnerability(BUG_TAINT(GET_FULL_NAME)) {
    // w32: CWE-15, 90, 256, 319, 591
    // unix: CWE-23, 78, 123, 426, 427
  }

  virtual void
  transfer(const GuardedValueFlowSite *Site, const GuardedValueFlowNode *Arg,
           std::vector<const GuardedValueFlowNode *> &TransferDsts) override {
    Instruction *SiteInst = Site->getInstruction();
    auto *Graph = Site->getGraph();
    // if it is a binary operation
    if (isa<BinaryOperator>(SiteInst)) {
      // now let's assume the taint will always be transferred to the result
      // operand of a binary operation there can be cases like x = y - y such
      // that x's value does not depend on y, which is tainted
      TransferDsts.push_back(Graph->findNode(SiteInst));
      return;
    }

    // if the instruction loads data from tainted memory address
    // for handling cases like pointer operations f = tainted -> field (f should
    // be tainted)
    if (isa<LoadInst>(SiteInst)) {
      TransferDsts.push_back(Graph->findNode(SiteInst));
      return;
    }

    auto *CS = dyn_cast<CallBase>(SiteInst);
    if (!CS) {
      // if it is not a call site
      return;
    }

    if ((!SiteInst->getType()->isVoidTy()) &&
        (CS->getCalledFunction() == nullptr ||
         CS->getCalledFunction()->isDeclaration())) {
      // always transfer to ret
      TransferDsts.push_back(Graph->findNode(SiteInst));
    }

    auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(SiteInst);

    Function *Callee = nullptr;

    if (GraphCS && GraphCS->getCallees().size() == 1) {
      Callee = (*GraphCS->getCallees().begin());
    }

    if (!Callee)
      return;

    if (Callee->isIntrinsic()) {
      switch (Callee->getIntrinsicID()) {
      case Intrinsic::memset:
      case Intrinsic::memmove:
      case Intrinsic::memcpy:
        if (Arg == Graph->findNode(CS->getArgOperand(1))) {
          TransferDsts.push_back(Graph->findNode(CS->getArgOperand(0)));
        }
      }
    }
  };

  virtual void setSources(const GuardedValueFlowGraph *Graph,
                          std::vector<ValueSitePairType> &Sources) override;
  virtual void setPrerequisites(GuardedValueFlowSolver *Solver,
                                const GuardedValueFlowSite *CurrSite,
                                const VulnerabilityTraceBuilder &TraceHistory,
                                SMTExprVec &Prerequisites) override;
  virtual bool
  checkNode(const GuardedValueFlowNode *Node,
            const VulnerabilityTraceBuilder &TraceHistory) override;
  virtual SiteType
  checkSite(const GuardedValueFlowSite *CurrSite,
            const VulnerabilityTraceBuilder &TraceHistory) override;
  virtual bool checkTrace(std::shared_ptr<VulnerabilityTrace> &Trace) override;

  virtual void getAnalysisUsage(AnalysisUsage &AU) override {
    AU.addRequired<GSAFModels>();
  }

  virtual void initializeAnalysis(Pass *P) override {
    CRA = static_cast<GSAFChecker *>(P);
    TaintSpec = &P->getAnalysis<GSAFModels>();
    MemorySpec = &P->getAnalysis<GSAFModels>();
  }

  virtual bool isSinkFunction(const GuardedValueFlowGraph *G,
                              const GuardedValueFlowNode *Arg) const override {
    Function *F = G->getBaseFunc();
    return isSinkFunction(F);
  }

  virtual GSAFReportDecorator *allocNewDecorator() override {
    return new GSAFTaintBugReportDecorator;
  }

private:
  bool isSinkFunction(Function *F) const {
    if (F) {
      if (TaintSpec->isFunctionAsSink(F)) {
        return true;
      } else if (F->getName().startswith("llvm.mem")) {
        return true;
      } else if (MemorySpec->is_malloc_function(F)) {
        return true;
      }
    }
    return false;
  }

  bool isSink(Value *CS, Value *Arg) const {
    if ((calledFunction(CS) != nullptr)) {
      auto *SinkArgs = TaintSpec->getTaintSinkArguments(calledFunction(CS));
      if (SinkArgs) {
        size_t ArgIndex = 0;
        bool Found = false;
        for (; ArgIndex < cast<CallBase>(CS)->arg_size(); ++ArgIndex) {
          if (isCallArgument(Arg, CS, ArgIndex)) {
            Found = true;
            break;
          }
        }
        if (Found) {
          for (auto I : *SinkArgs) {
            if (I == (int)ArgIndex) {
              return true;
            } else if (I < 0 && ((int)ArgIndex) >= -I) {
              return true;
            }
          }
        }
      } else if (calledName(CS).startswith("llvm.mem") &&
                 !isCallArgument(Arg, CS, 0)) {
        return true;
      } else if (MemorySpec->is_malloc_function(calledFunction(CS))) {
        if (calledName(CS).startswith("realloc")) {
          return !isCallArgument(Arg, CS, 0);
        } else {
          return true;
        }
      }
    }

    return false;
  }
};

void TaintAnalysis::setSources(const GuardedValueFlowGraph *Graph,
                               std::vector<ValueSitePairType> &Sources) {
  for (auto It = Graph->valueNodes().begin(), E = Graph->valueNodes().end();
       It != E; It++) {
    GuardedValueFlowNode *Node = It->second;
    Value *sValue = Node->getLLVMValue();
    if (!sValue)
      continue;
    GuardedValueFlowCallSite *CS = Graph->findSite<GuardedValueFlowCallSite>(
        dyn_cast<Instruction>(sValue));
    if (!CS)
      continue;
    Value *sValueCS = CS->getInstruction();
    if (!sValueCS)
      continue;
    if (!isa_and_nonnull<CallBase>(sValueCS))
      continue;
    if ((calledFunction(sValueCS) != nullptr)) {
      if (TaintSpec->isFunctionRetAsSource(calledFunction(sValueCS))) {
        auto *Output = CS->getCommonOutput();
        assert(Output);
        Sources.push_back(
            std::make_pair((const GuardedValueFlowNode *)Output, CS));
      }
      auto *TaintedArgs =
          TaintSpec->getTaintSourceArguments(calledFunction(sValueCS));
      if (!TaintedArgs)
        continue;
      for (auto It = TaintedArgs->begin(), End = TaintedArgs->end(); It != End;
           ++It) {
        int ArgIdx = *It;
        if (ArgIdx >= 0) {
          auto *TaintedArg = CS->getCommonInput(ArgIdx);
          Sources.push_back(std::make_pair(TaintedArg, CS));
        } else {
          int AbsArgIdx = -ArgIdx;
          for (int VarArgIdx = AbsArgIdx, End = CS->getCommonInputs().size();
               VarArgIdx < End; VarArgIdx++) {
            auto *TaintedPtr = CS->getCommonInput(VarArgIdx);
            Sources.push_back(std::make_pair(TaintedPtr, CS));
            for (auto UIt = TaintedPtr->useSites().begin(),
                      UEnd = TaintedPtr->useSites().end();
                 UIt != UEnd; UIt++) {
              const GuardedValueFlowSite *UseSite = *UIt;
              Value *sValueUS = UseSite->getInstruction();
              if (sValueUS && isa_and_nonnull<LoadInst>(sValueUS)) {
                GuardedValueFlowNode *LoadTaintedPtr =
                    Graph->findNode(UseSite->getInstruction());
                assert(isOperand(LoadTaintedPtr));
                Sources.push_back(std::make_pair(
                    (const GuardedValueFlowNode *)LoadTaintedPtr, CS));
              }
            }
          }
        }
      }
    }
  }

  Function *BaseFunc = Graph->getBaseFunc();
  for (auto &B : *BaseFunc) {
    for (auto &I : B) {
      auto *CS = dyn_cast<CallBase>(&I);
      if (!CS) {
        continue;
      }

      if (Function *Callee = CS->getCalledFunction()) {
        const auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(&I);
        if (!GraphCS)
          continue;

        if (TaintSpec->isFunctionRetAsSource(Callee)) {
          auto *Output = GraphCS->getCommonOutput();
          assert(Output);
          Sources.push_back(
              std::make_pair((const GuardedValueFlowNode *)Output, GraphCS));
        }

        auto *TaintedArgs = TaintSpec->getTaintSourceArguments(Callee);
        if (!TaintedArgs)
          continue;
        for (auto It = TaintedArgs->begin(), End = TaintedArgs->end();
             It != End; ++It) {
          int ArgIdx = *It;
          if (ArgIdx >= 0) {
            auto *TaintedArg = GraphCS->getCommonInput(ArgIdx);
            Sources.push_back(std::make_pair(TaintedArg, GraphCS));
          } else {
            int AbsArgIdx = -ArgIdx;
            for (int VarArgIdx = AbsArgIdx,
                     End = GraphCS->getCommonInputs().size();
                 VarArgIdx < End; VarArgIdx++) {
              auto *TaintedPtr = GraphCS->getCommonInput(VarArgIdx);
              Sources.push_back(std::make_pair(TaintedPtr, GraphCS));

              // FIXME
              for (auto UIt = TaintedPtr->useSites().begin(),
                        UEnd = TaintedPtr->useSites().end();
                   UIt != UEnd; UIt++) {
                const GuardedValueFlowSite *UseSite = *UIt;
                if (UseSite->getInstruction()->getOpcode() ==
                    Instruction::Load) {
                  GuardedValueFlowNode *LoadTaintedPtr =
                      Graph->findNode(UseSite->getInstruction());
                  assert(isOperand(LoadTaintedPtr));
                  Sources.push_back(std::make_pair(
                      (const GuardedValueFlowNode *)LoadTaintedPtr, GraphCS));
                }
              }
            }
          }
        }
      }
    }
  }
}

bool TaintAnalysis::checkNode(const GuardedValueFlowNode *Node,
                              const VulnerabilityTraceBuilder &TraceHistory) {
  return false;
}

Vulnerability::SiteType
TaintAnalysis::checkSite(const GuardedValueFlowSite *CurSite,
                         const VulnerabilityTraceBuilder &TraceHistory) {
  auto *SrcSite = TraceHistory.sourceSite();

  if (isa<GuardedValueFlowReturnSite>(CurSite)) {
    return ST_Return;
  } else if (const GuardedValueFlowCallSite *CS =
                 dyn_cast<GuardedValueFlowCallSite>(CurSite)) {
    auto *RecentNode = TraceHistory.recentObjAs<GuardedValueFlowNode>();
    Value *sValueRecent = RecentNode->getLLVMValue();
    Value *sValueCS = CS->getInstruction();
    if (sValueRecent && sValueCS && isSink(sValueCS, sValueRecent)) {
      // if source site and cur site are not in the same func
      // it should be search from a pseudo output node with
      // an output summary and, thus, cur site must be reachable.
      if (!SrcSite ||
          SrcSite->getParentFunction() != CurSite->getParentFunction() ||
          (CRA->isReachable(SrcSite->getInstruction(),
                            CurSite->getInstruction()) &&
           SrcSite != CurSite)) {
        return ST_Sink;
      } else {
        return ST_Others;
      }
    }

    // if source site and cur site are not in the same func
    // it should be search from a pseudo output node with
    // an output summary and, thus, cur site must be reachable.
    if (!SrcSite ||
        SrcSite->getParentFunction() != CurSite->getParentFunction() ||
        CRA->isReachable(SrcSite->getInstruction(),
                         CurSite->getInstruction()) ||
        CRA->isReachable(CurSite->getInstruction(),
                         SrcSite->getInstruction())) {
      if (SrcSite != CurSite)
        return ST_Call;
    }
  }

  return ST_Others;
}

void TaintAnalysis::setPrerequisites(
    GuardedValueFlowSolver *Solver, const GuardedValueFlowSite *CurrSite,
    const VulnerabilityTraceBuilder &TraceHistory, SMTExprVec &Prerequisites) {}

bool TaintAnalysis::checkTrace(std::shared_ptr<VulnerabilityTrace> &Trace) {
  if (!EnableExceptionChecking.getValue()) {
    return true;
  }

  auto *SrcNode = Trace->at(0);
  auto *SrcSite = Trace->at(1);
  assert(SrcNode);
  assert(SrcSite);
  assert(isOperand(SrcNode));
  assert(isa<GuardedValueFlowSite>(SrcSite));

  std::shared_ptr<VulnerabilityTrace> StartSubTrace =
      std::make_shared<VulnerabilityTrace>();
  StartSubTrace->push(SrcNode);
  StartSubTrace->push(SrcSite);

  if (Trace->at(2) != SrcNode && !Trace->at(3) &&
      isa<GuardedValueFlowArgumentNode>(Trace->at(2))) {
    // Find the taint wrapper
    std::stack<Function *> FrameStack;
    size_t I = 2;
    for (size_t E = Trace->get_length(); I < E; ++I) {
      auto *Obj = Trace->at(I);
      if (Obj) {
        if (isa<GuardedValueFlowArgumentNode>(Obj) && !Trace->at(I + 1)) {
          FrameStack.push(Obj->getGraph()->getBaseFunc());
        } else if (isa<GuardedValueFlowReturnSite>(Obj)) {
          FrameStack.pop();

          if (FrameStack.empty()) {
            break;
          }
        }

        if (Obj->getGraph()->getBaseFunc() != FrameStack.top()) {
          break;
        }

        StartSubTrace->push(Obj);
      } else {
        StartSubTrace->push(Obj);
      }
    }

    if (FrameStack.empty()) {
      StartSubTrace->clear();
      StartSubTrace->push(SrcNode);
      StartSubTrace->push(SrcSite);
    }
  }

  auto getFuncArgIndexPair = [](const GuardedValueFlowObject *Node,
                                const GuardedValueFlowObject *Site) {
    Function *CalledFunction = nullptr;
    int Index = -1;
    if (auto *CS = dyn_cast<GuardedValueFlowCallSite>(Site)) {
      CalledFunction = CS->getCalledFunction();
      for (size_t I = 0; I < CS->getCommonInputs().size(); ++I) {
        if (Node == CS->getCommonInput(I)) {
          Index = (int)I;
          break;
        }
      }
    }

    return std::make_pair(CalledFunction, Index);
  };

  const auto *RealSrcNode = StartSubTrace->at(StartSubTrace->get_length() - 2);
  const auto *RealSrcSite = StartSubTrace->at(StartSubTrace->get_length() - 1);
  auto Src = getFuncArgIndexPair(RealSrcNode, RealSrcSite);

  const auto *RealSinkNode = Trace->at(Trace->get_length() - 2);
  const auto *RealSinkSite = Trace->at(Trace->get_length() - 1);
  auto Sink = getFuncArgIndexPair(RealSinkNode, RealSinkSite);

  if (Src.first && Sink.first) {
    if (TaintSpec->isException(Src, Sink)) {
      return false;
    }
  }
  return true;
}

void registerTaintAnalysis() {
  static VulnerabilityRegistry<TaintAnalysis> X(
      "gsaf.taint", "Run path-sensitive taint checker.", "gsaf.experimental");
}

} // namespace gsaf
} // namespace lotus
