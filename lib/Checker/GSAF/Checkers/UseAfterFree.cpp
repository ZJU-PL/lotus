#include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/API/VulnerabilityRegistry.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/GSAF/Report/ReportDecorator.h"
#include "Checker/GSAF/Support/CheckerServices.h"

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/CommandLine.h>

namespace lotus {
namespace gsaf {
using namespace llvm;
using namespace std;
using namespace lotus::reporting;

using namespace llvm;

static cl::opt<bool>
    DisableHeapPtrAnalysis("gsaf.uaf-disable-heap-analysis",
                           cl::sub(lotus::checker::tooling::gsafSubCommand()),
                           cl::desc("Using a lightweight heap pointer analysis "
                                    "to exclude must-not-heap pointers."),
                           cl::Hidden, cl::init(false));

// Customized report Decorator

RegisterVulnerabilityDescriptionEvent(GSAFUAFDesc, "Use After Free",
                                      "CWE-415, CWE-416");
class GSAFUAFReportDecorator : public GSAFTaintReportDecorator {
protected:
  virtual void postProcess() override {
    appendLastStepDescription(ReportDecorator::getGSAFUAFDescEvent());
  }
};

class UseAfterFree : public TaintStyleVulnerability {
private:
  TargetLibraryInfoWrapperPass *TLI = nullptr;
  GSAFChecker *CRA = nullptr;
  std::unique_ptr<HeapPointerAnalysis> HPA;

public:
  UseAfterFree() : TaintStyleVulnerability(BUG_UAF(GET_FULL_NAME)) {}

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

  virtual void getAnalysisUsage(AnalysisUsage &AU) override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();

    AU.addRequired<DyckAliasAnalysis>();
    AU.addRequired<GSAFModels>();
  }

  virtual void initializeAnalysis(Pass *P) override {
    TLI = &P->getAnalysis<TargetLibraryInfoWrapperPass>();
    CRA = static_cast<GSAFChecker *>(P);
    HPA = heapAnalysis(P);
  }

  virtual bool isSinkFunction(const GuardedValueFlowGraph *G,
                              const GuardedValueFlowNode *Arg) const override {
    return Arg and
           nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                      GuardedValueFlowNode>(Arg) and
           G == Arg->getGraph();
  }

  virtual GSAFReportDecorator *allocNewDecorator() override {
    return new GSAFUAFReportDecorator;
  }
};

void UseAfterFree::setSources(const GuardedValueFlowGraph *Graph,
                              std::vector<ValueSitePairType> &Sources) {
  for (auto It = Graph->sites().begin(), E = Graph->sites().end(); It != E;
       It++) {
    GuardedValueFlowSite *Site = It->get();
    Value *sValue = Site->getInstruction();
    GuardedValueFlowCallSite *CS = dyn_cast<GuardedValueFlowCallSite>(Site);
    if (!CS)
      continue;
    if (isFreeCall(cast<Instruction>(sValue), TLI)) {
      GuardedValueFlowNode *Node =
          Graph->findNode(cast<CallBase>(sValue)->getArgOperand(0));
      if (!Node)
        continue;
      Sources.emplace_back(Node, CS);
    } else if (isa_and_nonnull<CallBase>(sValue)) {
      if ((calledFunction(sValue) != nullptr)) {
        if ((calledName(sValue) == "realloc" ||
             calledName(sValue) == "reallocf") &&
            calledFunction(sValue)->arg_size() == 2) {
          GuardedValueFlowNode *Node =
              Graph->findNode(cast<CallBase>(sValue)->getArgOperand(0));
          if (!Node)
            continue;
          Sources.emplace_back(Node, CS);
        }
      }
    }
  }
}

void UseAfterFree::setPrerequisites(
    GuardedValueFlowSolver *Solver, const GuardedValueFlowSite *CurrSite,
    const VulnerabilityTraceBuilder &TraceHistory, SMTExprVec &Prerequisites) {
  if (TraceHistory.size() == 1) {
    // It is a special case to set source condition
    // p = realloc(q, ...): p != 0 && p != q
    // p = reallocf(q, ...): p != q

    auto *call = cast<GuardedValueFlowCallSite>(CurrSite);
    Function *F = call->getCalledFunction();
    Instruction *CallInst = CurrSite->getInstruction();

    if (F && F->arg_size() == 2 &&
        F->getReturnType() == call->getCommonInput(0)->getType()) {
      auto *PNode = CurrSite->getGraph()->findNode(CallInst);
      assert(PNode);
      SMTExpr P = Solver->getOrInsertExpr(PNode);
      SMTExpr Q = Solver->getOrInsertExpr(
          TraceHistory.recentObjAs<GuardedValueFlowNode>());

      if (F->getName() == "realloc") {
        Prerequisites.push_back(P != 0 && P != Q);
      } else if (F->getName() == "reallocf") {
        Prerequisites.push_back(P != Q);
      }
    }
  } else {
    Prerequisites.push_back(
        Solver->getOrInsertExpr(
            TraceHistory.recentObjAs<GuardedValueFlowNode>()) != 0);
  }
}

bool UseAfterFree::checkNode(const GuardedValueFlowNode *Node,
                             const VulnerabilityTraceBuilder &TraceHistory) {
  assert(Node);
  return false;
}

Vulnerability::SiteType
UseAfterFree::checkSite(const GuardedValueFlowSite *CurSite,
                        const VulnerabilityTraceBuilder &TraceHistory) {
  auto *CurNode = TraceHistory.recentObjAs<GuardedValueFlowNode>();
  auto *SrcNode = TraceHistory.sourceNode();
  auto *SrcSite = TraceHistory.sourceSite();

  if (checkNode(CurNode, TraceHistory)) {
    return ST_Others;
  }

  // we don't use gep site in UAF
  if (isa<GuardedValueFlowGEPReferenceSite>(CurSite)) {
    return ST_Others;
  }

  if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                 GuardedValueFlowNode>(SrcNode) &&
      !SrcSite) {
    // search from a common argument
    if (isa<GuardedValueFlowCallSite>(CurSite)) {
      return ST_Call;
    } else if (isa<GuardedValueFlowReturnSite>(CurSite)) {
      return ST_Return;
    } else {
      assert(!isa<GuardedValueFlowCallSite>(CurSite) &&
             !isa<GuardedValueFlowReturnSite>(CurSite));
      return ST_Others;
    }
  } else if (nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                        GuardedValueFlowNode>(SrcNode) &&
             !SrcSite) {
    // search from a pseudo argument
    if (auto *GraphCS = dyn_cast<GuardedValueFlowCallSite>(CurSite)) {
      if (GraphCS->isCommonInput(CurNode)) {
        return (SiteType)(ST_Sink | ST_Call);
      } else {
        return ST_Call;
      }
    } else if (isa<GuardedValueFlowReturnSite>(CurSite)) {
      if (nodeOfKind<GuardedValueFlowNode::Kind::CommonReturn,
                     GuardedValueFlowReturnNode>(CurNode)) {
        return (SiteType)(ST_Sink | ST_Return);
      } else {
        assert((nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn,
                           GuardedValueFlowReturnNode>(CurNode)));
        return ST_Return;
      }
    } else {
      assert(!isa<GuardedValueFlowCallSite>(CurSite) &&
             !isa<GuardedValueFlowReturnSite>(CurSite));
      return ST_Sink;
    }
  } else if (SrcSite->getParentFunction() != CurSite->getParentFunction()) {
    // search from a pseudo output node with an output summary
    if (auto *GraphCS = dyn_cast<GuardedValueFlowCallSite>(CurSite)) {
      if (GraphCS->isCommonInput(CurNode)) {
        return ST_Sink;
      } else {
        return ST_Call;
      }
    } else if (isa<GuardedValueFlowReturnSite>(CurSite)) {
      if (nodeOfKind<GuardedValueFlowNode::Kind::CommonReturn,
                     GuardedValueFlowReturnNode>(CurNode)) {
        return ST_Sink;
      } else {
        assert((nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn,
                           GuardedValueFlowReturnNode>(CurNode)));
        return ST_Return;
      }
    } else {
      assert(!isa<GuardedValueFlowCallSite>(CurSite) &&
             !isa<GuardedValueFlowReturnSite>(CurSite));
      return ST_Sink;
    }
    // } else if (SrcSite->getInstruction() != CurSite->getInstruction()) {
  } else {
    Value *sValueSrc = SrcSite->getInstruction();
    Value *sValueCur = CurSite->getInstruction();
    if (sValueSrc && sValueCur) {
      if (!(sValueSrc == sValueCur)) {
        // search from free or free wrappers
        bool AfterSrcSite = CRA->isReachable(SrcSite->getInstruction(),
                                             CurSite->getInstruction());

        if (!AfterSrcSite && !CRA->isReachable(CurSite->getInstruction(),
                                               SrcSite->getInstruction())) {
          return ST_Others;
        }

        if (auto *GraphCS = dyn_cast<GuardedValueFlowCallSite>(CurSite)) {
          if (GraphCS->isCommonInput(CurNode)) {
            return AfterSrcSite ? ST_Sink : ST_Call;
          } else {
            return ST_Call;
          }
        } else if (isa<GuardedValueFlowReturnSite>(CurSite)) {
          assert(AfterSrcSite);
          if (nodeOfKind<GuardedValueFlowNode::Kind::CommonReturn,
                         GuardedValueFlowReturnNode>(CurNode)) {
            return ST_Return;
          }
        } else {
          assert(!isa<GuardedValueFlowCallSite>(CurSite) &&
                 !isa<GuardedValueFlowReturnSite>(CurSite));
          return AfterSrcSite ? ST_Sink : ST_Others;
        }
      }
    }
  }

  return ST_Others;
}

void registerUseAfterFree() {
  static VulnerabilityRegistry<UseAfterFree> X(
      "gsaf.use-after-free", "Run path-sensitive use-after-free checker.",
      "gsaf.stable");
}

} // namespace gsaf
} // namespace lotus
