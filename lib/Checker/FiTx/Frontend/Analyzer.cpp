// FiTx CFG-based typestate analysis: path-insensitive, inter-procedural,
// return-code aware state propagation (Suzuki et al., USENIX ATC 2024,
// §4.2, 4.3).
#include "Checker/FiTx/Frontend/Analyzer.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Instructions/BranchInstruction.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/FrameworkIR/IRGenerator.h"
#include "Checker/FiTx/Frontend/CommandlineArgs.h"
#include "Checker/FiTx/Frontend/Function.h"
#include "Checker/FiTx/Frontend/PropagationConstraint.h"
#include "Checker/FiTx/Frontend/StateTransition.h"
#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

namespace {
void appendUniqueValue(llvm::SmallVectorImpl<std::shared_ptr<fitx::Value>> &out,
                       const std::shared_ptr<fitx::Value> &val) {
  if (val && std::find(out.begin(), out.end(), val) == out.end())
    out.push_back(val);
}

struct FiTxBugSpec {
  const char *bug_name;
  BugDescription::BugImportance importance;
  BugDescription::BugClassification classification;
  const char *description;
  const char *suggestion;
};

FiTxBugSpec getFiTxBugSpec(llvm::StringRef state_name) {
  if (state_name == "UBI") {
    return {"Use of Uninitialized Value", BugDescription::BI_HIGH,
            BugDescription::BC_ERROR, "Value may be used before initialization",
            "Initialize the value on all paths before it is read."};
  }
  if (state_name == "Used") {
    return {"Use After Free", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY,
            "Value may be dereferenced after it has been freed",
            "Ensure the value is not used after the free-like operation."};
  }
  if (state_name == "used") {
    return {"Null Pointer Dereference", BugDescription::BI_HIGH,
            BugDescription::BC_ERROR,
            "Value may be dereferenced after becoming null",
            "Guard the dereference or guarantee the value is non-null."};
  }
  if (state_name == "double free") {
    return {"Double Free", BugDescription::BI_HIGH, BugDescription::BC_SECURITY,
            "Value may be freed more than once",
            "Ensure ownership is transferred once and the free path is not "
            "repeated."};
  }
  if (state_name == "double locked") {
    return {"Double Lock", BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
            "Lock may be acquired twice without an intervening unlock",
            "Check lock ownership and ensure each acquisition has a matching "
            "release."};
  }
  if (state_name == "double unlocked") {
    return {
        "Double Unlock", BugDescription::BI_MEDIUM, BugDescription::BC_ERROR,
        "Lock may be released twice",
        "Track lock ownership so each unlock corresponds to one held lock."};
  }
  if (state_name == "allocated") {
    return {"Memory Leak", BugDescription::BI_MEDIUM,
            BugDescription::BC_PERFORMANCE,
            "Allocated memory may escape without being released",
            "Ensure the allocation is released on all non-returning paths."};
  }
  if (state_name.substr(0, 8) == "counted ") {
    return {
        "Reference Count Imbalance", BugDescription::BI_MEDIUM,
        BugDescription::BC_ERROR,
        "Reference count may end in an imbalanced state",
        "Review retain/release pairs and verify the final reference count."};
  }
  if (state_name.substr(0, 10) == "uncounted ") {
    return {"Reference Count Underflow", BugDescription::BI_HIGH,
            BugDescription::BC_ERROR,
            "Reference count may be decremented too many times",
            "Ensure decrements are paired with successful increments."};
  }
  return {"FiTx Finding", BugDescription::BI_MEDIUM, BugDescription::BC_WARNING,
          "FiTx detected a typestate violation",
          "Inspect the trace and verify the relevant state transitions."};
}

int getOrRegisterFiTxBugType(const fitx::State &state) {
  const FiTxBugSpec spec = getFiTxBugSpec(state.Name());
  BugReportMgr &mgr = BugReportMgr::get_instance();
  return mgr.register_bug_type(spec.bug_name, spec.importance,
                               spec.classification, spec.description);
}

std::string renderFiTxValue(const std::shared_ptr<fitx::Value> &value) {
  if (!value) {
    return "tracked value";
  }

  std::string rendered;
  llvm::raw_string_ostream stream(rendered);
  stream << *value;
  return stream.str();
}

std::string renderTransitionTip(const fitx::TransitionLogs::TraceEntry &entry) {
  const std::string &source = entry.transition.Source().Name();
  const std::string &target = entry.transition.Target().Name();

  if (target == "allocated") {
    return "The value becomes allocated here.";
  }
  if (target == "free") {
    return "The value is released here.";
  }
  if (target == "null") {
    return "The value becomes null here.";
  }
  if (target == "Used") {
    return "The previously freed value is used here.";
  }
  if (target == "used") {
    return "The null value is used here.";
  }
  if (target == "UBI") {
    return "The analysis reaches a use of an uninitialized value here.";
  }

  return "State changes from " + source + " to " + target + ".";
}

std::string renderPrimaryTip(const fitx::State &state,
                             const std::shared_ptr<fitx::Value> &value) {
  const std::string &state_name = state.Name();
  (void)value;

  if (state_name == "UBI") {
    return "Potential use of uninitialized value";
  }
  if (state_name == "Used") {
    return "Potential use-after-free";
  }
  if (state_name == "used") {
    return "Potential null pointer dereference";
  }
  if (state_name == "double free") {
    return "Potential double free";
  }
  if (state_name == "double locked") {
    return "Potential double lock";
  }
  if (state_name == "double unlocked") {
    return "Potential double unlock";
  }
  if (state_name == "allocated") {
    return "Potential memory leak";
  }
  if (state_name.rfind("counted ", 0) == 0) {
    return "Potential reference count imbalance";
  }
  if (state_name.rfind("uncounted ", 0) == 0) {
    return "Potential reference count underflow";
  }

  return "Potential FiTx bug (" + state_name + ")";
}

int inferConfidenceScore(const fitx::State &state,
                         const std::shared_ptr<fitx::Value> &value) {
  int score = 90;
  if (state.NotificationTiming() == fitx::MODULE_END) {
    score = 75;
  } else if (state.NotificationTiming() == fitx::FUNCTION_END) {
    score = 80;
  }
  if (value && value->isArbitaryArrayElement()) {
    score -= 10;
  }
  return std::max(0, score);
}

void appendFiTxReport(const fitx::State &state,
                      const std::shared_ptr<fitx::Value> &value,
                      const fitx::TransitionLogs &logs) {
  int bug_type_id = getOrRegisterFiTxBugType(state);
  auto *report = new BugReport(bug_type_id);
  const FiTxBugSpec spec = getFiTxBugSpec(state.Name());

  for (const fitx::TransitionLogs::TraceEntry &entry : logs.getTraceEntries()) {
    llvm::Instruction *llvm_inst =
        entry.instruction ? entry.instruction->LLVMInstruction() : nullptr;
    report->append_step(llvm_inst, renderTransitionTip(entry), 0, {}, "", true);
  }

  llvm::Instruction *bug_inst =
      logs.CurrentInstruction() ? logs.CurrentInstruction()->LLVMInstruction()
                                : nullptr;
  report->append_step(bug_inst, renderPrimaryTip(state, value), 0,
                      {NodeTag::NONE}, "", true);
  report->set_conf_score(inferConfidenceScore(state, value));
  report->set_suggestion(spec.suggestion);
  report->add_metadata("fitx_state", state.Name());
  report->add_metadata("fitx_value", renderFiTxValue(value));

  std::string reduced_transition;
  llvm::raw_string_ostream transition_stream(reduced_transition);
  transition_stream << logs.ReducedTransition();
  report->add_metadata("fitx_transition", transition_stream.str());

  BugReportMgr::get_instance().insert_report(bug_type_id, report, true);
}
} // namespace

namespace fitx {
Analyzer::Analyzer(llvm::Module &llvm_module, fitx::StateManager &state_manager,
                   fitx::LoggingClient &client)
    : llvm_module_(llvm_module), state_manager_(state_manager), log_(client) {}

void Analyzer::analyze() {
  if (!state_manager_.TransitionManager()->canTriggerFromInit(llvm_module_)) {
    log_.flush();
    return;
  }

  auto &framework_ir = ir_generator::IRGenerator::framework_ir_;
  auto mod_it = framework_ir.find(&llvm_module_);
  if (mod_it == framework_ir.end())
    return;
  for (const auto &function : mod_it->second) {
    analyzeFunction(function);
  }
  log_.flush();
}

void Analyzer::analyzeFunction(std::shared_ptr<fitx::Function> function) {
  auto &func_info = function_info_[function];
  if (!func_info)
    func_info = std::make_shared<fitx::FunctionInformation>(function);

  if (func_info->Stat() != FunctionInformation::UNANALYZED)
    return;
  func_info->setAnalysisStat(
      fitx::FunctionInformation::AnalysisStat::IN_PROGRESS);

  analyzing_function_.push(function);
  const auto &target_blocks = function->OrderedBasicBlocks();
  std::queue<std::shared_ptr<fitx::BasicBlock>> block_queue(
      std::deque<std::shared_ptr<fitx::BasicBlock>>(target_blocks.begin(),
                                                    target_blocks.end()));

  while (!block_queue.empty()) {
    auto block = block_queue.front();
    bb_info_ =
        func_info->createBasicBlockInfo(block, state_manager_.getStates());
    func_info->setAnayzingBasicBlock(block);
    analyzePrevBlockBranch(block);
    for (const auto &inst : block->Instructions()) {
      switch (inst->Opcode()) {
      case llvm::Instruction::Call:
        analyzeCallInst(inst);
        break;
      case llvm::Instruction::Store:
        analyzeStoreInst(inst);
        break;
      case llvm::Instruction::Load:
        analyzeLoadInst(inst);
        break;
      default:
        break;
      }
    }
    block_queue.pop();

    if (func_info->basicBlockInfoChanged(block)) {
      block_queue.push(block);
      continue;
    }

    generateError(BugNotificationTiming::IMMEDIATE);
    if (!block->DeadValues().empty())
      generateError(BugNotificationTiming::END_OF_LIFE, &block->DeadValues());
  }

  analyzeReturnValue(function);

  bb_info_ = func_info->getBasicBlockInformation(function->ReturnBlock());
  if (bb_info_) {
    generateError(BugNotificationTiming::FUNCTION_END);

    if (function->CallerFunctions().empty())
      generateError(BugNotificationTiming::MODULE_END);
  }

  analyzing_function_.pop();
  func_info->setAnalysisStat(fitx::FunctionInformation::AnalysisStat::ANALYZED);

  // Release intra-procedural per-block state immediately after analyzing this
  // function; only compact exit arg summaries are retained for callers.
  bb_info_.reset();
  func_info->releaseAnalysisMemory(!function->CallerFunctions().empty());
}

void Analyzer::analyzePrevBlockBranch(std::shared_ptr<fitx::BasicBlock> block) {
  auto tm = state_manager_.TransitionManager();
  const auto &null_branch_null = tm->getStoreArgTransitions(
      fitx::StoreValueTransitionRule::NULL_BRANCH_CONSIDERED_NULL);
  const auto &null_branch_non_null = tm->getStoreArgTransitions(
      fitx::StoreValueTransitionRule::NULL_BRANCH_CONSIDERED_NON_NULL);
  const auto &null_branch_any = tm->getStoreArgTransitions(
      fitx::StoreValueTransitionRule::NULL_BRANCH_CONSIDERED_ANY);

  for (const auto &pred_reference : block->Predecessors()) {
    auto preds = pred_reference.lock();
    if (!preds)
      continue;
    auto pred_bb_info =
        currentFunctionInformation()->getBasicBlockInformation(preds);
    if (!pred_bb_info)
      continue;

    auto branch_inst = preds->getBranchInst();
    if (!branch_inst || !branch_inst->Condition())
      continue;

    auto condition_inst =
        shared_dyn_cast<fitx::CompareInst>(branch_inst->Condition());
    if (!condition_inst)
      continue;

    bool null_exists = false;
    std::shared_ptr<fitx::Value> comp_value = nullptr;
    for (const auto &operand : condition_inst->Operands()) {
      if (shared_isa<fitx::NullValue>(operand)) {
        null_exists = true;
        continue;
      }
      if (shared_isa<fitx::ConstValue>(operand))
        continue;
      comp_value = operand;
    }
    if (!comp_value)
      continue;

    auto call_inst = shared_dyn_cast<CallInst>(comp_value);
    if (call_inst && call_inst->CalledFunction() &&
        Function::IsExpectFunction(call_inst->CalledFunction())) {
      for (const auto &args : call_inst->Arguments())
        generateWarning(call_inst.get(), args.get());
    }

    if (!null_exists)
      continue;

    const BranchInst::TransitionNodes &null_nodes =
        condition_inst->GetPredicate() == llvm::CmpInst::ICMP_EQ
            ? branch_inst->TruePathNodes()
            : branch_inst->FalsePathNodes();
    bool is_null_path =
        std::find_if(null_nodes.begin(), null_nodes.end(),
                     [&block](const auto &node) {
                       return node.lock() == block;
                     }) != null_nodes.end();

    const auto &possible_transitions =
        is_null_path ? null_branch_null : null_branch_non_null;

    if (!possible_transitions.empty() || !null_branch_any.empty()) {
      llvm::SmallVector<fitx::Transition, 4> combined;
      llvm::ArrayRef<fitx::Transition> active = possible_transitions;
      if (possible_transitions.empty()) {
        active = null_branch_any;
      } else if (!null_branch_any.empty()) {
        combined.reserve(null_branch_any.size() + possible_transitions.size());
        combined.append(null_branch_any.begin(), null_branch_any.end());
        combined.append(possible_transitions.begin(),
                        possible_transitions.end());
        active = combined;
      }

      llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> related_values;
      const auto &vc = currentFunctionInformation()->GetValueCollection();
      vc.collectRelatedValues(comp_value, related_values);
      appendUniqueValue(related_values, comp_value);
      if (const auto *aliased =
              pred_bb_info->getAliasValues().getAliasInfo(comp_value)) {
        for (const auto &av : aliased->Values())
          appendUniqueValue(related_values, av);
      }

      generateWarning(branch_inst.get(), "Branch Inst Transition");
      for (const auto &value : related_values) {
        changeValueState(active, value, branch_inst);
      }
      generateWarning(branch_inst.get(), "Branch Inst Transition Done");
    }

    int remove_ret_val = is_null_path ? 0 : -1;
    bb_info_->removeReturnvalue(remove_ret_val);
  }
}

void Analyzer::analyzeCallInst(std::shared_ptr<fitx::Instruction> I) {
  auto call_inst = std::static_pointer_cast<fitx::CallInst>(I);

  if (analyzeFunctionCall(call_inst))
    return;

  auto function = call_inst->CalledFunction();
  if (!function) {
    const auto &vc = currentFunctionInformation()->GetValueCollection();
    for (const auto &value : call_inst->Arguments()) {
      llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> related_values;
      vc.collectRelatedValues(value, related_values);
      appendUniqueValue(related_values, value);
      for (const auto &related : related_values) {
        bb_info_->removeValueFromState(related, I);
      }
    }
    return;
  }

  if (fitx::Function::IsDebugDeclareFunction(function)) {
    if (!call_inst->Arguments().empty())
      bb_info_->resetValueState(call_inst->Arguments()[0], I);
    return;
  }

  if (function->isDebugFunction())
    return;

  if (Function::IsMemSetFunction(function) && !call_inst->Arguments().empty()) {
    const auto &transitions =
        state_manager_.TransitionManager()->getStoreArgTransitions(
            fitx::StoreValueTransitionRule::ANY);
    if (!transitions.empty()) {
      llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> related_values;
      currentFunctionInformation()->GetValueCollection().collectRelatedValues(
          call_inst->Arguments()[0], related_values);
      appendUniqueValue(related_values, call_inst->Arguments()[0]);
      for (const auto &value : related_values) {
        changeValueState(transitions, value, I);
      }
    }
    return;
  }

  if (function->isDeclaration()) {
    const auto &vc = currentFunctionInformation()->GetValueCollection();
    for (const auto &value : call_inst->Arguments()) {
      llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> related_values;
      vc.collectRelatedValues(value, related_values);
      appendUniqueValue(related_values, value);
      for (const auto &related : related_values) {
        bb_info_->removeValueFromState(related, I);
      }
    }
  } else {
    if (state_manager_.getStatefulConstraint() &&
        !state_manager_.getStatefulConstraint()->shouldPropagateOnCallInst(
            call_inst)) {
      return;
    }
    analyzeFunction(function);
    bb_info_ = function_info_[analyzing_function_.top()]
                   ->getCurrentBasicBlockInformation();
    copyFunctionValues(function, call_inst);
  }
}

void Analyzer::analyzeStoreInst(std::shared_ptr<fitx::Instruction> I) {
  auto store_inst = std::static_pointer_cast<fitx::StoreInst>(I);
  auto tm = state_manager_.TransitionManager();
  auto value_operand = store_inst->ValueOperand();

  const auto &val_transitions = tm->getStoreArgTransitions(
      fitx::shared_isa<fitx::NullValue>(value_operand)
          ? fitx::StoreValueTransitionRule::NULL_VAL
          : fitx::StoreValueTransitionRule::NON_NULL_VAL);

  static const std::vector<fitx::Transition> kEmptyTransitions;
  const std::vector<fitx::Transition> *call_transitions = &kEmptyTransitions;
  if (auto call_inst = fitx::shared_dyn_cast<fitx::CallInst>(value_operand)) {
    if (auto called_func = call_inst->CalledFunction()) {
      call_transitions = &tm->getStoreArgTransitions(
          fitx::StoreValueTransitionRule::CALL_FUNC, called_func->Name());
    }
  }

  const auto &any_transitions =
      tm->getStoreArgTransitions(fitx::StoreValueTransitionRule::ANY);

  const unsigned non_empty_count = (!val_transitions.empty() ? 1u : 0u) +
                                   (!call_transitions->empty() ? 1u : 0u) +
                                   (!any_transitions.empty() ? 1u : 0u);

  if (non_empty_count > 0) {
    llvm::SmallVector<fitx::Transition, 4> combined;
    llvm::ArrayRef<fitx::Transition> active;
    if (non_empty_count == 1) {
      if (!val_transitions.empty())
        active = val_transitions;
      else if (!call_transitions->empty())
        active = *call_transitions;
      else
        active = any_transitions;
    } else {
      combined.reserve(val_transitions.size() + call_transitions->size() +
                       any_transitions.size());
      combined.append(val_transitions.begin(), val_transitions.end());
      combined.append(call_transitions->begin(), call_transitions->end());
      combined.append(any_transitions.begin(), any_transitions.end());
      active = combined;
    }

    llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> related_values;
    currentFunctionInformation()->GetValueCollection().collectRelatedValues(
        store_inst->PointerOperand(), related_values);
    appendUniqueValue(related_values, store_inst->PointerOperand());
    for (const auto &value : related_values) {
      changeValueState(active, value, I);
    }
  }

  checkAlias(store_inst);
}

void Analyzer::checkAlias(std::shared_ptr<fitx::StoreInst> store_inst) {
  auto value_operand = store_inst->ValueOperand();
  auto &alias_vals = bb_info_->getAliasValues();

  const ValueCollection *aliased = alias_vals.getAliasInfo(value_operand);

  alias_vals.addAlias(store_inst->PointerOperand(), value_operand);

  const auto &possible_transitions =
      state_manager_.TransitionManager()->getAliasTransitions();
  if (possible_transitions.empty())
    return;

  if (shared_isa<fitx::ConstValue>(value_operand) ||
      shared_isa<fitx::NullValue>(value_operand) ||
      (shared_isa<fitx::CallInst>(value_operand) && !aliased))
    return;

  const auto &vc = currentFunctionInformation()->GetValueCollection();
  llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> related_values;
  vc.collectRelatedValues(store_inst->PointerOperand(), related_values);
  appendUniqueValue(related_values, store_inst->PointerOperand());

  if (!shared_isa<fitx::CallInst>(value_operand)) {
    llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> value_related;
    vc.collectRelatedValues(value_operand, value_related);
    for (const auto &vr : value_related)
      appendUniqueValue(related_values, vr);
    appendUniqueValue(related_values, value_operand);
  }

  if (aliased) {
    for (const auto &av : aliased->Values())
      appendUniqueValue(related_values, av);
  }

  for (const auto &value : related_values) {
    changeValueState(possible_transitions, value, store_inst);
  }
}

void Analyzer::analyzeLoadInst(std::shared_ptr<fitx::Instruction> I) {
  const auto &transitions =
      state_manager_.TransitionManager()->getUseValueTransitions();
  if (transitions.empty())
    return;
  auto load_inst = std::static_pointer_cast<fitx::LoadInst>(I);
  changeValueState(transitions, load_inst->LoadValue(), I);
}

bool Analyzer::functionInformationExists(
    std::shared_ptr<fitx::Function> function) {
  return function_info_.find(function) != function_info_.end();
}

void Analyzer::changeValueState(
    llvm::ArrayRef<Transition> transitions,
    const std::shared_ptr<Value> &value,
    const std::shared_ptr<fitx::Instruction> &inst) {
  if (!value || value->isGlobalVar() || transitions.empty())
    return;
  if (bb_info_->changeValueState(transitions, value, inst)) {
    currentFunctionInformation()->addValue(value);
    fitx::generateWarning(inst.get(), "[Value Change] Change Value State: ");
    fitx::generateWarning(inst.get(), value.get());
  }
}

void Analyzer::generateError(
    BugNotificationTiming timing,
    const std::set<std::shared_ptr<fitx::Value>, BasicBlock::ValuePtrCompare>
        *values) {
  for (const auto &state : state_manager_.getBugStates()) {
    if (state.NotificationTiming() != timing)
      continue;
    for (auto &value : bb_info_->getValueTransitionStates(state)) {
      if (values && !values->empty() &&
          values->find(value.first) == values->end())
        continue;
      if (value.second->CurrentState() != state)
        continue;
      if (!value.second->LeastSignificantSource().isInitState())
        continue;

      if (state.getTriggerConstraint() == TriggerConstraint::NON_RETURN &&
          value.first ==
              currentFunctionInformation()->Function()->getReturnValue()) {
        continue;
      }

      if (fitx::CommandLineArgs::ReportAllCandidates ||
          !value.first->isArbitaryArrayElement()) {
        appendFiTxReport(state, value.first, *value.second);
      }
      value.second->logicalTerminate(value.second->CurrentInstruction());
    }
  }
}

void Analyzer::copyFunctionValues(std::shared_ptr<fitx::Function> called_func,
                                  std::shared_ptr<fitx::CallInst> call_inst) {
  if (!functionInformationExists(called_func))
    return;
  auto called_func_info = function_info_[called_func];

  if (called_func->ProtectedRefcountValue() &&
      called_func->lastRefcountInstruction() != call_inst) {
    return;
  }

  if (addPendingFunctionValues(called_func, call_inst))
    return;

  const auto &success_blocks = called_func_info->getSuccessBlock();
  if (success_blocks.empty())
    return;

  const auto &operands = call_inst->Arguments();
  if (operands.empty())
    return;

  // Fast path: when there is <= 1 non-empty exit block summary, read directly
  // without allocating/merging a temporary ArgValueStates object.
  const ArgValueStates *direct_states = nullptr;
  ArgValueStates merged_states;
  bool use_merged = false;
  for (const auto &success_block_ref : success_blocks) {
    auto success_block = success_block_ref.lock();
    if (!success_block)
      continue;
    auto basic_block_info =
        called_func_info->getBasicBlockInformation(success_block);
    if (!basic_block_info || basic_block_info->getArgValueStates().empty())
      continue;
    const auto &block_arg_states = basic_block_info->getArgValueStates();
    if (!direct_states && !use_merged) {
      direct_states = &block_arg_states;
    } else {
      if (!use_merged) {
        merged_states = ArgValueStates(operands.size(), state_manager_.getStates());
        merged_states.addArgValueState(*direct_states);
        direct_states = nullptr;
        use_merged = true;
      }
      merged_states.addArgValueState(block_arg_states);
    }
  }

  const ArgValueStates *active_states =
      use_merged ? &merged_states : direct_states;
  if (!active_states || active_states->empty())
    return;

  generateWarning(call_inst.get(), "Call Inst Here");
  const bool block_values_empty = bb_info_->ValueStates().ValueStates().empty();
  const bool protect_refcount = called_func->ProtectedRefcountValue() != nullptr;
  for (size_t i = 0; i < operands.size(); i++) {
    const auto &arg_transitions = active_states->getArgTransitions(i);
    if (arg_transitions.empty())
      continue;
    const auto &operand = operands[i];
    if (operand && operand->isGlobalVar())
      continue;
    const bool non_arg_empty_block =
        operand && !fitx::shared_isa<fitx::Argument>(operand) &&
        block_values_empty;
    for (const auto &value : arg_transitions) {
      llvm::SmallVector<Transition, 4> reduced;
      value.second.collectReducedTransitions(reduced);
      if (reduced.empty())
        continue;
      if (non_arg_empty_block) {
        bool has_init = false;
        for (const auto &t : reduced) {
          if (t.Source().isInitState()) {
            has_init = true;
            break;
          }
        }
        if (!has_init)
          continue;
      }
      auto new_value = Value::CreateAppend(operand, value.first);
      if (protect_refcount && fitx::shared_isa<fitx::Argument>(new_value))
        continue;
      changeValueState(reduced, new_value, call_inst);
    }
  }
}

bool Analyzer::addPendingFunctionValues(
    std::shared_ptr<fitx::Function> called_func,
    std::shared_ptr<fitx::CallInst> call_inst) {
  auto branch_inst =
      currentFunctionInformation()->currentBasicBlock()->getBranchInst();
  if (!branch_inst || !branch_inst->Condition())
    return false;

  generateWarning(call_inst.get(), "Found BranchInst");

  int64_t compared_value = fitx::FunctionInformation::kSuccessCode;
  llvm::CmpInst::Predicate predicate = llvm::CmpInst::Predicate::ICMP_NE;
  bool is_null_value = false;

  if (auto compare_inst =
          fitx::shared_dyn_cast<fitx::CompareInst>(branch_inst->Condition())) {
    if (!compare_inst->operandExists(call_inst)) {
      return false;
    }
    generateWarning(call_inst.get(), "Found CompareInst");

    predicate = compare_inst->GetPredicate();

    auto operand = std::find_if(
        compare_inst->Operands().begin(), compare_inst->Operands().end(),
        [&call_inst](const auto &op) { return op != call_inst; });

    if (operand == compare_inst->Operands().end())
      return false;

    if (auto const_value = fitx::shared_dyn_cast<fitx::ConstValue>(*operand)) {
      compared_value = const_value->getConstValue();
    } else if (fitx::shared_isa<fitx::NullValue>(*operand)) {
      is_null_value = true;
    }
  } else if (fitx::shared_isa<fitx::CallInst>(branch_inst->Condition())) {
    if (call_inst != branch_inst->Condition())
      return false;
    generateWarning(call_inst.get(), "Found Call Inst instead");
  } else {
    return false;
  }

  if (!functionInformationExists(called_func))
    return false;
  auto called_func_info = function_info_[called_func];
  generateWarning(call_inst.get(), "Found called func info");

  auto basic_block_info =
      currentFunctionInformation()->getCurrentBasicBlockInformation();
  for (const auto &ret : called_func_info->getReturnValueInfo()) {
    bool is_true_path = false;
    switch (predicate) {
    case llvm::CmpInst::Predicate::ICMP_EQ:
      is_true_path = ret.first == compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_NE:
      is_true_path = ret.first != compared_value;
      if (is_null_value)
        is_true_path = !is_true_path;
      break;
    case llvm::CmpInst::Predicate::ICMP_SLE:
    case llvm::CmpInst::Predicate::ICMP_ULE:
      is_true_path = ret.first <= compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_SGE:
    case llvm::CmpInst::Predicate::ICMP_UGE:
      is_true_path = ret.first >= compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_SLT:
    case llvm::CmpInst::Predicate::ICMP_ULT:
      is_true_path = ret.first < compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_SGT:
    case llvm::CmpInst::Predicate::ICMP_UGT:
      is_true_path = ret.first > compared_value;
      break;
    default:
      break;
    }
    generateWarning(call_inst.get(),
                    std::to_string(ret.first) + " Called Func");
    generateWarning(call_inst.get(),
                    is_true_path ? "True Path" : "False Path");

    const BranchInst::TransitionNodes &nodes =
        is_true_path ? branch_inst->TruePathNodes()
                     : branch_inst->FalsePathNodes();
    for (const auto &successor_node : nodes) {
      auto locked = successor_node.lock();
      if (!locked)
        continue;
      auto ret_value = std::make_shared<fitx::ConstValue>(ret.first);
      for (const auto &success_block_ref : ret.second) {
        auto success_block = success_block_ref.lock();
        if (!success_block)
          continue;
        auto success_basic_block_info =
            called_func_info->getBasicBlockInformation(success_block);

        if (!success_block->Instructions().empty())
          generateWarning(success_block->Instructions().front().get(),
                          "Pred Blocks");

        if (!locked->Instructions().empty())
          generateWarning(locked->Instructions().front().get(),
                          "Propagating Block");

        if (!success_basic_block_info)
          continue;

        basic_block_info->setPendingValueStates(
            successor_node, success_basic_block_info->getArgValueStates());
        basic_block_info->setPendingReturnValues(successor_node, ret_value);
      }
    }
  }

  currentFunctionInformation()->addValues(
      called_func_info->GetValueCollection());
  return true;
}

bool Analyzer::analyzeFunctionCall(std::shared_ptr<fitx::CallInst> call_inst) {
  auto function = call_inst->CalledFunction();
  if (!function || call_inst->Arguments().empty())
    return false;

  auto tm = state_manager_.TransitionManager();
  bool changed = false;
  const auto &call_args = call_inst->Arguments();
  for (unsigned int arg = 0; arg < call_args.size(); arg++) {
    const auto *found = tm->findFunctionArgTransitions(function->Name(), arg);
    if (!found || found->second.empty())
      continue;
    changed = true;

    if (found->first.consider_parent) {
      llvm::SmallVector<std::shared_ptr<fitx::Value>, 8> args;
      currentFunctionInformation()->GetValueCollection().collectParentValues(
          call_args[arg], args);
      appendUniqueValue(args, call_args[arg]);
      for (const auto &value : args) {
        changeValueState(found->second, value, call_inst);
      }
    } else {
      changeValueState(found->second, call_args[arg], call_inst);
    }
  }
  return changed;
}

void Analyzer::analyzeReturnValue(std::shared_ptr<fitx::Function> function) {
  auto return_block = function->ReturnBlock();
  auto func_info = getFunctionInformation(function);
  if (!return_block || !func_info)
    return;

  if (!function->ReturnType()->isIntOrPtrTy()) {
    func_info->addReturnValueInfo(FunctionInformation::kSuccessCode,
                                  return_block);
    return;
  }

  std::set<std::weak_ptr<fitx::BasicBlock>> pred_block(
      return_block->Predecessors());

  if (!return_block->Instructions().empty() || pred_block.empty()) {
    pred_block.clear();
    pred_block.insert(return_block);
  }

  for (const auto &pred_reference : pred_block) {
    if (auto pred = pred_reference.lock()) {
      if (!pred->Instructions().empty()) {
        generateWarning(pred->Instructions().front().get(),
                        "Pred Block analysis");
      }

      auto block_info = func_info->getBasicBlockInformation(pred);
      if (!block_info)
        continue;
      if (block_info->ReturnValues().empty()) {
        func_info->addReturnValueInfo(FunctionInformation::kSuccessCode, pred);
        func_info->addReturnValueInfo(FunctionInformation::kErrorCode, pred);
        continue;
      }

      for (const auto &return_value : block_info->ReturnValues()) {
        if (auto const_int =
                fitx::shared_dyn_cast<fitx::ConstValue>(return_value)) {
          auto value = const_int->getConstValue();
          generateWarning(std::to_string(value));
          if (block_info->ReturnValueSatisfiable(value))
            func_info->addReturnValueInfo(value, pred);
        } else if (fitx::shared_isa<fitx::NullValue>(return_value)) {
          func_info->addReturnValueInfo(FunctionInformation::kErrorCode, pred);
        } else if (auto call_inst =
                       fitx::shared_dyn_cast<fitx::CallInst>(return_value)) {
          auto called_function = call_inst->CalledFunction();
          if (called_function) {
            if (called_function->isErrorFunction()) {
              func_info->addReturnValueInfo(FunctionInformation::kErrorCode,
                                            pred);
              continue;
            }

            auto called_func_info = getFunctionInformation(called_function);
            if (!called_func_info)
              continue;

            for (const auto &callee_ret :
                 called_func_info->getReturnValueInfo()) {
              if (block_info->ReturnValueSatisfiable(callee_ret.first))
                func_info->addReturnValueInfo(callee_ret.first, pred);
            }
          }
        } else {
          if (return_value->getValueID() > 0) {
            func_info->addReturnValueInfo(FunctionInformation::kSuccessCode,
                                          pred);
            func_info->addReturnValueInfo(FunctionInformation::kErrorCode,
                                          pred);
          }
        }
      }
    }
  }
}

std::shared_ptr<FunctionInformation>
Analyzer::getFunctionInformation(std::shared_ptr<fitx::Function> function) {
  auto func_info = function_info_.find(function);
  if (func_info != function_info_.end())
    return func_info->second;
  return nullptr;
}

} // namespace fitx
