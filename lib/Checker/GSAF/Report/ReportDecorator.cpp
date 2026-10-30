#include "Checker/GSAF/Report/ReportDecorator.h"

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "Annotation/APISpec.h"
#include "Checker/Framework/CheckerDiagnostic.h"
#include "Checker/Framework/DiagnosticEvent.h"
#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GSA/GSA.h"
#include "Utils/LLVM/CallUtils.h"
#include "Utils/LLVM/StringUtils.h"

#include <algorithm>
#include <stack>

#include <llvm/Support/Debug.h>

namespace lotus::gsaf {
using namespace lotus::reporting;
using namespace lotus::reporting::ReportDecorator;
using namespace llvm;
using namespace std;

using namespace std;
using namespace REPORT_DECORATOR_NAMESPACE;

DefaultReportDecorator::DefaultReportDecorator(
    DiagnosticTransformations *transformation_rules)
    : DiagnosticBuilder(transformation_rules) {}

DefaultReportDecorator::~DefaultReportDecorator() {}

RegisterDecoratorEventWithOneArgument(DefaultNumST, "$(1)st", "");
RegisterDecoratorEventWithOneArgument(DefaultNumND, "$(1)nd", "");
RegisterDecoratorEventWithOneArgument(DefaultNumRD, "$(1)rd", "");
RegisterDecoratorEventWithOneArgument(DefaultNumTH, "$(1)th", "");
DiagnosticText DefaultReportDecorator::getOrdinalEventDescription(int idx) {
  string idx_str = format_str("%d", idx);

  // make idx always positive. This is faster than having to check for
  // negative cases.
  idx = idx < 0 ? -idx : idx; // same as idx = abs(idx); but faster, so there.

  // Numbers from 11 to 13 don't have st, nd, rd
  if (10 < idx && idx < 14)
    return getDefaultNumTHEvent(idx_str);

  switch (idx % 10) {
  case 1:
    return getDefaultNumSTEvent(idx_str);

  case 2:
    return getDefaultNumNDEvent(idx_str);

  case 3:
    return getDefaultNumRDEvent(idx_str);

  default:
    return getDefaultNumTHEvent(idx_str);
  }
}

RegisterDecoratorEventWithNoArgument(DefaultTraceStart,
                                     "The bug trace start from this point", "");
void DefaultReportDecorator::pushTraceStartEvent(
    const DiagnosticLocation &dbg_loc) {
  pushReportDecoratorEventWithNoArg(dbg_loc, DefaultTraceStart);
}

RegisterDecoratorEventWithTwoArguments(DefaultValueUsedInFunction,
                                       "$(1) is used in function $(2)", "");
void DefaultReportDecorator::pushGeneralValueUsedInCalleeEvent(
    const DiagnosticLocation &dbg_loc, const string &value_name,
    const string &function_name) {
  pushDecoratorEvent(dbg_loc, DefaultValueUsedInFunction, value_name,
                     function_name);
}

RegisterDecoratorEventWithOneArgument(DefaultTrueOrFalseBranch,
                                      "Select the $(1) branch at this point",
                                      "");
void DefaultReportDecorator::pushTrueOrFalseBranchEvent(
    const DiagnosticLocation &dbg_loc, bool branch_choice) {
  pushDecoratorEvent(dbg_loc, DefaultTrueOrFalseBranch,
                     branch_choice ? "true" : "false");
}

RegisterDecoratorEventWithNoArgument(DefaultTrueAndFalseBranch,
                                     "Select either branch at this point", "");
void DefaultReportDecorator::pushTrueAndFalseBranchEvent(
    const DiagnosticLocation &dbg_loc) {
  pushReportDecoratorEventWithNoArg(dbg_loc, DefaultTrueAndFalseBranch);
}

RegisterDecoratorEventWithOneArgument(
    DefaultPhiFlowToCurrent, "Program flows from line $(1) to this point", "");
void DefaultReportDecorator::pushPHIFlowToCurrentPointEvent(
    const DiagnosticLocation &dbg_loc, int from_line) {
  pushDecoratorEvent(dbg_loc, DefaultPhiFlowToCurrent,
                     format_str("%d", from_line));
}

RegisterDecoratorEventWithTwoArguments(
    DefaultPhiFlowGeneral, "Program flows from line $(1) to Line $(2)", "");
void DefaultReportDecorator::pushGeneralPHIFlowEvent(
    const DiagnosticLocation &dbg_loc, int from_line, int to_line) {
  pushDecoratorEvent(dbg_loc, DefaultPhiFlowGeneral,
                     format_str("%d", from_line), format_str("%d", to_line));
}

void DefaultReportDecorator::pushCallsiteCommonArgEvent(
    const DiagnosticLocation &dbg_loc, const std::string &arg_name, int arg_idx,
    const std::string &func_name) {
  pushCallsiteCommonArgEvent(dbg_loc, DiagnosticTextUnit(arg_name), arg_idx,
                             func_name);
}

RegisterDecoratorEventWithThreeArguments(
    DefaultCallsiteCommonArg,
    "$(1) is used as the $(2) parameter in function $(3)", "");
RegisterDecoratorEventWithTwoArguments(
    DefaultCallsiteThisPointerArg,
    "$(1) is passed as the \'this\' pointer to function $(2)", "");
RegisterDecoratorEventWithTwoArguments(
    DefaultCallsiteUnknownArg, "$(1) is passed as a parameter to function $(2)",
    "");
void DefaultReportDecorator::pushCallsiteCommonArgEvent(
    const DiagnosticLocation &dbg_loc, const DiagnosticText &arg_name,
    int arg_idx, const std::string &func_name) {

  if (arg_idx > 0) {
    pushDecoratorEvent(dbg_loc, DefaultCallsiteCommonArg, arg_name,
                       getOrdinalEventDescription(arg_idx), func_name);
  } else {
    if (arg_idx == ARG_IDX_THIS_POINTER) {
      pushDecoratorEvent(dbg_loc, DefaultCallsiteThisPointerArg, arg_name,
                         func_name);
    } else {
      pushDecoratorEvent(dbg_loc, DefaultCallsiteUnknownArg, arg_name,
                         func_name);
    }
  }
}

RegisterDecoratorEventWithFourArguments(
    DefaultCallsitePsdudoArgOnCommonArg,
    "The value $(1) is passed to function $(2), where $(3) is the $(4) "
    "parameter",
    "");
RegisterDecoratorEventWithThreeArguments(
    DefaultCallsitePsdudoArgOnGlobal,
    "The value $(1) is passed to function $(2), where $(3) is a global "
    "variable",
    "");
RegisterDecoratorEventWithTwoArguments(
    DefaultCallsitePsdudoArgOnUnknownSource,
    "The value $(1) is passed to function $(2)", "");
void DefaultReportDecorator::pushCallsitePseudoArgEvent(
    const DiagnosticLocation &dbg_loc, const std::string &full_access_path,
    const std::string &func_name, const std::string &arg_name, int arg_idx) {
  if (arg_idx > 0) {
    pushDecoratorEvent(dbg_loc, DefaultCallsitePsdudoArgOnCommonArg,
                       full_access_path, func_name, arg_name,
                       getOrdinalEventDescription(arg_idx + 1));
  } else if (arg_idx == DefaultReportDecorator::ARG_IDX_GLOBAL) {
    pushDecoratorEvent(dbg_loc, DefaultCallsitePsdudoArgOnGlobal,
                       full_access_path, func_name, arg_name);
  } else {
    // Unknown source or this pointer
    pushDecoratorEvent(dbg_loc, DefaultCallsitePsdudoArgOnUnknownSource,
                       full_access_path, func_name);
  }
}

void DefaultReportDecorator::pushCallsiteVaArgEvent(
    const DiagnosticLocation &dbg_loc, const std::string &arg_name,
    const std::string &func_name) {
  pushCallsiteVaArgEvent(dbg_loc, DiagnosticTextUnit(arg_name), func_name);
}

RegisterDecoratorEventWithTwoArguments(
    DefaultCallsiteVaArg,
    "$(1) is passed to function $(2) as a varidic argument", "");
void DefaultReportDecorator::pushCallsiteVaArgEvent(
    const DiagnosticLocation &dbg_loc, const DiagnosticText &arg_name,
    const std::string &func_name) {
  pushDecoratorEvent(dbg_loc, DefaultCallsiteVaArg, arg_name, func_name);
}

RegisterDecoratorEventWithNoArgument(DefaultEnterFunction,
                                     "Enter function call", "");
RegisterDecoratorEventWithOneArgument(DefaultEnterFunctionWithName,
                                      "Enter function $(1)", "");
void DefaultReportDecorator::pushCallsiteSimpleEnterEvent(
    const DiagnosticLocation &dbg_loc, const std::string &func_name) {
  if (func_name.empty()) {
    pushReportDecoratorEventWithNoArg(dbg_loc, DefaultEnterFunction);
  } else {
    pushDecoratorEvent(dbg_loc, DefaultEnterFunctionWithName, func_name);
  }
}

RegisterDecoratorEventWithOneArgument(DefaultCallSiteFinishWithName,
                                      "Function $(1) executes and returns", "");
void DefaultReportDecorator::pushCallsiteEmptyRetEvent(
    const DiagnosticLocation &dbg_loc, const std::string &func_name) {
  pushDecoratorEvent(dbg_loc, DefaultCallSiteFinishWithName, func_name);
}

RegisterDecoratorEventWithTwoArguments(
    DefaultCommonReturn,
    "Function $(2) executes and stores the return value to $(1)", "");
RegisterDecoratorEventWithOneArgument(DefaultCommonReturnRightValue,
                                      "Function $(1) executes and returns", "");
void DefaultReportDecorator::pushCallsiteCommonRetEvent(
    const DiagnosticLocation &dbg_loc, const std::string &ret_val_name,
    const std::string &func_name, bool is_left_value_call_expr) {
  if (is_left_value_call_expr) {
    pushDecoratorEvent(dbg_loc, DefaultCommonReturn, ret_val_name, func_name);
  } else {
    pushDecoratorEvent(dbg_loc, DefaultCommonReturnRightValue, func_name);
  }
}

RegisterDecoratorEventWithFiveArguments(
    DefaultCallSitePseudoRetOnCommonArg,
    "Calling to function $(2) modified $(1), where $(3) is used as the $(4) "
    "parameter ($(5))",
    "");
RegisterDecoratorEventWithThreeArguments(
    DefaultCallSitePseudoRetOnGlobal,
    "Calling to function $(2) modified $(1), where $(3) is a global variable",
    "");
RegisterDecoratorEventWithThreeArguments(
    DefaultCallSitePseudoRetOnCommonRet,
    "Calling to function $(2) modified $(1), where $(3) gets the return value",
    "");
RegisterDecoratorEventWithTwoArguments(
    DefaultCallSitePseudoRetOnCommonRetRightValue,
    "Calling to function $(2) modified $(1)", "");
RegisterDecoratorEventWithTwoArguments(DefaultCallSitePseudoRetOnUnknownSource,
                                       "Calling to function $(2) modified $(1)",
                                       "");
void DefaultReportDecorator::pushCallsitePseudoRetEvent(
    const DiagnosticLocation &dbg_loc, const std::string &full_access_path,
    const std::string &func_name, const std::string &arg_name, int arg_idx,
    const std::string &formal_arg_name, bool is_left_value_call_expr) {
  if (arg_idx > 0) {
    pushDecoratorEvent(dbg_loc, DefaultCallSitePseudoRetOnCommonArg,
                       full_access_path, func_name, arg_name,
                       getOrdinalEventDescription(arg_idx), formal_arg_name);
  } else {
    switch (arg_idx) {
    case DefaultReportDecorator::ARG_IDX_GLOBAL: {
      pushDecoratorEvent(dbg_loc, DefaultCallSitePseudoRetOnGlobal,
                         full_access_path, func_name, arg_name);
    } break;
    case DefaultReportDecorator::ARG_IDX_RETURN: {
      if (is_left_value_call_expr) {
        pushDecoratorEvent(dbg_loc, DefaultCallSitePseudoRetOnCommonRet,
                           full_access_path, func_name, arg_name);
      } else {
        pushDecoratorEvent(dbg_loc,
                           DefaultCallSitePseudoRetOnCommonRetRightValue,
                           full_access_path, func_name);
      }
    } break;
    default: {
      pushDecoratorEvent(dbg_loc, DefaultCallSitePseudoRetOnUnknownSource,
                         full_access_path, func_name);
    }
    }
  }
}

RegisterDecoratorEventWithOneArgument(
    DefaultFunctionCommonArg, "Argument $(1) gets the value from its caller",
    "");
void DefaultReportDecorator::pushFunctionCommonArgEvent(
    const DiagnosticLocation &dbg_loc, const std::string &arg_name) {
  pushDecoratorEvent(dbg_loc, DefaultFunctionCommonArg, arg_name);
}

RegisterDecoratorEventWithThreeArguments(
    DefaultFunctionPseudoArgOnCommonArg,
    "$(1) gets the value from caller of the function, where $(2) is the $(3) "
    "parameter",
    "");
RegisterDecoratorEventWithTwoArguments(
    DefaultFunctionPseudoArgOnGlobal,
    "$(1) gets the value from caller of the function, where $(2) is a global "
    "variable",
    "");
RegisterDecoratorEventWithOneArgument(
    DefaultFunctionPseudoArgOnUnknownSource,
    "$(1) gets the value from caller of the function", "");
void DefaultReportDecorator::pushFunctionPseudoArgEvent(
    const DiagnosticLocation &dbg_loc, const std::string &full_access_path,
    const std::string &arg_name, int arg_idx) {
  if (arg_idx > 0) {
    pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoArgOnCommonArg,
                       full_access_path, arg_name,
                       getOrdinalEventDescription(arg_idx + 1));
  } else if (arg_idx == DefaultReportDecorator::ARG_IDX_GLOBAL) {
    pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoArgOnGlobal,
                       full_access_path, arg_name);
  } else {
    pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoArgOnUnknownSource,
                       full_access_path);
  }
}

RegisterDecoratorEventWithOneArgument(
    DefaultFunctionVaArg,
    "$(1) gets the value from a varidic argument given by its caller", "");
void DefaultReportDecorator::pushFunctionVaArgEvent(
    const DiagnosticLocation &dbg_loc, const std::string &arg_name) {
  pushDecoratorEvent(dbg_loc, DefaultFunctionVaArg, arg_name);
}

RegisterDecoratorEventWithNoArgument(DefaultFunctionEmptyRet,
                                     "Return to caller", "");
void DefaultReportDecorator::pushFunctionEmptyRetEvent(
    const DiagnosticLocation &dbg_loc) {
  pushReportDecoratorEventWithNoArg(dbg_loc, DefaultFunctionEmptyRet);
}

RegisterDecoratorEventWithNoArgument(DefaultFunctionUndefRet,
                                     "Return an undefined value to its caller",
                                     "");
RegisterDecoratorEventWithOneArgument(DefaultFunctionCommonRet,
                                      "Return $(1) to caller", "");
void DefaultReportDecorator::pushFunctionCommonRetEvent(
    const DiagnosticLocation &dbg_loc, const std::string &ret_val_name) {
  if (ret_val_name == "undef") {
    pushReportDecoratorEventWithNoArg(dbg_loc, DefaultFunctionUndefRet);
  } else {
    pushDecoratorEvent(dbg_loc, DefaultFunctionCommonRet, ret_val_name);
  }
}

RegisterDecoratorEventWithThreeArguments(
    DefaultFunctionPseudoRetOnCommonArg,
    "Return with value $(1) modified, where $(2) is used as the $(3) parameter",
    "");
RegisterDecoratorEventWithTwoArguments(
    DefaultFunctionPseudoRetOnGlobal,
    "Return with value $(1) modified, where $(2) is a global variable", "");
RegisterDecoratorEventWithTwoArguments(
    DefaultFunctionPseudoRetOnCommonRet,
    "Return with value $(1) modified, where $(2) is the return value", "");
RegisterDecoratorEventWithOneArgument(DefaultFunctionPseudoRetOnUnknownSource,
                                      "Return with value $(1) modified", "");
void DefaultReportDecorator::pushFunctionPseudoRetEvent(
    const DiagnosticLocation &dbg_loc, const std::string &full_access_path,
    const std::string &arg_name, int arg_idx) {
  if (arg_idx > 0) {
    pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoRetOnCommonArg,
                       full_access_path, arg_name,
                       getOrdinalEventDescription(arg_idx + 1));
  } else {
    switch (arg_idx) {
    case DefaultReportDecorator::ARG_IDX_GLOBAL: {
      pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoRetOnGlobal,
                         full_access_path, arg_name);
    } break;
    case DefaultReportDecorator::ARG_IDX_RETURN: {
      pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoRetOnCommonRet,
                         full_access_path, arg_name);
    } break;
    default: {
      pushDecoratorEvent(dbg_loc, DefaultFunctionPseudoRetOnUnknownSource,
                         full_access_path);
    }
    }
  }
}

RegisterDecoratorEventWithOneArgument(DefaultValueAlive,
                                      "$(1) is unchanged at this point", "");
void DefaultReportDecorator::pushGeneralValueAliveEvent(
    const DiagnosticLocation &dbg_loc, const std::string &value_name) {
  pushDecoratorEvent(dbg_loc, DefaultValueAlive, value_name);
}

RegisterDecoratorEventWithOneArgument(DefaultValueAliveAndUsed,
                                      "$(1) is used in this statement", "");
void DefaultReportDecorator::pushGeneralValueAliveAndUsedEvent(
    const DiagnosticLocation &dbg_loc, const std::string &value_name) {
  pushDecoratorEvent(dbg_loc, DefaultValueAliveAndUsed, value_name);
}

RegisterDecoratorEventWithOneArgument(DefaultValueUsed,
                                      "$(1) is used in this statement", "");
void DefaultReportDecorator::pushGeneralValueUsedEvent(
    const DiagnosticLocation &dbg_loc, const std::string &value_name) {
  pushDecoratorEvent(dbg_loc, DefaultValueUsed, value_name);
}

RegisterDecoratorEventWithNoArgument(
    DefaultExecuteThrough, "Program execution passes through this point", "");
void DefaultReportDecorator::pushGeneralInstExecutedEvent(
    const DiagnosticLocation &dbg_loc) {
  pushReportDecoratorEventWithNoArg(dbg_loc, DefaultExecuteThrough);
}

RegisterDecoratorEventWithOneArgument(DefaultCalleeToCaller,
                                      "Return to the caller function $(1)", "");
void DefaultReportDecorator::pushCalleeToCallerEvent(
    const DiagnosticLocation &dbg_loc, const std::string &caller_name) {
  pushDecoratorEvent(dbg_loc, DefaultCalleeToCaller, caller_name);
}

RegisterDecoratorEventWithOneArgument(DefaultCallerToCallee,
                                      "Enter into the callee function $(1)",
                                      "");
void DefaultReportDecorator::pushCallerToCalleeEvent(
    const DiagnosticLocation &dbg_loc, const std::string &callee_name) {
  pushDecoratorEvent(dbg_loc, DefaultCallerToCallee, callee_name);
}

RegisterDecoratorEventWithTwoArguments(
    DefaultCallerToIndirectCallee,
    "Program goes into its callee function $(1) through value $(2)", "");
void DefaultReportDecorator::pushIndirectCallerToCalleeEvent(
    const DiagnosticLocation &dbg_loc, const std::string &callee_name,
    const std::string &callee_expr) {
  pushDecoratorEvent(dbg_loc, DefaultCallerToIndirectCallee, callee_name,
                     callee_expr);
}

RegisterDecoratorEventWithNoArgument(DefaultProgramExit,
                                     "Program reaches an exit point", "");
void DefaultReportDecorator::pushProgramExitEvent(
    const DiagnosticLocation &dbg_loc) {
  pushReportDecoratorEventWithNoArg(dbg_loc, DefaultProgramExit);
}

RegisterDecoratorEventWithOneArgument(
    DefaultProgramConditionalExit,
    "Taking $(1) branch, program reaches an exit point", "");
void DefaultReportDecorator::pushProgramConditionExitEvent(
    const DiagnosticLocation &dbg_loc, bool cond) {
  pushDecoratorEvent(dbg_loc, DefaultProgramConditionalExit,
                     cond ? "true" : "false");
}

int DefaultReportDecorator::adjustArgumentIndexForHuman(llvm::CallBase *CS,
                                                        int arg_index) {
  Function *F = CS->getCalledFunction();

  if (!F)
    return arg_index;

  /*
   * Saying the 1^st argument is more friendly for human than saying the 0^th
   * argument. Therefore, we normally add 1 for argument index. But for C++
   * class methods, the first argument is the invisible `this` pointer.
   * Therefore, we don't need to add 1 for counting the argument index.
   */
  int offset_for_reading = 1;

  if (llvm_utils::isClassMemberFunction(*F) || llvm_utils::isVirtualCall(CS)) {
    // Class methods must have this pointer
    offset_for_reading = 0;
  }

  int k = arg_index + offset_for_reading;
  if (k == 0) {
    // arg_index == 0 and offset_for_reading == 0, which is the `this` pointer
    k = ARG_IDX_THIS_POINTER;
  }

  return k;
}

string DefaultReportDecorator::getReadableFunctionName(Function *F) {
  return DIA->getDeclaredFunctionName(F);
}
} // namespace lotus::gsaf

namespace lotus::gsaf {
using namespace lotus::reporting;
using namespace lotus::reporting::ReportDecorator;
using namespace llvm;
using namespace std;
namespace {
Value *getRWPtr(Instruction *instruction) {
  if (auto *load = dyn_cast<LoadInst>(instruction))
    return load->getPointerOperand();
  if (auto *store = dyn_cast<StoreInst>(instruction))
    return store->getPointerOperand();
  return nullptr;
}
int brCondFromTo(BasicBlock *from, BasicBlock *to) {
  auto *branch = dyn_cast<BranchInst>(from->getTerminator());
  if (!branch)
    return -1;
  for (unsigned index = 0; index < branch->getNumSuccessors(); ++index)
    if (branch->getSuccessor(index) == to)
      return index;
  return -1;
}
const char *get_real_branch_selection_label(BranchInst *, int index) {
  return index == 0 ? "true" : "false";
}
} // namespace

#undef DEBUG_TYPE
#define DEBUG_TYPE "llvm-value-report-decorator"

#define DEFAULT_UNDEF_DESC "an undefined value"

const std::string ReportDecorator::UNDEF_DESC = DEFAULT_UNDEF_DESC;

using namespace std;
using namespace REPORT_DECORATOR_NAMESPACE;

ir_expression::IRExpressionRenderer *LLVMValueReportDecorator::inst_resolver =
    nullptr;
gsa::ControlDependenceAnalysisPass *LLVMValueReportDecorator::CDGs = nullptr;
const lotus::APISpec *LLVMValueReportDecorator::memory_spec = nullptr;
// DebugInfoAnalysis *LLVMValueReportDecorator::DIA = nullptr;

Value *LLVMValueReportDecorator::trackValueSource(Value *val) {
  bool track_back = false;
  if (!isa<Constant>(val)) {
    do {
      track_back = false;
      const MDNode *var_md = DIA->findVarInfoMDNode(val);
      if (!var_md) {
        // This value perhaps is an intermediate variable generated by LLVM
        // Handle some special instructions
        if (const Instruction *inst = dyn_cast<Instruction>(val)) {
          if (inst->isCast() && !DIA->hasVariableDebugName(val)) {
            val = inst->getOperand(0);
            track_back = true;
          }
        }
      }
    } while (track_back);
  }

  return val;
}

LLVMValueReportDecorator::LLVMValueReportDecorator(
    DiagnosticTransformations *transformation_rules)
    : DefaultReportDecorator(transformation_rules) {}

LLVMValueReportDecorator::~LLVMValueReportDecorator() {}

void LLVMValueReportDecorator::procInstruction(
    const DiagnosticLocation &dbg_loc, Instruction *inst, Value *val) {
  assert(inst && "Are you sure to build reports for a nullptr instruction?");
  assert(val && "You do not indicate what value is used in the instruction!");
  bool value_is_operand = false;

  for (unsigned i = 0; i < inst->getNumOperands(); i++) {
    if (inst->getOperand(i) == val) {
      value_is_operand = true;
      break;
    }
  }
  if (!value_is_operand && val != inst) {
    errs() << "\nInst: " << *inst << "\n";
    errs() << "Value: " << *val << "\n";
    llvm_unreachable(
        "The instruction does not contain the value as an operand!");
  }

  switch (inst->getOpcode()) {
  case Instruction::Ret:
    procReturnInst(dbg_loc, (ReturnInst *)inst, val);
    break;
    // mem
  case Instruction::Load:
    procLoadInst(dbg_loc, (LoadInst *)inst, val);
    break;
  case Instruction::Store:
    procStoreInst(dbg_loc, (StoreInst *)inst, val);
    break;
    // others
  case Instruction::PHI:
    procPhiInst(dbg_loc, (PHINode *)inst, val);
    break;
  case Instruction::Call:
    procCallInst(dbg_loc, (CallInst *)inst, val);
    break;
  case Instruction::Select:
    procSelectInst(dbg_loc, (SelectInst *)inst, val);
    break;

    // insts that are ignored
    // can extend in the future
    // -----------------------------------------
    // gep
  case Instruction::GetElementPtr:
    // var arg
  case Instruction::VAArg:
    // vector
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
    break;
    // binary inst
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::FRem:
  case Instruction::SRem:
    procDivisionInst(dbg_loc, inst, val);
    break;
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::Add:
  case Instruction::Sub:
  // conversion inst
  case Instruction::AddrSpaceCast:
  case Instruction::IntToPtr:
  case Instruction::PtrToInt:
  case Instruction::BitCast:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::SIToFP:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::FPToUI:
  // cmp
  case Instruction::ICmp:
  case Instruction::FCmp:
    break;
  // unsupported
  case Instruction::Alloca:
  case Instruction::ExtractValue:
  case Instruction::InsertValue:
  case Instruction::ShuffleVector:
  case Instruction::AtomicRMW:
  case Instruction::AtomicCmpXchg:
  case Instruction::Fence:
  case Instruction::Switch:
  case Instruction::Invoke:
  case Instruction::LandingPad:
  case Instruction::Resume:
  case Instruction::IndirectBr:
  case Instruction::Unreachable:
  case Instruction::Br:
  default:
    errs() << *inst << "\n";
    llvm_unreachable("Unsupported instruction!");
    exit(1);
  }
}

RegisterDecoratorEventWithTwoArguments(
    DefaultGlobalInit, "The global variable $(1) is initialized as $(2)", "");
void LLVMValueReportDecorator::procGlobalVariable(
    const DiagnosticLocation &dbg_loc, GlobalVariable *gv, Value *val) {
  assert(gv->hasInitializer() &&
         "If no initializer, it should not be an item in a trace!");
  pushDecoratorEvent(
      dbg_loc, DefaultGlobalInit, decorator_emph_str(DIA->getVariableName(gv)),
      decorator_emph_str(DIA->getVariableName(gv->getInitializer())));
}

RegisterDecoratorEventWithOneArgument(DefaultComplexReturn,
                                      "Return $(1) to caller", "");
void LLVMValueReportDecorator::procReturnInst(const DiagnosticLocation &dbg_loc,
                                              ReturnInst *return_inst,
                                              Value *val) {
  if (DIA->hasVariableDebugName(val)) {
    pushFunctionCommonRetEvent(
        dbg_loc, decorator_emph_str(inst_resolver->restore_value_expr(
                     val, &phi_select_record)));
  } else {
    pushDecoratorEvent(dbg_loc, DefaultComplexReturn,
                       getValueEvent(val, &dbg_loc));
  }
}

RegisterDecoratorEventWithOneArgument(DefaultLoadVirtualCall,
                                      "Call a virtual function on $(1)", "");
RegisterDecoratorEventWithTwoArguments(
    DefaultLoadInstWithTarget, "Load value from $(1) and assign to $(2)", "");
RegisterDecoratorEventWithOneArgument(DefaultLoadInstWithoutTarget,
                                      "Load value from $(1)", "");
void LLVMValueReportDecorator::procLoadInst(const DiagnosticLocation &dbg_loc,
                                            LoadInst *load, Value *val) {

  string load_expr =
      inst_resolver->restore_value_expr(load, &phi_select_record);
  DiagnosticTextUnit load_expr_event(decorator_emph_str(load_expr));

  StringRef load_name = load->getName();

  if (load_name.startswith("vtable")) {
    // Might be used for calling a virtual function
    pushDecoratorEvent(dbg_loc, DefaultLoadVirtualCall, load_expr_event);
  } else {
    if (DIA->hasVariableDebugName(load)) {
      pushDecoratorEvent(dbg_loc, DefaultLoadInstWithTarget, load_expr_event,
                         decorator_emph_str(DIA->getVariableName(load)));
    } else {
      // A LLVM temporary variable to hold the loaded value
      pushDecoratorEvent(dbg_loc, DefaultLoadInstWithoutTarget,
                         load_expr_event);
    }
  }
}

RegisterDecoratorEventWithTwoArguments(DefaultStoreValue, "Store $(1) to $(2)",
                                       "");
void LLVMValueReportDecorator::procStoreInst(const DiagnosticLocation &dbg_loc,
                                             StoreInst *store, Value *val) {
  Value *store_val = store->getOperand(0);

  string store_expr =
      inst_resolver->restore_value_expr(store, &phi_select_record);
  DiagnosticTextUnit target_ptr_expr(decorator_emph_str(store_expr));
  DiagnosticText store_val_event = getValueEvent(store_val);

  pushDecoratorEvent(dbg_loc, DefaultStoreValue,
                     getValueEvent(store_val, &dbg_loc), target_ptr_expr);
}

RegisterDecoratorEventWithOneArgument(DefaultDevideAction, "Divide by $(1)",
                                      "");
void LLVMValueReportDecorator::procDivisionInst(
    const DiagnosticLocation &dbg_loc, Instruction *divide, Value *val) {
  Value *op2 = divide->getOperand(1);
  pushDecoratorEvent(dbg_loc, DefaultDevideAction,
                     getValueEvent(op2, &dbg_loc));
}

RegisterDecoratorEventWithTwoArguments(DefaultPhiActionConstant,
                                       "$(1) assigned to $(2) reaches here",
                                       "");
RegisterDecoratorEventWithThreeArguments(
    DefaultPhiActionWithInitLine,
    "$(1) defined on line $(2) reaches here and is assigned to $(3)", "");
RegisterDecoratorEventWithTwoArguments(DefaultPhiNoNameWithLine,
                                       "$(1) on line $(2) reaches here", "");
RegisterDecoratorEventWithOneArgument(DefaultPhiReachable,
                                      "value of $(1) reaches here", "");

RegisterDecoratorEventWithOneArgument(DefaultPhiActionUndef,
                                      "$(1) gets undefined value", "");
RegisterDecoratorEventWithTwoArguments(
    DefaultPhiActionWithInitLineOnUndef,
    "undefined value on line $(1) reaches here and is assigned to $(2)", "");
RegisterDecoratorEventWithOneArgument(
    DefaultPhiNoNameWithLineOnUndef,
    "undefined value on line $(1) reaches here", "");
RegisterDecoratorEventWithNoArgument(DefaultPhiReachableOnUndef,
                                     "an undefined value reaches here", "");

RegisterDecoratorEventWithOneArgument(DefaultConstValueInit,
                                      "$(1) is defined here", "");
RegisterDecoratorEventWithTwoArguments(DefaultPhiActionNonConstant,
                                       "$(1) defined on line $(2) reaches here",
                                       "");
DiagnosticText
LLVMValueReportDecorator::procPhiAction(PHINode *phi, Value *val,
                                        const DiagnosticLocation *dbg_loc) {
  int val_line = isa<Constant>(val) ? DIA->getPhiOperandSourceLine(phi, val)
                                    : DIA->getSourceLine(val);
  bool new_step = false;

  if (isa<Constant>(val)) {
    if (DIA->hasVariableDebugName(phi)) {
      if (val_line <= 0 || new_step) {
        if (isa<UndefValue>(val)) {
          return getDefaultPhiActionUndefEvent(
              decorator_emph_str(DIA->getVariableName(phi)));
        }

        return getDefaultPhiActionConstantEvent(
            decorator_emph_str(inst_resolver->restore_value_expr(val)),
            decorator_emph_str(DIA->getVariableName(phi)));
      } else {
        if (isa<UndefValue>(val)) {
          return getDefaultPhiActionWithInitLineOnUndefEvent(
              format_str("%d", val_line),
              decorator_emph_str(DIA->getVariableName(phi)));
        }

        return getDefaultPhiActionWithInitLineEvent(
            decorator_emph_str(inst_resolver->restore_value_expr(val)),
            format_str("%d", val_line),
            decorator_emph_str(DIA->getVariableName(phi)));
      }
    } else {
      if (val_line <= 0 || new_step) {
        if (isa<UndefValue>(val)) {
          return getDefaultPhiReachableOnUndefEvent();
        }

        return getDefaultPhiReachableEvent(getValueEvent(val, dbg_loc));
      } else {
        if (isa<UndefValue>(val)) {
          return getDefaultPhiNoNameWithLineOnUndefEvent(
              format_str("%d", val_line));
        }

        return getDefaultPhiNoNameWithLineEvent(
            decorator_emph_str(inst_resolver->restore_value_expr(val)),
            format_str("%d", val_line));
      }
    }
  } else {
    if (!dbg_loc) {
      return getDefaultPhiActionNonConstantEvent(getValueEvent(val),
                                                 format_str("%d", val_line));
    } else {
      return getDefaultPhiReachableEvent(getValueEvent(val, dbg_loc));
    }
  }
}

static bool is_bb_reachable_recursive(BasicBlock *src, BasicBlock *sink,
                                      unordered_set<BasicBlock *> &visited) {
  if (src == nullptr || sink == nullptr) {
    return false;
  }

  if (visited.count(src)) {
    return false;
  }

  visited.insert(src);

  if (src == sink) {
    return true;
  }

  auto *terminator = src->getTerminator();
  unsigned num_successors = terminator->getNumSuccessors();
  for (unsigned i = 0; i < num_successors; i++) {
    BasicBlock *succ = terminator->getSuccessor(i);
    if (is_bb_reachable_recursive(succ, sink, visited)) {
      return true;
    }
  }

  return false;
}

static bool is_bb_reachable(BasicBlock *src, BasicBlock *sink) {
  unordered_set<BasicBlock *> visited;
  return is_bb_reachable_recursive(src, sink, visited);
}

RegisterDecoratorEventWithOneArgument(DefaultPhiPathList,
                                      "Possible Paths: \n $(1)\n", "");
RegisterDecoratorEventWithTwoArguments(DefaultPhiPathListItem,
                                       "\t $(1). $(2)\n", "");
RegisterDecoratorEventWithOneArgument(DefaultPhiPathListInternalItem,
                                      " or $(1),", "");
RegisterDecoratorEventWithNoArgument(DefaultEmptyPhiPath, "", "");
RegisterDecoratorEventWithTwoArguments(DefaultPhiPathBranchChoice,
                                       "Taking $(1) branch at line $(2), ", "");
RegisterDecoratorEventWithTwoArguments(DefaultPhiInst, "$(1)$(2)", "");
RegisterDecoratorEventWithTwoArguments(DefaultPhiCondDesc, " ($(1) is $(2))",
                                       "");
void LLVMValueReportDecorator::procPhiInst(const DiagnosticLocation &dbg_loc,
                                           PHINode *phi, Value *val) {
  assert(val);
  phi_select_record[phi] = val;

  Value *last_step_IR_source = empty() ? nullptr : back()->getIRSource();
  BasicBlock *last_step_BB = nullptr;
  if (last_step_IR_source) {
    if (Instruction *last_step_inst =
            dyn_cast<Instruction>(last_step_IR_source)) {
      last_step_BB = last_step_inst->getParent();
    }
  }

  // start the lambda
  auto generateDiagInfo = [&](BasicBlock *incoming_BB, Value *&result_cond_val,
                              bool &result_cond, int &result_dbg_line,
                              Instruction *&result_br_inst) {
    result_cond_val = nullptr;
    result_cond = false;
    result_dbg_line = 0;
    result_br_inst = nullptr;

    DiagnosticTextList ret;
    assert(incoming_BB &&
           "A null PHI node must have one of its argument is null.");

    // Get the condition of picking this operand
    BasicBlock *phi_BB = phi->getParent();
    Instruction *terminator = incoming_BB->getTerminator();

    if (BranchInst *br = dyn_cast<BranchInst>(terminator)) {
      int br_line = DIA->getSourceLine(terminator);
      if (br_line && br->isConditional()) {
        // Has line info for the nearest branch
        result_cond_val = br->getCondition();
        result_cond = brCondFromTo(incoming_BB, phi_BB) == 0;
        result_dbg_line = br_line;
        result_br_inst = br;

        const string br_label = result_cond ? "true" : "false";
        ret.pushArg(getDefaultPhiPathBranchChoiceEvent(
            br_label, format_str("%d", br_line)));
      } else {
        // Seek the nearest if conditions that route to oprdBB for help
        Function *phi_func = phi_BB->getParent();
        assert(phi_func && "Phi instruction must have an enclosing function");

        // We print the key if-condition choices that lead to the oprdBB
        auto &CDG = CDGs->getControlDependenceAnalysis(*phi_func);
        std::vector<std::pair<int, BasicBlock *>> ctrl_deps;
        for (BasicBlock *controller : CDG.getCDBlocks(incoming_BB)) {
          auto *branch = dyn_cast<BranchInst>(controller->getTerminator());
          int sense =
              branch && branch->isConditional() &&
                      CDG.isReachable(branch->getSuccessor(1), incoming_BB)
                  ? 1
                  : 0;
          ctrl_deps.emplace_back(sense, controller);
        }
        int num_deps = ctrl_deps.size();
        std::vector<std::pair<const char *, int>> br_info;

        for (int Index = 0; Index < num_deps; ++Index) {
          BasicBlock *cdep_BB = ctrl_deps[0].second;
          if (cdep_BB) {
            Instruction *cdep_terminator = cdep_BB->getTerminator();
            const char *br_label = "true";
            if (BranchInst *br_inst = dyn_cast<BranchInst>(cdep_terminator)) {
              // It should be a conditional branch
              br_label =
                  get_real_branch_selection_label(br_inst, ctrl_deps[0].first);
            }

            int br_line = DIA->getSourceLine(cdep_terminator);
            if (br_line) {
              auto br_pair = std::make_pair(br_label, br_line);
              // Do not add redundant path condition
              if (std::find(br_info.begin(), br_info.end(), br_pair) ==
                  br_info.end()) {
                br_info.push_back(br_pair);

                if (br_info.size() == 1) {
                  if (BranchInst *br_inst =
                          dyn_cast<BranchInst>(cdep_terminator)) {
                    if (br_inst->isConditional()) {
                      result_cond_val = br_inst->getCondition();
                      result_cond = string(br_label) == "true";
                      result_dbg_line = br_line;
                      result_br_inst = br_inst;
                    }
                  }
                } else {
                  // invalidate
                  result_cond_val = nullptr;
                  result_cond = false;
                  result_dbg_line = 0;
                  result_br_inst = nullptr;
                }
              }
            }
          }
        }

        if (!br_info.empty()) {
          for (size_t index = 0; index < br_info.size(); ++index) {
            const char *br_label = br_info[index].first;
            int br_line = br_info[index].second;
            if (index == 0)
              ret.pushArg(getDefaultPhiPathBranchChoiceEvent(
                  br_label, format_str("%d", br_line)));
            else {
              ret.pushArg(getDefaultPhiPathListInternalItemEvent(
                  getDefaultPhiPathBranchChoiceEvent(
                      br_label, format_str("%d", br_line))));
            }
          }
        }
      }
    }

    return ret;
    // end of the lambda
  };

  auto generateFullPathChoiceInfo =
      [&, generateDiagInfo, dbg_loc](std::set<BasicBlock *> &path_choices) {
        if (path_choices.size() > 1) {
          DiagnosticTextList path_choice_list;
          unordered_set<string> cached_tips;
          size_t i = 0;

          bool has_new_step = false;
          BasicBlock *cached_incomming_bb;
          Value *cached_incomming_cond_val = nullptr;
          bool cached_incomming_cond = false;
          int cached_incomming_dbg_line = 0;
          Instruction *cached_incomming_br_inst = nullptr;

          for (BasicBlock *incoming_BB : path_choices) {
            Value *tmp_incomming_cond_val = nullptr;
            bool tmp_incomming_cond = false;
            int tmp_incomming_dbg_line = 0;
            Instruction *tmp_incomming_br_inst = nullptr;

            DiagnosticTextList PathChoiceItem = generateDiagInfo(
                incoming_BB, tmp_incomming_cond_val, tmp_incomming_cond,
                tmp_incomming_dbg_line, tmp_incomming_br_inst);
            string tip = PathChoiceItem.makeTip();

            if (cached_tips.empty() && tmp_incomming_cond_val != nullptr) {
              has_new_step = true;
              cached_incomming_bb = incoming_BB;
              cached_incomming_cond_val = tmp_incomming_cond_val;
              cached_incomming_cond = tmp_incomming_cond;
              cached_incomming_dbg_line = tmp_incomming_dbg_line;
              cached_incomming_br_inst = tmp_incomming_br_inst;
            }

            if (!cached_tips.count(tip)) {
              cached_tips.insert(tip);
              i++;
              path_choice_list.pushArg(getDefaultPhiPathListItemEvent(
                  format_str("%d", i), PathChoiceItem));
            }
          }

          if (cached_tips.size() == 1 && has_new_step) {
            if (!existing_Phi_cond_cache.count(cached_incomming_bb)) {
              BasicBlock *br_bb = cached_incomming_br_inst
                                      ? cached_incomming_br_inst->getParent()
                                      : nullptr;

              if (br_bb == last_step_BB ||
                  (!is_bb_reachable(br_bb, last_step_BB))) {
                existing_Phi_cond_cache.insert(cached_incomming_bb);

                DiagnosticLocation new_dbg_loc(dbg_loc);
                new_dbg_loc.setDbgLine(cached_incomming_dbg_line);
                new_dbg_loc.setIRSource(cached_incomming_br_inst);
                pushTrueOrFalseBranchEvent(new_dbg_loc, cached_incomming_cond);
                appendLastStepDescription(getDefaultPhiCondDescEvent(
                    decorator_emph_str(inst_resolver->restore_value_expr(
                        cached_incomming_cond_val, &phi_select_record)),
                    cached_incomming_cond ? "true" : "false"));
              }
            }

            return getDefaultEmptyPhiPathEvent();
          }
          return getDefaultPhiPathListEvent(path_choice_list);

        } else if (path_choices.size() == 1) {
          BasicBlock *incoming_BB = *path_choices.begin();

          Value *cached_incomming_cond_val = nullptr;
          bool cached_incomming_cond = false;
          int cached_incomming_dbg_line = 0;
          Instruction *cached_incomming_br_inst = nullptr;

          DiagnosticTextList PathChoice = generateDiagInfo(
              incoming_BB, cached_incomming_cond_val, cached_incomming_cond,
              cached_incomming_dbg_line, cached_incomming_br_inst);

          if (cached_incomming_cond_val != nullptr) {
            if (!existing_Phi_cond_cache.count(incoming_BB)) {
              BasicBlock *br_bb = cached_incomming_br_inst
                                      ? cached_incomming_br_inst->getParent()
                                      : nullptr;

              if (br_bb == last_step_BB ||
                  (!is_bb_reachable(br_bb, last_step_BB))) {
                existing_Phi_cond_cache.insert(incoming_BB);

                DiagnosticLocation new_dbg_loc(dbg_loc);
                new_dbg_loc.setDbgLine(cached_incomming_dbg_line);
                new_dbg_loc.setIRSource(cached_incomming_br_inst);
                pushTrueOrFalseBranchEvent(new_dbg_loc, cached_incomming_cond);

                appendLastStepDescription(getDefaultPhiCondDescEvent(
                    decorator_emph_str(inst_resolver->restore_value_expr(
                        cached_incomming_cond_val)),
                    cached_incomming_cond ? "true" : "false"));
              }
            }

            return getDefaultEmptyPhiPathEvent();
          }
          return (const DiagnosticText)PathChoice;
        } else {
          return getDefaultEmptyPhiPathEvent();
        }
      };

  Function *parent_func = phi->getParent()->getParent();

  bool should_find_phi = !(phi_node_cache.count(parent_func));
  std::unordered_set<PHINode *> &phi_nodes = phi_node_cache[parent_func];

  if (should_find_phi) {
    for (BasicBlock &BB : *parent_func) {
      for (Instruction &inst : BB) {
        if (PHINode *cur_phi = dyn_cast<PHINode>(&inst)) {
          phi_nodes.insert(cur_phi);
        }
      }
    }
  }

  std::set<BasicBlock *> path_choices;
  for (unsigned index = 0; index < phi->getNumOperands(); ++index) {
    if (phi->getIncomingValue(index) == val) {
      BasicBlock *incoming_BB = phi->getIncomingBlock(index);
      path_choices.insert(incoming_BB);

      // Guess the path select for other Phi Nodes, which is important for Dbg
      // Info Generation
      for (PHINode *phi_node_to_guess : phi_nodes) {

        // We only guess the value selection for PhiNodes that are not guessed
        if (phi_select_record.count(phi_node_to_guess))
          continue;

        int idx = phi_node_to_guess->getBasicBlockIndex(incoming_BB);
        if (idx >= 0) {
          phi_select_record[phi_node_to_guess] =
              phi_node_to_guess->getIncomingValue(idx);
        }
      }
    }
  }

  DiagnosticText phi_action = procPhiAction(phi, val, &dbg_loc);
  DiagnosticText path_description = generateFullPathChoiceInfo(path_choices);

  if (path_description.getEventID() == DefaultEmptyPhiPath &&
      phi_action.getEventID() == DefaultPhiReachable) {
    // Simple value Reachable with no condition has no help to bug comprehension
    // and can be ignored in trace
  } else {
    pushDecoratorEvent(dbg_loc, DefaultPhiInst, path_description, phi_action);
  }
}

RegisterDecoratorEventWithOneArgument(DefaultSelectCond, "Take $(1) condition",
                                      "");
RegisterDecoratorEventWithThreeArguments(
    DefaultSelect, "Take $(1) condition, assign $(2) to $(3)", "");
RegisterDecoratorEventWithTwoArguments(DefaultSelectWithNoTarget,
                                       "Take $(1) condition, choose value $(2)",
                                       "");
void LLVMValueReportDecorator::procSelectInst(const DiagnosticLocation &dbg_loc,
                                              SelectInst *select, Value *val) {
  phi_select_record[select] = val;

  Value *true_br_value = select->getTrueValue();
  const string label = (true_br_value == val ? "true" : "false");
  if (DIA->hasVariableDebugName(select)) {
    pushDecoratorEvent(dbg_loc, DefaultSelect, label,
                       getValueEvent(val, &dbg_loc),
                       decorator_emph_str(DIA->getVariableName(select)));
  } else {
    pushDecoratorEvent(dbg_loc, DefaultSelectWithNoTarget, label,
                       getValueEvent(val, &dbg_loc));
  }
}

RegisterDecoratorEventWithOneArgument(DefaultCallInstNoGettingIn, "Call $(1)",
                                      "");
RegisterDecoratorEventWithOneArgument(DefaultMemoryRelease,
                                      "The memory pointed by $(1) is freed",
                                      "");
RegisterDecoratorEventWithOneArgument(
    DefaultMemoryDestruct, "The memory pointed by $(1) is destructed", "");
RegisterDecoratorEventWithOneArgument(DefaultMemoryConstruct,
                                      "Construct the memory pointed by $(1)",
                                      "");
RegisterDecoratorEventWithOneArgument(DefaultMemoryAlloca,
                                      "Allocate memory to $(1)", "");
void LLVMValueReportDecorator::procCallInst(const DiagnosticLocation &dbg_loc,
                                            CallInst *call, Value *val) {
  Function *callee = call->getCalledFunction();
  Value *callee_val = call->getCalledOperand();
  auto *CS = cast<CallBase>(call);

  // Obtain the callee's name
  string callee_name;

  if (callee) {
    callee_name = decorator_emph_str(getReadableFunctionName(callee));
  } else if (llvm_utils::isVirtualCall(CS)) {
    // TODO: obtain the virtual function name
    callee_name = "";
  } else {
    callee_name = decorator_emph_str(
        inst_resolver->restore_value_expr(callee_val, &phi_select_record));
  }

  if (call == val) {
    // Situation 1: Receive the function returned value
    if (DIA->hasVariableDebugName(call)) {
      string target_var = decorator_emph_str(
          inst_resolver->restore_value_expr(call, &phi_select_record));

      if (memory_spec->isAllocatorLike(callee ? callee->getName().str()
                                              : std::string{})) {
        pushDecoratorEvent(dbg_loc, DefaultMemoryAlloca, target_var);
      } else {
        pushCallsiteCommonRetEvent(dbg_loc, target_var, callee_name);
      }
    } else {
      pushCallsiteEmptyRetEvent(dbg_loc, callee_name);
    }
  } else if (callee_val == val) {
    // Situation 2: A function callsite but we don't analyze the callee
    if (callee) {
      pushDecoratorEvent(dbg_loc, DefaultCallInstNoGettingIn, callee_name);
    } else {
      pushDecoratorEvent(dbg_loc, DefaultCallInstNoGettingIn,
                         decorator_emph_str(inst_resolver->restore_value_expr(
                             val, &phi_select_record)));
    }
  } else {
    // Situation 3: Pass argument to function and prepare for analyzing the
    // called function First obtain the argument index at the call site
    int arg_index = llvm_utils::getArgumentIndex(call, val);

    if (arg_index == -1) {
      // Defensive programming, val is not passed to call
      pushGeneralValueUsedEvent(
          dbg_loc, decorator_emph_str(inst_resolver->restore_value_expr(
                       val, &phi_select_record)));
    } else {
      const DiagnosticText &arg_val_desc = getValueEvent(val, &dbg_loc);

      // Second obtain the parameter for the callee function
      if (memory_spec &&
          memory_spec->isDeallocatorLike(callee ? callee->getName().str()
                                                : std::string{}) &&
          arg_index == 0) {
        pushDecoratorEvent(dbg_loc, DefaultMemoryRelease,
                           decorator_emph_str(inst_resolver->restore_value_expr(
                               val, &phi_select_record)));
      } else if (llvm_utils::isDestructor(callee) && arg_index == 0) {
        pushDecoratorEvent(dbg_loc, DefaultMemoryDestruct,
                           decorator_emph_str(inst_resolver->restore_value_expr(
                               val, &phi_select_record)));
      } else if (llvm_utils::isConstructor(callee) && arg_index == 0) {
        pushDecoratorEvent(dbg_loc, DefaultMemoryConstruct,
                           decorator_emph_str(inst_resolver->restore_value_expr(
                               val, &phi_select_record)));
      } else if ((!callee) || llvm_utils::getParameterAt(callee, arg_index)) {
        int k = adjustArgumentIndexForHuman(CS, arg_index);
        pushCallsiteCommonArgEvent(dbg_loc, arg_val_desc, k, callee_name);
      } else {
        // The argument is assigned to a variadic parameter
        pushCallsiteVaArgEvent(dbg_loc, arg_val_desc, callee_name);
      }
    }
  }
}

/**
 * Some private functions for assistance
 */

RegisterDecoratorEventWithOneArgument(DefaultCommonFunctionRetAsNoun,
                                      "the return value of function $(1)", "");
RegisterDecoratorEventWithOneArgument(DefaultVirtualFunctionRetAsNoun,
                                      "the return value of virtual call $(1)",
                                      "");
RegisterDecoratorEventWithOneArgument(DefaultIndirectFunctionRetAsNoun,
                                      "the return value of indirect call $(1)",
                                      "");
RegisterDecoratorEventWithNoArgument(DefaultUndefValue, DEFAULT_UNDEF_DESC, "");
DiagnosticText LLVMValueReportDecorator::getValueEvent(
    Value *val, const DiagnosticLocation *dbg_loc, bool is_deref_val) {
  assert(val != nullptr && "Generating a description for null value!!!");

  val = trackValueSource(val);

  if (!DIA->hasVariableDebugName(val) || isa<PHINode>(val)) {
    bool is_step_deleted = false;
    if (dbg_loc && !empty() && dbg_loc->getDbgLine() == back()->getDbgLine() &&
        dbg_loc->getDbgFileName() == back()->getDbgFileName()) {
      Value *last_track_back_val = nullptr;
      Value *track_back_val = val;
      Value *last_IR_source = back()->getIRSource();

      // If the last step is generated from the value, we remove the last step
      while (track_back_val != last_track_back_val) {
        last_track_back_val = track_back_val;

        if (last_IR_source == track_back_val) {
          // We delete the last step if the last step is used as an operand of
          // the following step
          is_step_deleted = true;
          removeEnd();
          break;
        }

        if (GEPOperator *gep = dyn_cast<GEPOperator>(track_back_val)) {
          track_back_val = gep->getPointerOperand();
        } else if (CastInst *cast = dyn_cast<CastInst>(track_back_val)) {
          if (cast->getNumOperands() > 0) {
            track_back_val = cast->getOperand(0);
          }
        } else {
          break;
        }
      }
    }

    if (isa<UndefValue>(val)) {
      return getDefaultUndefValueEvent();
    } else if (isa<CallInst>(val)) {
      CallInst *ci = cast<CallInst>(val);
      Function *func = ci->getCalledFunction();
      if (func) {
        return getDefaultCommonFunctionRetAsNounEvent(
            decorator_emph_str(getReadableFunctionName(func)));
      } else {
        auto *CS = cast<CallBase>(ci);
        if (llvm_utils::isVirtualCall(CS)) {
          // TODO: Should print the virtual function name
          // But under current architecture, we don't know which function was
          // in.
          string VirtualCallName = "";
          return getDefaultVirtualFunctionRetAsNounEvent(VirtualCallName);
        } else {
          return getDefaultIndirectFunctionRetAsNounEvent(decorator_emph_str(
              inst_resolver->restore_value_expr(ci->getCalledOperand())));
        }
      }
    } else if (SelectInst *select = dyn_cast<SelectInst>(val)) {
      Value *selected_val = nullptr;
      if (phi_select_record.count(select)) {
        selected_val = phi_select_record[select];
      }

      Value *cond_val = select->getCondition();
      Value *true_val = select->getTrueValue();
      if (selected_val) {
        if (is_step_deleted) {
          // description for select inst not exist in the trace, we add a new
          // event describing the value choice
          if (dbg_loc) {
            DiagnosticLocation new_dbg_loc(*dbg_loc);
            new_dbg_loc.setIRSource(nullptr);
            bool val_cond = selected_val == true_val;
            pushTrueOrFalseBranchEvent(new_dbg_loc, val_cond);
            appendLastStepDescription(getDefaultPhiCondDescEvent(
                decorator_emph_str(inst_resolver->restore_value_expr(cond_val)),
                val_cond ? "true" : "false"));
          } else {
            // unknown dbg line, we do nothing and only return the value choice
          }
        } else {
          // description for select inst exists in the trace, we only return the
          // selected value
        }

        if (!isa<UndefValue>(selected_val)) {
          return getValueEvent(selected_val, dbg_loc, is_deref_val);
        } else {
          // select a constant, the current step name shall be anyway, say, we
          // apply default procedure
        }
      } else {
        // A select with no name, and no last step hint, either branch is OK
        // we use the default procedure by using inst-resolver
      }
    } else if (PHINode *phi = dyn_cast<PHINode>(val)) {
      Value *selected_val = nullptr;
      if (phi_select_record.count(phi)) {
        selected_val = phi_select_record[phi];
      }

      if (selected_val && (!isa<UndefValue>(selected_val))) {
        return getValueEvent(selected_val, dbg_loc, is_deref_val);
      } else {
        // TODO: An unnamed phi with no value choice provided, we do not know
        // which value to choose Currently, we use default proceeding method,
        // which is improper...
      }
    }
  }

  string ret = inst_resolver->restore_value_expr(val, &phi_select_record);

  return DiagnosticTextUnit(decorator_emph_str(ret));
}
} // namespace lotus::gsaf

namespace lotus::gsaf {
using namespace llvm;
using namespace std;
using namespace lotus::reporting;
using namespace lotus::reporting::ReportDecorator;

#undef DEBUG_TYPE
#define DEBUG_TYPE "gsaf-report-decorator"

using namespace std;
using namespace REPORT_DECORATOR_NAMESPACE;

GSAFReportDecorator::GSAFReportDecorator()
    : report_decorator_var_setting(llvm::IntraLotusAAConfig::pts_setting) {}

GSAFReportDecorator::~GSAFReportDecorator() {}

bool GSAFReportDecorator::hasDebugInfo(const GuardedValueFlowObject *Obj) {
  if (!Obj)
    return false;

  if (auto *node = dyn_cast<GuardedValueFlowNode>(Obj)) {
    using K = GuardedValueFlowNode::Kind;
    switch (node->getKind()) {
    case K::CommonReturn:
    case K::LoadMemory:
    case K::Region:
    case K::SimpleOpcode:
    case K::CastOpcode:
      return false;
    case K::StoreMemory:
      return node->getDebugInstruction() != nullptr;
    case K::SimpleOperand:
      return node->getLLVMValue() != nullptr;
    default:
      return true;
    }
  }
  auto *site = cast<GuardedValueFlowSite>(Obj);
  return site->getKind() != GuardedValueFlowSite::Kind::Alloc;
}

RegisterDecoratorEventWithNoArgument(GSAFTraceStart, " (Trace staring point)",
                                     "");

void GSAFReportDecorator::buildFromTrace(
    std::shared_ptr<VulnerabilityTrace> &Trace, bool IsFinalize) {
#define OBJECT 0
#define INDEX 1

  auto HasDebugInfo = [this](const GuardedValueFlowObject *Obj) {
    return hasDebugInfo(Obj);
  };

  auto SrcIndexPair = Trace->find(0, HasDebugInfo);

  LLVM_DEBUG(errs() << "\n\n");

  for (size_t I = 1, E = Trace->get_length(); I < E; I++) {
    auto NodeIndexPair = Trace->find(I, HasDebugInfo);

    auto *CurrObj = std::get<OBJECT>(SrcIndexPair);
    auto *NextObj = std::get<OBJECT>(NodeIndexPair);

    if (NextObj == CurrObj) {
      I = std::get<INDEX>(NodeIndexPair);
      SrcIndexPair = std::move(NodeIndexPair);
      continue;
    }

    // cannot find the next operation
    if (!NextObj)
      break;

    bool CurrObjIsSite = isa<GuardedValueFlowSite>(CurrObj);
    bool NextObjIsSite = isa<GuardedValueFlowSite>(NextObj);

    LLVM_DEBUG(errs() << "[" << std::get<INDEX>(SrcIndexPair) << "] "
                      << *CurrObj << "\n";
               errs() << "[" << std::get<INDEX>(NodeIndexPair) << "] "
                      << *NextObj << "\n";);

    bool HitConstantExpr = false;

    Instruction *Operation;
    if (CurrObjIsSite && !NextObjIsSite) {
      // build tips for <site, operand>
      Operation = ((const GuardedValueFlowSite *)CurrObj)->getInstruction();
      if (auto *Output = dyn_cast<GuardedValueFlowCallOutputNode>(NextObj)) {
        Operation = Output->getCallSite();
      }
      assert(Operation);

      DiagnosticLocation DbgLoc(Operation, *DIA);
      procSiteNodePair(DbgLoc, (const GuardedValueFlowSite *)CurrObj,
                       (const GuardedValueFlowNode *)NextObj);

    } else if (!CurrObjIsSite && NextObjIsSite) {
      // build tips for <operand, site>
      Operation = ((const GuardedValueFlowSite *)NextObj)->getInstruction();
      if (!Operation) {
        llvm_unreachable("Fatal error: a null operation");
        continue;
      }

      DiagnosticLocation DbgLoc(Operation, *DIA);
      procNodeSitePair(DbgLoc, (const GuardedValueFlowNode *)CurrObj,
                       (const GuardedValueFlowSite *)NextObj);
    } else if (!CurrObjIsSite && !NextObjIsSite) {
      // build tips for <operand, operand>
      if (auto *N = nodeOfKind<GuardedValueFlowNode::Kind::SimpleOperand,
                               GuardedValueFlowNode>(NextObj)) {
        Operation = dyn_cast<Instruction>(N->getLLVMValue());
        if (!Operation) {
          auto *CE = dyn_cast<ConstantExpr>(N->getLLVMValue());
          assert(CE &&
                 "It must be an constant expr if it is not an instruction!");
          Operation = CE->getAsInstruction();
          HitConstantExpr = true;
        }
      } else if (auto *N = nodeOfKind<GuardedValueFlowNode::Kind::StoreMemory,
                                      GuardedValueFlowNode>(NextObj)) {
        Operation = N->getDebugInstruction();
        assert(Operation);
      } else if (auto *N = dyn_cast<GuardedValueFlowPhiNode>(NextObj)) {
        Operation = dyn_cast<Instruction>(N->getLLVMValue());
        assert(Operation);
      } else if (auto *N =
                     nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput,
                                GuardedValueFlowCallOutputNode>(NextObj)) {
        Operation = N->getCallSite();
        assert(Operation);
      } else if (isa<GuardedValueFlowReturnNode>(NextObj)) {
        Operation = nullptr;
      } else {
        errs() << DIA->getIRString(NextObj->getDebugValue()) << "\n";
        llvm_unreachable("Unknown trace pattern!");
        continue;
      }

      if (Operation) {
        DiagnosticLocation DbgLoc(Operation, *DIA);
        procNodeNodePair(DbgLoc, (const GuardedValueFlowNode *)CurrObj,
                         (const GuardedValueFlowNode *)NextObj);
      }
    } else {
      llvm_unreachable(
          "One use site cannot be followed by the other use site in a trace!");
      continue;
    }

    I = std::get<INDEX>(NodeIndexPair);
    SrcIndexPair = std::move(NodeIndexPair);

    // we must del the operation manually
    // if the operation is created via
    // a constant expr
    if (HitConstantExpr)
      Operation->deleteValue();
  }

  if (IsFinalize) {
    if ((!empty()) && report_decorator_var_setting == 1) {
      front()->appendAfter(ReportDecorator::getGSAFTraceStartEvent());
    }

    postProcess();
  }
  phi_select_record.clear();
}

void GSAFReportDecorator::procNodeSitePair(const DiagnosticLocation &DbgLoc,
                                           const GuardedValueFlowNode *Operand,
                                           const GuardedValueFlowSite *Site) {
  if (auto *CS = dyn_cast<GuardedValueFlowCallSite>(Site)) {
    if (isa<GuardedValueFlowCallOutputNode>(Operand) &&
        ((const GuardedValueFlowCallOutputNode *)Operand)->getCallSite() ==
            CS->getInstruction()) {
      procCallSiteOutput(DbgLoc, CS,
                         ((const GuardedValueFlowCallOutputNode *)Operand));
    } else {
      procCallSiteInput(DbgLoc, CS, Operand);
    }
  } else if (auto *RS = dyn_cast<GuardedValueFlowReturnSite>(Site)) {
    if (auto *PseudoReturn =
            nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn,
                       GuardedValueFlowReturnNode>(Operand)) {
      procPseudoReturn(DbgLoc, RS, PseudoReturn);
    } else {
      // common return
      procInstruction(DbgLoc, RS->getInstruction(), Operand->getLLVMValue());
    }
  } else if (auto *DS = operationalSite(Site)) {
    procInstruction(DbgLoc, DS->getInstruction(), Operand->getLLVMValue());
  }
}

void GSAFReportDecorator::procSiteNodePair(
    const DiagnosticLocation &DbgLoc, const GuardedValueFlowSite *Site,
    const GuardedValueFlowNode *Operand) {
  if (auto *CS = dyn_cast<GuardedValueFlowCallSite>(Site)) {
    if (auto *Output = dyn_cast<GuardedValueFlowCallOutputNode>(Operand)) {
      procCallSiteOutput(DbgLoc, CS, Output);
    } else if (!isa<GuardedValueFlowArgumentNode>(Operand)) {
      procCallSiteInput(DbgLoc, CS, Operand);
    }
  } else if (isa<GuardedValueFlowReturnSite>(Site)) {
    assert(Site->getGraph()->getBaseFunction() !=
           Operand->getGraph()->getBaseFunction());
    auto *Output = dyn_cast<GuardedValueFlowCallOutputNode>(Operand);
    assert(Output);
    procCallSiteOutput(DbgLoc, callSite(Output), Output);
  } else if (auto *DS = operationalSite(Site)) {
    procInstruction(DbgLoc, DS->getInstruction(), Operand->getLLVMValue());
  }
}

void GSAFReportDecorator::procCallSiteOutput(
    const DiagnosticLocation &DbgLoc, const GuardedValueFlowCallSite *CS,
    const GuardedValueFlowCallOutputNode *Output) {
  if (auto *PseudoOutput =
          nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoOutput,
                     GuardedValueFlowCallOutputNode>(Output)) {
    Function *Callee = PseudoOutput->getCallee();
    Instruction *Call = PseudoOutput->getCallSite();

    if (Callee) {
      string APName;
      string FuncName = decorator_emph_str(getReadableFunctionName(Callee));
      string ActualArgName;
      int ArgIdx = ARG_IDX_UNCLASSIFIED;
      string FormalArgName;

      //            int Offset4Reading = isClassMemberFunction(*Callee) ? 0 : 1;
      const gvfg::AccessPath &AP = PseudoOutput->getAccessPath();
      Value *APBasePtr = AP.get_base_ptr();

      if (APBasePtr) {
        // True is the call expr can be a left value
        bool is_left_value_call = true;

        if (!DIA->hasVariableDebugName(Call)) {
          // Right Value is Specially handled
          is_left_value_call = false;
        }

        if (Argument *BaseArg = dyn_cast<Argument>(APBasePtr)) {
          int Idx = 0;
          for (Argument &CalleeArg : Callee->args()) {
            if (&CalleeArg == BaseArg) {
              Value *ActualArg =
                  cast<CallBase>(CS->getInstruction())->getArgOperand(Idx);
              ActualArgName = inst_resolver->restore_value_expr(
                  ActualArg, &phi_select_record);
              APName =
                  inst_resolver->restore_access_path_expr(AP, ActualArgName);
              ArgIdx = adjustArgumentIndexForHuman(
                  cast<CallBase>(CS->getInstruction()), Idx);
              FormalArgName = decorator_emph_str(DIA->getVariableName(BaseArg));
              break;
            }
            Idx++;
          }
        } else if (isa<GlobalValue>(APBasePtr)) {
          ActualArgName = DIA->getVariableName(APBasePtr);
          APName = inst_resolver->restore_access_path_expr(AP, ActualArgName);
          ArgIdx = ARG_IDX_GLOBAL;
        } else {
          if (AP.isFromReturn()) {
            ActualArgName =
                inst_resolver->restore_value_expr(Call, &phi_select_record);
            APName = inst_resolver->restore_access_path_expr(AP, ActualArgName);
            ArgIdx = ARG_IDX_RETURN;
          } else {
            ActualArgName = "UNKNOWN";
            APName = inst_resolver->restore_access_path_expr(AP);
            ArgIdx = ARG_IDX_UNCLASSIFIED;
          }
        }

        ActualArgName = decorator_emph_str(ActualArgName);
        APName = decorator_emph_str(APName);

        pushCallsitePseudoRetEvent(DbgLoc, APName, FuncName, ActualArgName,
                                   ArgIdx, FormalArgName, is_left_value_call);
      } else {
        pushCallsiteEmptyRetEvent(DbgLoc, FuncName);
      }
    } else {
      Value *CalledValue =
          cast<CallBase>(CS->getInstruction())->getCalledOperand();
      if (CalledValue) {
        string FuncName = inst_resolver->restore_value_expr(CalledValue);
        pushCallsiteEmptyRetEvent(DbgLoc, FuncName);
      } else {
        pushGeneralInstExecutedEvent(DbgLoc);
      }
    }
  } else {
    // common callsite output
    procInstruction(DbgLoc, CS->getInstruction(), Output->getLLVMValue());
  }
}

void GSAFReportDecorator::procCallSiteInput(const DiagnosticLocation &DbgLoc,
                                            const GuardedValueFlowCallSite *CS,
                                            const GuardedValueFlowNode *Input) {
  if (auto *PseudoInput =
          nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput,
                     GuardedValueFlowCallOutputNode>(Input)) {
    // pseudo argument
    Function *Callee = PseudoInput->getCallee();
    if (Callee) {
      string callee_name = decorator_emph_str(getReadableFunctionName(Callee));

      const gvfg::AccessPath &AP = PseudoInput->getAccessPath();
      Value *APBasePtr = AP.get_base_ptr();
      if (APBasePtr) {
        if (Argument *BaseArg = dyn_cast<Argument>(APBasePtr)) {
          int Idx = 0;
          for (Argument &CalleeArg : Callee->args()) {
            if (&CalleeArg == BaseArg) {
              Value *Arg =
                  cast<CallBase>(CS->getInstruction())->getArgOperand(Idx);
              std::string ActualArgDbgInfo =
                  decorator_emph_str(inst_resolver->restore_left_value_expr(
                      Arg, &phi_select_record));
              std::string APDbgInfo =
                  decorator_emph_str(inst_resolver->restore_access_path_expr(
                      AP, ActualArgDbgInfo));

              int argIdx = adjustArgumentIndexForHuman(
                  cast<CallBase>(CS->getInstruction()), Idx);

              pushCallsitePseudoArgEvent(DbgLoc, APDbgInfo, callee_name,
                                         ActualArgDbgInfo, argIdx);
              break;
            }
            Idx++;
          }
        } else if (isa<GlobalValue>(APBasePtr)) {
          string ArgName = decorator_emph_str(DIA->getVariableName(APBasePtr));
          string APDbgInfo = decorator_emph_str(
              inst_resolver->restore_access_path_expr(AP, ArgName));
          pushCallsitePseudoArgEvent(DbgLoc, APDbgInfo, callee_name, ArgName,
                                     ARG_IDX_GLOBAL);
        } else {
          pushCallsiteSimpleEnterEvent(DbgLoc, callee_name);
        }
      }
    } else {
      pushCallsiteSimpleEnterEvent(DbgLoc);
    }
  } else if (CS->isCommonInput(Input)) {
    // common input
    procInstruction(DbgLoc, CS->getInstruction(), Input->getLLVMValue());
  } else {
    // A patch for Taint XXX
    if (LoadInst *LI = dyn_cast<LoadInst>(Input->getLLVMValue())) {
      procInstruction(DbgLoc, CS->getInstruction(), LI->getOperand(0));
    }
  }
}

void GSAFReportDecorator::procPseudoReturn(
    const DiagnosticLocation &DbgLoc, const GuardedValueFlowReturnSite *RS,
    const GuardedValueFlowReturnNode *PseudoReturn) {
  const gvfg::AccessPath &AP = PseudoReturn->getAccessPath();
  Value *APBasePtr = AP.get_base_ptr();
  if (APBasePtr == nullptr) {
    pushFunctionEmptyRetEvent(DbgLoc);
  } else {
    std::string APDbgInfo = inst_resolver->restore_access_path_expr(AP);
    if (isa<GlobalValue>(APBasePtr)) {
      pushFunctionPseudoRetEvent(
          DbgLoc, decorator_emph_str(APDbgInfo),
          decorator_emph_str(DIA->getVariableName(APBasePtr)), ARG_IDX_GLOBAL);
    } else if (AP.isFromReturn()) {
      pushFunctionPseudoRetEvent(
          DbgLoc, decorator_emph_str(APDbgInfo),
          decorator_emph_str(
              inst_resolver->restore_value_expr(APBasePtr, &phi_select_record)),
          ARG_IDX_RETURN);
    } else {
      // From Argument, here, we do not list the source of the Pseudo arg and
      // thus, ARG_IDX_UNCLASSIFIED is used
      pushFunctionPseudoRetEvent(
          DbgLoc, decorator_emph_str(APDbgInfo),
          decorator_emph_str(
              inst_resolver->restore_value_expr(APBasePtr, &phi_select_record)),
          ARG_IDX_UNCLASSIFIED);
    }
  }
}

void GSAFReportDecorator::procNodeNodePair(const DiagnosticLocation &DbgLoc,
                                           const GuardedValueFlowNode *From,
                                           const GuardedValueFlowNode *To) {
  if (auto *TSt = nodeOfKind<GuardedValueFlowNode::Kind::StoreMemory,
                             GuardedValueFlowNode>(To)) {
    // anything -> store-mem-node
    if (auto *SI = dyn_cast<StoreInst>(TSt->getDebugInstruction())) {
      procInstruction(DbgLoc, SI, From->getLLVMValue());
    }
  } else if (nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput>(To)) {
    // Fake Load by callsite pseudo input is redundant (with function entering
    // event existing in trace)
  } else {
    // anything -> node that is not in {store-mem-node, pseudo callsite input}
    if (isa<GuardedValueFlowArgumentNode>(From) ||
        isa<LoadInst>(To->getLLVMValue())) {
      if (!nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn>(To))
        procInstruction(DbgLoc, dyn_cast<Instruction>(To->getLLVMValue()),
                        To->getLLVMValue());
    } else {
      // in case constant expr
      auto *Op = dyn_cast<Instruction>(To->getLLVMValue());
      auto *Op2 =
          (Op ? Op
              : dyn_cast<ConstantExpr>(To->getLLVMValue())->getAsInstruction());
      procInstruction(DbgLoc, Op2, From->getLLVMValue());
      if (!Op)
        Op2->deleteValue();
    }
  }
}

void GSAFReportDecorator::postProcess() {}
using namespace llvm;
using namespace std;
using namespace lotus::reporting;
using namespace lotus::reporting::ReportDecorator;

GSAFTaintReportDecorator::GSAFTaintReportDecorator() {}

GSAFTaintReportDecorator::~GSAFTaintReportDecorator() {}

void GSAFTaintReportDecorator::buildFromTrace(
    std::shared_ptr<VulnerabilityTrace> &Trace, bool IsFinalize) {
  assert(Trace->get_length() > 3);

  auto *SrcNode = Trace->at(0);
  auto *SrcSite = Trace->at(1);
  assert(SrcNode);
  assert(SrcSite);
  assert(isa<GuardedValueFlowNode>(SrcNode));
  assert(isa<GuardedValueFlowSite>(SrcSite));

  std::shared_ptr<VulnerabilityTrace> StartSubTrace =
      std::make_shared<VulnerabilityTrace>();
  StartSubTrace->push(SrcNode);
  StartSubTrace->push(SrcSite);

  size_t WrapperTraceEnd = 2;
  if (Trace->at(2) != SrcNode && !Trace->at(3) &&
      isa<GuardedValueFlowNode>(Trace->at(2))) {
    // Find the taint wrapper
    std::stack<Function *> FrameStack;
    size_t I = 2;
    for (size_t E = Trace->get_length(); I < E; ++I) {
      auto *Obj = Trace->at(I);
      if (Obj) {
        if (isa<GuardedValueFlowNode>(Obj) && !Trace->at(I + 1)) {
          FrameStack.push(Obj->getGraph()->getBaseFunction());
        } else if (isa<GuardedValueFlowReturnSite>(Obj)) {
          FrameStack.pop();

          if (FrameStack.empty()) {
            break;
          }
        }

        if (Obj->getGraph()->getBaseFunction() != FrameStack.top()) {
          break;
        }

        StartSubTrace->push(Obj);
      } else {
        StartSubTrace->push(Obj);
      }
    }

    if (!FrameStack.empty()) {
      WrapperTraceEnd = I;
    } else {
      StartSubTrace->clear();
      StartSubTrace->push(SrcNode);
      StartSubTrace->push(SrcSite);
    }
  }

  GSAFReportDecorator::buildFromTrace(StartSubTrace, false);

  // produce tips for alias relations TODO
  // Searching the alias part
  size_t I = WrapperTraceEnd;
  for (size_t E = Trace->get_length(); I < E; ++I) {
    assert(I + 1 < E);

    auto *CurrObj = Trace->at(I);
    auto *NextObj = Trace->at(I + 1);

    if (!NextObj) {
      // Here it must be a format like:
      // src-node nullptr(src-site is nullptr) src-node
      assert(I + 2 < E);
      assert(CurrObj == Trace->at(I + 2));
      ++I;
      continue;
    }

    bool CurrObjIsSite = isa<GuardedValueFlowSite>(CurrObj);
    bool NextObjIsSite = isa<GuardedValueFlowSite>(NextObj);
    assert(!(CurrObjIsSite && NextObjIsSite) &&
           "Two GVFGSites cannot be adjacent to each other in the trace!");

    if (!CurrObjIsSite && !NextObjIsSite) {
      if (((const GuardedValueFlowNode *)NextObj)
              ->containsParent((const GuardedValueFlowNode *)CurrObj)) {
        // okay
      } else {
        break;
      }
    } else if (NextObjIsSite) {
      if (isa<GuardedValueFlowCallOutputNode>(CurrObj) &&
          callSite((const GuardedValueFlowCallOutputNode *)CurrObj) ==
              NextObj) {
        // search the output summary in alias part.
        // Thus, there must be something after the site
        assert(I + 3 < E);
        assert(isa<GuardedValueFlowNode>(Trace->at(I + 2)) &&
               "Output summary must start with an argument!");
        assert(!Trace->at(I + 3) &&
               "The source site of an output summary must be nullptr!");

        std::stack<Function *> FrameStack;

        size_t II = I + 2;
        for (; II < E; ++II) {
          auto *Obj = Trace->at(II);
          if (Obj) {
            if (isa<GuardedValueFlowNode>(Obj) && !Trace->at(II + 1)) {
              FrameStack.push(Obj->getGraph()->getBaseFunction());
            } else if (isa<GuardedValueFlowReturnSite>(Obj)) {
              FrameStack.pop();
              if (FrameStack.empty()) {
                break;
              }
            }
          }
        }

        // set I be the index of the last ret site
        assert(II != E);
        I = II;
      } else {
        break;
      }
    } else {
      break;
    }
  }

  // produce tips for value flows
  std::shared_ptr<VulnerabilityTrace> SubTrace =
      std::make_shared<VulnerabilityTrace>();
  for (size_t E = Trace->get_length(); I < E; ++I) {
    SubTrace->push(Trace->at(I));
  }

  GSAFReportDecorator::buildFromTrace(SubTrace, IsFinalize);
}
} // namespace lotus::gsaf
