#pragma once

#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "Annotation/APISpec.h"
#include "Checker/Framework/CheckerDiagnostic.h"
#include "Checker/Framework/DiagnosticEvent.h"
#include "Checker/GSAF/API/Trace.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GSA/GSA.h"
#include "Utils/LLVM/CallUtils.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace lotus::gsaf {
using namespace lotus::reporting;
using namespace llvm;
using namespace std;

using namespace llvm;

class DefaultReportDecorator : public DiagnosticBuilder {
protected:
  // some primarily defined indices special arguments
  // use as the arg_idx parameter in corresponding functions if the argument to
  // process is special
  static const int ARG_IDX_GLOBAL = -1;
  static const int ARG_IDX_RETURN = -2;
  static const int ARG_IDX_UNCLASSIFIED = -3;
  static const int ARG_IDX_THIS_POINTER = -4;

public:
  DefaultReportDecorator(
      DiagnosticTransformations *transformation_rules = nullptr);
  ~DefaultReportDecorator();

protected:
  // for all functions below: first arg of type DiagnosticLocation is the
  // program point for the event to push

  // General processing of trace start
  virtual void pushTraceStartEvent(const DiagnosticLocation &);

  // Value "value_name" is used in function "function_name"
  virtual void
  pushGeneralValueUsedInCalleeEvent(const DiagnosticLocation &,
                                    const std::string &value_name,
                                    const std::string &function_name);

  // Take "branch_choice" branch
  virtual void pushTrueOrFalseBranchEvent(const DiagnosticLocation &,
                                          bool branch_choice);

  // Take any branch in a true/false branch
  virtual void pushTrueAndFalseBranchEvent(const DiagnosticLocation &);

  // Program flow, from "from_line" to "to line"
  virtual void pushPHIFlowToCurrentPointEvent(const DiagnosticLocation &,
                                              int from_line);
  virtual void pushGeneralPHIFlowEvent(const DiagnosticLocation &,
                                       int from_line, int to_line);

  // enter call site (caller side):
  // passing common argument/pseudo argument/ va-arg/ or simply get in function
  virtual void pushCallsiteCommonArgEvent(const DiagnosticLocation &,
                                          const std::string &arg_name,
                                          int arg_idx,
                                          const std::string &func_name);
  virtual void pushCallsiteCommonArgEvent(const DiagnosticLocation &,
                                          const DiagnosticText &arg_name,
                                          int arg_idx,
                                          const std::string &func_name);
  virtual void pushCallsitePseudoArgEvent(const DiagnosticLocation &,
                                          const std::string &full_access_path,
                                          const std::string &func_name,
                                          const std::string &arg_name,
                                          int arg_idx);
  virtual void pushCallsiteVaArgEvent(const DiagnosticLocation &,
                                      const std::string &arg_name,
                                      const std::string &func_name);
  virtual void pushCallsiteVaArgEvent(const DiagnosticLocation &,
                                      const DiagnosticText &arg_name,
                                      const std::string &func_name);
  virtual void pushCallsiteSimpleEnterEvent(const DiagnosticLocation &,
                                            const std::string &func_name = "");

  // return from callee (caller side)
  // simple return / caring the common return value / caring a pseudo return
  // value
  virtual void pushCallsiteEmptyRetEvent(const DiagnosticLocation &,
                                         const std::string &func_name);
  virtual void pushCallsiteCommonRetEvent(const DiagnosticLocation &,
                                          const std::string &ret_val_name,
                                          const std::string &func_name,
                                          bool is_left_value_call_expr = true);
  virtual void pushCallsitePseudoRetEvent(
      const DiagnosticLocation &, const std::string &full_access_path,
      const std::string &func_name, const std::string &arg_name, int arg_idx,
      const std::string &formal_arg_name, bool is_left_value_call_expr = true);

  // enter call site (callee side):
  // passing common argument/pseudo argument/ va-arg
  virtual void pushFunctionCommonArgEvent(const DiagnosticLocation &,
                                          const std::string &arg_name);
  virtual void pushFunctionPseudoArgEvent(const DiagnosticLocation &,
                                          const std::string &full_access_path,
                                          const std::string &arg_name,
                                          int arg_idx);
  virtual void pushFunctionVaArgEvent(const DiagnosticLocation &,
                                      const std::string &arg_name);

  // return from callee (callee side)
  // simple return / caring the common return value / caring a pseudo return
  // value
  virtual void pushFunctionEmptyRetEvent(const DiagnosticLocation &);
  virtual void pushFunctionCommonRetEvent(const DiagnosticLocation &,
                                          const std::string &ret_val_name);
  virtual void pushFunctionPseudoRetEvent(const DiagnosticLocation &,
                                          const std::string &full_access_path,
                                          const std::string &arg_name,
                                          int arg_idx);

  // General value action :
  // Value "value_name" alive/ value "value_name" used/ value "value_name" alive
  // and used/ program point executed
  virtual void pushGeneralValueAliveEvent(const DiagnosticLocation &,
                                          const std::string &value_name);
  virtual void pushGeneralValueUsedEvent(const DiagnosticLocation &,
                                         const std::string &value_name);
  virtual void pushGeneralValueAliveAndUsedEvent(const DiagnosticLocation &,
                                                 const std::string &value_name);
  virtual void pushGeneralInstExecutedEvent(const DiagnosticLocation &);

  // simple function switch:
  // in callee side, switch to caller "caller_name"
  virtual void pushCalleeToCallerEvent(const DiagnosticLocation &,
                                       const std::string &caller_name);
  // in caller side, switch to callee "callee_name"
  virtual void pushCallerToCalleeEvent(const DiagnosticLocation &,
                                       const std::string &callee_name);
  // in caller side, switch to callee "callee_name" using the expression
  // "callee_expr" (indirect call)
  virtual void pushIndirectCallerToCalleeEvent(const DiagnosticLocation &,
                                               const std::string &callee_name,
                                               const std::string &callee_expr);

  // general program exit
  virtual void pushProgramExitEvent(const DiagnosticLocation &);
  // general program exit by taking true/false condition at this program point
  virtual void pushProgramConditionExitEvent(const DiagnosticLocation &,
                                             bool cond);

protected:
  DiagnosticText getOrdinalEventDescription(int arg);

  // Human is reading from 1, but machine is reading from 0
  int adjustArgumentIndexForHuman(llvm::CallBase *CS, int arg_index);

  // @deprecated, just call DIA getRealFunctionName if appropriate
  std::string getReadableFunctionName(Function *func);
};

} // namespace lotus::gsaf

namespace lotus::gsaf {
using namespace lotus::reporting;
using namespace llvm;
using namespace std;

namespace ReportDecorator {
extern const std::string UNDEF_DESC;
} // namespace ReportDecorator

class LLVMValueReportDecorator : public DefaultReportDecorator {
protected:
  // Cache used to prevent multiple decoration of a same PHI
  std::unordered_set<BasicBlock *> existing_Phi_cond_cache;

  // Cache all the phiNodes in the analysed module
  std::unordered_map<Function *, std::unordered_set<PHINode *>> phi_node_cache;

  // This is used to help build bug reports for phi instruction
  std::unordered_map<Value *, Value *> phi_select_record;

public:
  static ir_expression::IRExpressionRenderer *inst_resolver;
  static gsa::ControlDependenceAnalysisPass *CDGs;
  static const lotus::APISpec *memory_spec;
  //  static DebugInfoAnalysis *DIA;

public:
  LLVMValueReportDecorator(
      DiagnosticTransformations *transformation_rules = nullptr);
  virtual ~LLVMValueReportDecorator();

protected:
  // build bug report for a global variable
  virtual void procGlobalVariable(const DiagnosticLocation &dbg_loc,
                                  GlobalVariable *gv, Value *val);

  // build bug report for an instruction
  virtual void procInstruction(const DiagnosticLocation &dbg_loc,
                               Instruction *inst, Value *val);

  // Detailed procedure for different kinds of instructions:
  // Instruction: the instruction to process,
  // Value : the value we care in the instruction
  //  @{
  virtual void procReturnInst(const DiagnosticLocation &dbg_loc,
                              ReturnInst *inst, Value *val);
  virtual void procLoadInst(const DiagnosticLocation &dbg_loc, LoadInst *inst,
                            Value *val);
  virtual void procStoreInst(const DiagnosticLocation &dbg_loc, StoreInst *inst,
                             Value *val);

  // The action of a PHI node, such as value flow.
  // A full description for a PHI node is like "taking xxx branch" + "xxx
  // reaches xxx"
  //           where the phi action is the second part
  virtual DiagnosticText
  procPhiAction(PHINode *phi, Value *val,
                const DiagnosticLocation *dbg_loc = nullptr);
  virtual void procPhiInst(const DiagnosticLocation &dbg_loc, PHINode *phi,
                           Value *val);

  virtual void procSelectInst(const DiagnosticLocation &dbg_loc, SelectInst *sl,
                              Value *val);
  // The format for CallInst-Value pair is as follows:
  // Situation 1, inst==val : Receive the function returned value
  // Situation 2, callee == val : A function callsite but we don't analyze the
  // callee Situation 3, otherwise: Pass argument to function and prepare for
  // analyzing the called function
  virtual void procCallInst(const DiagnosticLocation &dbg_loc, CallInst *inst,
                            Value *val);
  virtual void procDivisionInst(const DiagnosticLocation &dbg_loc,
                                Instruction *inst, Value *val);
  //  @}

  // Get the event description for a value.
  // Note that \p val cannot be load or store instructions.
  //
  // Given loc == nullptr, we guarantee that the function has no side effect to
  // the trace
  //
  // If the value event is used for dereferencing, we specially use the right
  // value for better human readability
  virtual DiagnosticText getValueEvent(Value *val,
                                       const DiagnosticLocation *loc = nullptr,
                                       bool is_deref_val = false);

protected:
  // Track the source of a value
  // A value can be used after some internal bit casts and in this function, we
  // track the source value in the source code
  Value *trackValueSource(Value *val);
};

} // namespace lotus::gsaf

namespace lotus::gsaf {
using namespace llvm;
using namespace std;
using namespace lotus::reporting;

using namespace llvm;

class Vulnerability;

class GSAFReportDecorator : public LLVMValueReportDecorator {
protected:
  int report_decorator_var_setting;

public:
  GSAFReportDecorator();
  virtual ~GSAFReportDecorator();

  // Build report from GSAFTrace Trace.
  // If IsFinalize is true, we invoke postProcess function.
  virtual void buildFromTrace(std::shared_ptr<VulnerabilityTrace> &Trace,
                              bool IsFinalize = true);

protected:
  virtual bool hasDebugInfo(const GuardedValueFlowObject *Obj);

  // build bug report for the value flow from an operand node to a use site
  virtual void procNodeSitePair(const DiagnosticLocation &DbgLoc,
                                const GuardedValueFlowNode *,
                                const GuardedValueFlowSite *);

  // build bug report for the value flow from a use site to an operand node
  virtual void procSiteNodePair(const DiagnosticLocation &,
                                const GuardedValueFlowSite *,
                                const GuardedValueFlowNode *);

  // build bug report for a call site that returns something
  virtual void procCallSiteOutput(const DiagnosticLocation &DbgLoc,
                                  const GuardedValueFlowCallSite *,
                                  const GuardedValueFlowCallOutputNode *);

  // build bug report for a call site that uses an operand node as its input
  virtual void procCallSiteInput(const DiagnosticLocation &DbgLoc,
                                 const GuardedValueFlowCallSite *,
                                 const GuardedValueFlowNode *);

  // build bug report for side-effect return
  virtual void procPseudoReturn(const DiagnosticLocation &DbgLoc,
                                const GuardedValueFlowReturnSite *,
                                const GuardedValueFlowReturnNode *);

  // build bug report for the value flow from an operand node to the next
  // operand node
  virtual void procNodeNodePair(const DiagnosticLocation &DbgLoc,
                                const GuardedValueFlowNode *,
                                const GuardedValueFlowNode *);

  // post process, invoked before decoration finished (at end of
  // buildFromTrace() function)
  virtual void postProcess();
};

using namespace llvm;
using namespace std;
using namespace lotus::reporting;

class GSAFTaintReportDecorator : public GSAFReportDecorator {
public:
  GSAFTaintReportDecorator();
  virtual ~GSAFTaintReportDecorator();

  // Build report from GSAFTrace Trace.
  // If IsFinalize is true, we invoke postProcess function.
  virtual void buildFromTrace(std::shared_ptr<VulnerabilityTrace> &Trace,
                              bool IsFinalize) override;
};

} // namespace lotus::gsaf
