//===----------------------------------------------------------------------===//
//
// SymbolicExecutionWrapper is the outer integration boundary between the
// symbolic execution engine, the bug-checking layer, LLVM's legacy pass
// pipeline, and Lotus's BugReportMgr.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Integration/SymbolicExecutionWrapper.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowBuilder.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAAWrapper.h"
#include "SymbolicExecution/Checks/QueryEngine.h"
#include "SymbolicExecution/Engine/AnalysisDriver.h"
#include "SymbolicExecution/Models/TaintModel.h"
#include "SymbolicExecution/Support/AnalysisContext.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace SymbolicExecution;

#define DEBUG_TYPE "SymbolicExecutionWrapper"

namespace {

struct SymexBugTypeInfo {
  const char *name;
  BugDescription::BugImportance importance;
  BugDescription::BugClassification classification;
  const char *cwe;
  const char *primary_message;
};

SymexBugTypeInfo getBugTypeInfo(SymexBugType BugType) {
  switch (BugType) {
  case BUG_TY_BOF:
    return {"Symbolic Execution Buffer Overflow", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-120, CWE-122",
            "Potential buffer overflow"};
  case BUG_TY_DBZ:
    return {"Symbolic Execution Divide by Zero", BugDescription::BI_MEDIUM,
            BugDescription::BC_ERROR, "CWE-369", "Potential divide by zero"};
  case BUG_TY_INT_OVERFLOW:
    return {"Symbolic Execution Integer Overflow", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-190",
            "Potential integer overflow"};
  case BUG_TY_INT_UNDERFLOW:
    return {"Symbolic Execution Integer Underflow", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-191",
            "Potential integer underflow"};
  case BUG_TY_NULL_DEREF:
    return {"Symbolic Execution Null Dereference", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-476, CWE-690",
            "Potential null dereference"};
  case BUG_TY_SIGNED_INT_OVERFLOW:
    return {"Symbolic Execution Signed Integer Overflow",
            BugDescription::BI_HIGH, BugDescription::BC_SECURITY, "CWE-190",
            "Potential signed integer overflow"};
  case BUG_TY_SIGNED_INT_UNDERFLOW:
    return {"Symbolic Execution Signed Integer Underflow",
            BugDescription::BI_HIGH, BugDescription::BC_SECURITY, "CWE-191",
            "Potential signed integer underflow"};
  case BUG_TY_SHIFT_OVERFLOW:
    return {"Symbolic Execution Shift Overflow", BugDescription::BI_MEDIUM,
            BugDescription::BC_ERROR, "CWE-1335",
            "Potential invalid shift operation"};
  case BUG_TY_ARRAY_INDEX_OOB:
    return {"Symbolic Execution Array Index Out of Bounds",
            BugDescription::BI_HIGH, BugDescription::BC_SECURITY,
            "CWE-129, CWE-787", "Potential array index out of bounds"};
  case BUG_TY_UNINIT_READ:
    return {"Symbolic Execution Uninitialized Read", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-457",
            "Potential uninitialized read"};
  case BUG_TY_UAF:
    return {"Symbolic Execution Use After Free", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-416", "Potential use after free"};
  case BUG_TY_DOUBLE_FREE:
    return {"Symbolic Execution Double Free", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-415", "Potential double free"};
  case BUG_TY_NEGATIVE_ARRAY_INDEX:
    return {"Symbolic Execution Negative Array Index", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-129, CWE-839",
            "Potential negative array index"};
  case BUG_TY_INT_TRUNCATION:
    return {"Symbolic Execution Integer Truncation", BugDescription::BI_MEDIUM,
            BugDescription::BC_WARNING, "CWE-197",
            "Potential integer truncation"};
  case BUG_TY_UNDEF:
    break;
  }

  return {"Symbolic Execution Issue", BugDescription::BI_NA,
          BugDescription::BC_NA, "", "Potential symbolic execution issue"};
}

std::string renderValueLabel(const Value *Val) {
  if (!Val) {
    return "value";
  }
  if (Val->hasName()) {
    return Val->getName().str();
  }

  std::string Printed;
  raw_string_ostream OS(Printed);
  Val->printAsOperand(OS, false);
  return OS.str();
}

std::vector<NodeTag> getNodeTags(const Instruction *Inst, bool IsCallStep) {
  std::vector<NodeTag> Tags;
  if (IsCallStep || isa<CallInst>(Inst) || isa<InvokeInst>(Inst)) {
    Tags.push_back(NodeTag::CALL_SITE);
  }
  return Tags;
}

std::string getInstructionAccess(const Instruction *Inst, StringRef Fallback) {
  if (!Inst) {
    return Fallback.str();
  }
  if (isa<LoadInst>(Inst)) {
    return "load";
  }
  if (isa<StoreInst>(Inst)) {
    return "store";
  }
  if (isa<CallInst>(Inst) || isa<InvokeInst>(Inst)) {
    return "call";
  }
  return Fallback.str();
}

std::string renderTraceMessage(const TraceStep &Step) {
  switch (Step.TK) {
  case TraceStep::TRACE_STEP_CALL:
    return "Call site on the reported path";
  case TraceStep::TRACE_STEP_ALLOC:
    return "Memory object related to this report is allocated here";
  case TraceStep::TRACE_STEP_BUFFER_ACCESS:
    return "Bounds-relevant access involving " + renderValueLabel(Step.Val);
  case TraceStep::TRACE_STEP_DIV:
    return "Division uses " + renderValueLabel(Step.Val);
  case TraceStep::TRACE_STEP_ARITH:
    return "Arithmetic step involving " + renderValueLabel(Step.Val);
  case TraceStep::TRACE_STEP_DEREF:
    return "Pointer dereference involving " + renderValueLabel(Step.Val);
  }
  return "Symbolic execution trace step";
}

std::string getTraceAccess(const TraceStep &Step) {
  switch (Step.TK) {
  case TraceStep::TRACE_STEP_CALL:
    return "call";
  case TraceStep::TRACE_STEP_ALLOC:
    return "allocation";
  case TraceStep::TRACE_STEP_BUFFER_ACCESS:
    return getInstructionAccess(Step.Inst, "buffer");
  case TraceStep::TRACE_STEP_DIV:
    return "divide";
  case TraceStep::TRACE_STEP_ARITH:
    return "arithmetic";
  case TraceStep::TRACE_STEP_DEREF:
    return getInstructionAccess(Step.Inst, "dereference");
  }
  return "trace";
}

std::string renderTaintMessage(const TaintStep &Step) {
  switch (Step.TK) {
  case TaintStep::TAINT_STEP_SOURCE:
    return "Taint source reaches " + renderValueLabel(Step.V1);
  case TaintStep::TAINT_STEP_CALL:
    return "Taint crosses this call boundary";
  case TaintStep::TAINT_STEP_PROP:
    return "Taint propagates from " + renderValueLabel(Step.V1) + " to " +
           renderValueLabel(Step.V2);
  }
  return "Taint-related step";
}

std::string getTaintAccess(const TaintStep &Step) {
  switch (Step.TK) {
  case TaintStep::TAINT_STEP_SOURCE:
    return "source";
  case TaintStep::TAINT_STEP_CALL:
    return "call";
  case TaintStep::TAINT_STEP_PROP:
    return getInstructionAccess(Step.Inst, "propagation");
  }
  return "taint";
}

Instruction *
getPrimaryBugInstruction(const std::vector<TraceStep> &TraceSteps,
                         const std::vector<TaintStep> &TaintSteps) {
  if (!TraceSteps.empty()) {
    return TraceSteps.front().Inst;
  }
  if (!TaintSteps.empty()) {
    return TaintSteps.back().Inst;
  }
  return nullptr;
}

} // namespace

char SymbolicExecutionWrapper::ID = 0;
static RegisterPass<SymbolicExecutionWrapper>
    X(DEBUG_TYPE, "Lotus symbolic execution wrapper");

SymbolicExecutionWrapper::SymbolicExecutionWrapper(unsigned EnabledBugTypes)
    : ModulePass(ID), EnabledBugTypes(EnabledBugTypes) {}

SymbolicExecutionWrapper::~SymbolicExecutionWrapper() = default;

int SymbolicExecutionWrapper::getBugTypeId(SymexBugType BugType) {
  const auto Info = getBugTypeInfo(BugType);
  return BugReportMgr::get_instance().register_bug_type(
      Info.name, Info.importance, Info.classification, Info.cwe);
}

void SymbolicExecutionWrapper::emitBugReports(
    const BugReportCollector &Collector) const {
  auto Reports = Collector.getReports();
  auto &Mgr = BugReportMgr::get_instance();

  for (const auto &Record : Reports) {
    if (Record.BugTy == BUG_TY_UNDEF) {
      continue;
    }

    const auto Info = getBugTypeInfo(Record.BugTy);
    const int BugTypeId = getBugTypeId(Record.BugTy);
    if (BugTypeId < 0) {
      continue;
    }

    auto *Report = new BugReport(BugTypeId);

    int TraceLevel = 0;
    for (const auto &Step : Record.TaintSteps) {
      Report->append_step(
          Step.Inst, renderTaintMessage(Step), TraceLevel,
          getNodeTags(Step.Inst, Step.TK == TaintStep::TAINT_STEP_CALL),
          getTaintAccess(Step));
      if (Step.TK == TaintStep::TAINT_STEP_CALL) {
        ++TraceLevel;
      }
    }

    for (auto It = Record.TraceSteps.rbegin(); It != Record.TraceSteps.rend();
         ++It) {
      if (It->Inst == nullptr) {
        continue;
      }
      if (It == Record.TraceSteps.rbegin() && Record.TraceSteps.size() == 1) {
        continue;
      }
      if (It == Record.TraceSteps.rend() - 1) {
        continue;
      }

      Report->append_step(
          It->Inst, renderTraceMessage(*It), TraceLevel,
          getNodeTags(It->Inst, It->TK == TraceStep::TRACE_STEP_CALL),
          getTraceAccess(*It));
      if (It->TK == TraceStep::TRACE_STEP_CALL) {
        ++TraceLevel;
      }
    }

    Instruction *BugInst =
        getPrimaryBugInstruction(Record.TraceSteps, Record.TaintSteps);
    Report->append_step(BugInst, Info.primary_message, TraceLevel,
                        getNodeTags(BugInst, false), "bug");
    Report->set_conf_score(85);
    Report->add_metadata("checker", "SymbolicExecutionWrapper");
    Report->add_metadata("engine", "SymbolicExecution");
    if (*Info.cwe != '\0') {
      Report->add_metadata("cwe", Info.cwe);
    }

    Mgr.insert_report(BugTypeId, Report, true);
  }
}

void SymbolicExecutionWrapper::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<gsa::ControlDependenceAnalysisPass>();
  AU.addRequired<gsa::GateAnalysisPass>();
  AU.addRequired<lotus::gvfg::GuardedValueFlowGraphBuilderPass>();
  AU.addRequired<lotus::gvfg::LotusAAWrapper>();
}

bool SymbolicExecutionWrapper::runOnModule(Module &M) {
  auto &Builder = getAnalysis<lotus::gvfg::GuardedValueFlowGraphBuilderPass>();
  AnalysisContext Ctx(&M, &M.getDataLayout(), &Builder);
  TaintModel TaintSpec;
  BugReportCollector Collector;

  AnalysisDriver Driver(Ctx, TaintSpec, [&](Function *) {
    return std::make_unique<QueryEngine>(EnabledBugTypes, Collector);
  });
  Driver.runOnModule(&M);
  emitBugReports(Collector);
  return false;
}
