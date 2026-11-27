#include "Checker/FiTx/Frontend/BasicBlock.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instructions/BranchInstruction.h"
#include "Checker/FiTx/Core/Value.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace fitx {
BasicBlockInformation::BasicBlockInformation(
    std::shared_ptr<fitx::BasicBlock> basic_block,
    const std::set<State> &states)
    : basic_block_(std::move(basic_block)), is_partial_states_(false),
      predecessor_partial_(false), arg_value_states_(),
      states_(&states), status_(NONE) {
  uint64_t arg_size = 0;
  if (auto function = basic_block_->Parent().lock()) {
    arg_size = function->ArgSize();

    const auto &return_assignment = function->getReturnAssignments();
    auto it = return_assignment.find(basic_block_);
    if (it != return_assignment.end()) {
      return_values_.insert(it->second);
    }
  }
  arg_value_states_ = ArgValueStates(arg_size, states);
}

BasicBlockInformation::BasicBlockInformation(const BasicBlockInformation &info)
    : basic_block_(info.basic_block_), time_to_live_(info.time_to_live_),
      is_partial_states_(info.is_partial_states_),
      predecessor_partial_(info.predecessor_partial_),
      same_line_predecessors_(info.same_line_predecessors_),
      return_values_(info.return_values_), alias_info_(info.alias_info_),
      pending_values_(info.pending_values_),
      arg_value_states_(info.arg_value_states_),
      value_states_(info.value_states_), states_(info.states_),
      status_(info.status_) {}

bool BasicBlockInformation::changeValueState(
    llvm::ArrayRef<Transition> transitions,
    const std::shared_ptr<fitx::Value> &value,
    const std::shared_ptr<fitx::Instruction> &instruction) {
  if (!value || transitions.empty())
    return false;

  if (fitx::shared_isa<Argument>(value)) {
    bool pending_changed = false;
    if (!pending_values_.empty()) {
      for (auto &pending_values : pending_values_) {
        pending_changed |= pending_values.second.arg_states.transitionState(
            transitions, value, instruction);
      }
    }
    return pending_changed || arg_value_states_.transitionState(
                                  transitions, value, instruction);
  }
  return value_states_.transitionState(transitions, value, instruction);
}

bool BasicBlockInformation::valueHasState(
    const std::shared_ptr<Value> &value) const {
  return value_states_.valueExists(value);
}

void BasicBlockInformation::removeValueFromState(
    const std::shared_ptr<Value> &value,
    const std::shared_ptr<fitx::Instruction> &instruction) {
  if (!value_states_.valueExists(value))
    return;

  auto null_transition =
      Transition(value_states_.getState(value), NullState::GetInstance());
  value_states_.setValueState(value, null_transition, instruction);
}

void BasicBlockInformation::resetValueState(
    const std::shared_ptr<Value> &value,
    const std::shared_ptr<fitx::Instruction> &instruction) {
  if (!value_states_.valueExists(value))
    return;
  Transition log = value_states_.getTransitionLog(value).ReducedTransition();
  Transition back_init = Transition(log.Target(), log.Source());

  value_states_.setValueState(value, back_init, instruction);
}

void BasicBlockInformation::setPendingValueStates(
    std::weak_ptr<fitx::BasicBlock> basic_block,
    const fitx::ArgValueStates &arg_value_state) {
  if (arg_value_state.empty())
    return;
  auto it = pending_values_.find(basic_block);
  if (it == pending_values_.end()) {
    static const std::set<State> kEmptyStates;
    it = pending_values_
             .emplace(basic_block,
                      PendingValues{
                          ArgValueStates(arg_value_state.Size(),
                                         states_ ? *states_ : kEmptyStates),
                          {}})
             .first;
  }
  it->second.arg_states.addArgValueState(arg_value_state);
}

std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
BasicBlockInformation::getValueTransitionStates(const State &state) {
  auto value_transition_states = value_states_.getValueTransitionStates(state);
  if (state.EarlyNotification()) {
    auto arg_transition_states =
        arg_value_states_.getValueTransitionStates(state);

    value_transition_states.insert(value_transition_states.end(),
                                   arg_transition_states.begin(),
                                   arg_transition_states.end());
  }
  return value_transition_states;
}

void BasicBlockInformation::setPendingReturnValues(
    std::weak_ptr<fitx::BasicBlock> basic_block,
    std::shared_ptr<fitx::ConstValue> arg_value_state) {
  pending_values_[basic_block].return_values.insert(std::move(arg_value_state));
}

std::pair<fitx::BasicBlockValueStates, fitx::ArgValueStates>
BasicBlockInformation::ValueStatesForSuccessor(
    std::shared_ptr<fitx::BasicBlock> successor) {
  std::pair<fitx::BasicBlockValueStates, fitx::ArgValueStates> states(
      value_states_, arg_value_states_);

  auto pend_it = pending_values_.find(successor);
  if (pend_it == pending_values_.end()) {
    return states;
  }

  auto branch_inst = basic_block_->getBranchInst();
  if (!branch_inst || !branch_inst->Condition())
    return states;

  std::shared_ptr<fitx::CallInst> call_inst =
      fitx::shared_dyn_cast<fitx::CallInst>(branch_inst->Condition());

  if (auto compare_inst =
          fitx::shared_dyn_cast<fitx::CompareInst>(branch_inst->Condition())) {
    auto operand = std::find_if(
        compare_inst->Operands().begin(), compare_inst->Operands().end(),
        [](const auto &op) { return fitx::shared_isa<CallInst>(op); });
    if (operand == compare_inst->Operands().end())
      return states;
    call_inst = fitx::shared_dyn_cast<CallInst>(*operand);
  }

  if (!call_inst)
    return states;

  const auto &operands = call_inst->Arguments();
  for (size_t i = 0; i < operands.size(); i++) {
    const auto &arg_transitions =
        pend_it->second.arg_states.getArgTransitions(i);
    if (arg_transitions.empty())
      continue;
    const auto &operand = operands[i];
    for (const auto &value : arg_transitions) {
      llvm::SmallVector<Transition, 4> reduced;
      value.second.collectReducedTransitions(reduced);
      if (reduced.empty())
        continue;
      auto new_value = Value::CreateAppend(operand, value.first);
      if (!fitx::shared_isa<Argument>(new_value))
        states.first.transitionState(reduced, new_value, call_inst);
      else
        states.second.transitionState(reduced, new_value, call_inst);
    }
  }

  return states;
}

std::set<std::shared_ptr<fitx::Value>>
BasicBlockInformation::ReturnCodeForSuccessor(
    std::shared_ptr<fitx::BasicBlock> successor) {
  std::set<std::shared_ptr<fitx::Value>> return_values = return_values_;
  auto branch_inst = basic_block_->getBranchInst();
  if (!branch_inst || !branch_inst->Condition())
    return return_values;

  auto compare_inst =
      fitx::shared_dyn_cast<fitx::CompareInst>(branch_inst->Condition());
  if (!compare_inst)
    return return_values;
  auto pend_it = pending_values_.find(successor);
  if (pend_it != pending_values_.end()) {
    return_values.insert(pend_it->second.return_values.begin(),
                         pend_it->second.return_values.end());

    auto operand = std::find_if(
        compare_inst->Operands().begin(), compare_inst->Operands().end(),
        [](const auto &op) { return fitx::shared_isa<CallInst>(op); });

    if (operand != compare_inst->Operands().end())
      return_values.erase(*operand);
  }

  return return_values;
}

bool BasicBlockValueStates::operator==(
    const BasicBlockValueStates &states) const {
  if (value_states_ == states.value_states_)
    return true;
  const bool lhs_empty = !value_states_ || value_states_->empty();
  const bool rhs_empty = !states.value_states_ || states.value_states_->empty();
  if (lhs_empty && rhs_empty)
    return true;
  if (lhs_empty || rhs_empty)
    return false;
  return *value_states_ == *states.value_states_;
}

bool BasicBlockValueStates::valueExists(
    const std::shared_ptr<fitx::Value> &value) const {
  return value_states_ && value_states_->find(value) != value_states_->end();
}

void BasicBlockValueStates::setValueState(
    const std::shared_ptr<fitx::Value> &value, const fitx::Transition &transition,
    const std::shared_ptr<fitx::Instruction> &instruction) {
  ensureMutable()[value].addTransition(transition, instruction);
}

void BasicBlockValueStates::setValueState(
    const std::shared_ptr<fitx::Value> &value, const fitx::TransitionLogs &logs) {
  ensureMutable()[value] = logs;
}

void BasicBlockValueStates::mergeFrom(const BasicBlockValueStates &pred) {
  if (!pred.value_states_ || pred.value_states_->empty())
    return;
  if (value_states_ == pred.value_states_)
    return;
  if (!value_states_ || value_states_->empty()) {
    value_states_ = pred.value_states_;
    return;
  }

  // Read-only subsumption check: preserve CoW sharing when `pred` adds no new
  // keys and improves no existing CurrentState.
  bool any_change = false;
  for (const auto &val_entry : *pred.value_states_) {
    auto cur_it = value_states_->find(val_entry.first);
    if (cur_it == value_states_->end() ||
        val_entry.second.CurrentState() < cur_it->second.CurrentState()) {
      any_change = true;
      break;
    }
  }
  if (!any_change)
    return;

  ValueMap &dest = ensureMutable();
  for (const auto &val_entry : *pred.value_states_) {
    auto res = dest.try_emplace(val_entry.first, val_entry.second);
    if (!res.second &&
        val_entry.second.CurrentState() < res.first->second.CurrentState()) {
      res.first->second = val_entry.second;
    }
  }
}

TransitionLogs &BasicBlockValueStates::getTransitionLog(
    const std::shared_ptr<fitx::Value> &value) {
  return ensureMutable()[value];
}

const TransitionLogs &BasicBlockValueStates::getTransitionLog(
    const std::shared_ptr<fitx::Value> &value) const {
  static const ValueMap kEmpty;
  return (value_states_ ? *value_states_ : kEmpty).at(value);
}

std::vector<std::shared_ptr<fitx::Value>>
BasicBlockValueStates::getStateValues(const fitx::State &state) {
  std::vector<std::shared_ptr<fitx::Value>> values;
  if (!value_states_)
    return values;
  for (const auto &entry : *value_states_) {
    if (entry.second.CurrentState() == state)
      values.push_back(entry.first);
  }
  return values;
}

std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
BasicBlockValueStates::getValueTransitionStates(const fitx::State &state) {
  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>> values;
  if (!value_states_ || value_states_->empty())
    return values;
  bool any_match = false;
  for (const auto &entry : *value_states_) {
    if (entry.second.CurrentState() == state) {
      any_match = true;
      break;
    }
  }
  if (!any_match)
    return values;

  ValueMap &dest = ensureMutable();
  for (auto &entry : dest) {
    if (entry.second.CurrentState() == state)
      values.emplace_back(entry.first, &entry.second);
  }
  return values;
}

bool BasicBlockValueStates::transitionState(
    llvm::ArrayRef<Transition> transitions,
    const std::shared_ptr<fitx::Value> &value,
    const std::shared_ptr<fitx::Instruction> &instruction) {
  generateWarning(instruction.get(), "Transition State Called");
  if (value_states_) {
    auto existing = value_states_->find(value);
    if (existing != value_states_->end()) {
      generateWarning(instruction.get(), "value exists");
      const TransitionLogs &current_transitions = existing->second;
      const State &current_state = current_transitions.CurrentState();

      auto next_transition =
          std::find_if(transitions.begin(), transitions.end(),
                       [&current_state](const Transition &transition) {
                         return transition.Source() == current_state;
                       });

      if (next_transition != transitions.end() &&
          current_transitions.CurrentInstruction() != instruction &&
          *current_transitions.CurrentInstruction() <= *instruction) {
        ValueMap &dest = ensureMutable();
        dest.find(value)->second.addTransition(*next_transition, instruction);
        return true;
      }
      return false;
    }
  }

  const fitx::Transition *transition = nullptr;
  for (const auto &cand : transitions) {
    if (cand.Source().isInitState() &&
        (!transition || cand.Target() < transition->Target())) {
      transition = &cand;
    }
  }

  if (transition) {
    ensureMutable()[value].addTransition(*transition, instruction);
    return true;
  }
  return false;
}

void BasicBlockValueStates::print() {
  if (!value_states_)
    return;
  for (const auto &states : *value_states_) {
    llvm::errs() << states.first << " " << states.second.ReducedTransition()
                 << "\n";
  }
}

/* ArgTransitions Class (lazy sorted SmallVector: only non-dummy states) */
ArgTransitions::ArgTransitions() : transition_per_state_() {}

ArgTransitions::ArgTransitions(const std::set<fitx::State> &states) {
  (void)states;
}

ArgTransitions::ArgTransitions(const ArgTransitions &arg_transitions)
    : transition_per_state_(arg_transitions.transition_per_state_) {}

ArgTransitions::Storage::iterator
ArgTransitions::findEntry(const fitx::State &state) {
  for (auto it = transition_per_state_.begin();
       it != transition_per_state_.end(); ++it) {
    if (it->first == state)
      return it;
    if (state < it->first)
      return transition_per_state_.end();
  }
  return transition_per_state_.end();
}

ArgTransitions::Storage::const_iterator
ArgTransitions::findEntry(const fitx::State &state) const {
  for (auto it = transition_per_state_.begin();
       it != transition_per_state_.end(); ++it) {
    if (it->first == state)
      return it;
    if (state < it->first)
      return transition_per_state_.end();
  }
  return transition_per_state_.end();
}

bool ArgTransitions::operator==(const ArgTransitions &arg_transitions) const {
  return transition_per_state_ == arg_transitions.transition_per_state_;
}

bool ArgTransitions::wouldChangeFrom(
    const ArgTransitions &arg_transitions) const {
  if (arg_transitions.transition_per_state_.empty())
    return false;
  if (transition_per_state_.empty()) {
    for (const auto &state : arg_transitions.transition_per_state_) {
      if (!state.second.isDummy())
        return true;
    }
    return false;
  }
  auto it_a = transition_per_state_.begin();
  auto end_a = transition_per_state_.end();
  for (const auto &state : arg_transitions.transition_per_state_) {
    if (state.second.isDummy())
      continue;
    while (it_a != end_a && it_a->first < state.first)
      ++it_a;
    if (it_a == end_a || it_a->first != state.first)
      return true;
    if (it_a->second.isDummy() ||
        state.second.CurrentState() < it_a->second.CurrentState())
      return true;
  }
  return false;
}

void ArgTransitions::addArgTransitions(const ArgTransitions &arg_transitions) {
  if (arg_transitions.transition_per_state_.empty())
    return;
  if (transition_per_state_.empty()) {
    transition_per_state_ = arg_transitions.transition_per_state_;
    return;
  }
  for (const auto &state : arg_transitions.transition_per_state_) {
    if (state.second.isDummy())
      continue;

    auto pos = std::lower_bound(
        transition_per_state_.begin(), transition_per_state_.end(), state.first,
        [](const Entry &e, const fitx::State &s) { return e.first < s; });
    if (pos == transition_per_state_.end() || pos->first != state.first) {
      transition_per_state_.insert(pos, state);
    } else if (pos->second.isDummy() ||
               state.second.CurrentState() < pos->second.CurrentState()) {
      pos->second = state.second;
    }
  }
}

bool ArgTransitions::addTransition(
    llvm::ArrayRef<Transition> transitions,
    const std::shared_ptr<fitx::Instruction> &inst) {
  if (transitions.empty())
    return false;

  bool changed = false;

  // 1. Collect transitions starting from states that do not yet have an entry
  //    in `transition_per_state_` (mathematically equivalent to the dummy
  //    entries in an eagerly populated map, selecting the first matching rule
  //    per source state).
  llvm::SmallVector<const Transition *, 4> to_insert;
  for (const auto &t : transitions) {
    if (findEntry(t.Source()) != transition_per_state_.end())
      continue;
    bool already_queued = false;
    for (const auto *queued : to_insert) {
      if (queued->Source() == t.Source()) {
        already_queued = true;
        break;
      }
    }
    if (!already_queued)
      to_insert.push_back(&t);
  }

  // 2. Advance already-materialized state entries first so newly inserted
  //    entries are not advanced twice on the same instruction.
  for (auto &entry : transition_per_state_) {
    const State &current_state =
        entry.second.isDummy() ? entry.first : entry.second.CurrentState();
    auto next_transition =
        std::find_if(transitions.begin(), transitions.end(),
                     [&current_state](const Transition &transition) {
                       return transition.Source() == current_state;
                     });
    if (next_transition != transitions.end()) {
      entry.second.addTransition(*next_transition, inst);
      changed = true;
    }
  }

  // 3. Materialize newly triggered source states in sorted order.
  for (const auto *t : to_insert) {
    auto pos = std::lower_bound(
        transition_per_state_.begin(), transition_per_state_.end(), t->Source(),
        [](const Entry &e, const fitx::State &s) { return e.first < s; });
    auto inserted =
        transition_per_state_.insert(pos, Entry(t->Source(), TransitionLogs()));
    inserted->second.addTransition(*t, inst);
    changed = true;
  }

  return changed;
}

bool ArgTransitions::wouldChange(
    llvm::ArrayRef<Transition> transitions) const {
  if (transitions.empty())
    return false;
  for (const auto &t : transitions) {
    if (findEntry(t.Source()) == transition_per_state_.end())
      return true;
  }
  for (const auto &entry : transition_per_state_) {
    const State &current_state =
        entry.second.isDummy() ? entry.first : entry.second.CurrentState();
    for (const auto &t : transitions) {
      if (t.Source() == current_state)
        return true;
    }
  }
  return false;
}

void ArgTransitions::collectReducedTransitions(
    llvm::SmallVectorImpl<Transition> &out) const {
  for (const auto &transition_log : transition_per_state_) {
    if (!transition_log.second.isDummy()) {
      Transition reduced = transition_log.second.ReducedTransition();
      if (reduced.Source() != reduced.Target())
        out.push_back(std::move(reduced));
    }
  }
}

TransitionLogs ArgTransitions::getTransitionLog(State state) {
  auto it = findEntry(state);
  if (it != transition_per_state_.end())
    return it->second;
  return TransitionLogs();
}

// ArgValueStates Class
ArgValueStates::ArgValueStates() : arg_num_(0) {}

ArgValueStates::ArgValueStates(uint64_t arg_num, const std::set<State> &states)
    : arg_num_(arg_num) {
  (void)states;
}

std::map<std::shared_ptr<fitx::Value>, std::vector<Transition>>
ArgValueStates::getValueStateForArg(uint64_t index) const {
  std::map<std::shared_ptr<fitx::Value>, std::vector<Transition>> new_map;

  if (value_states_.size() <= index || !value_states_[index])
    return new_map;
  for (const auto &value : *value_states_[index]) {
    for (const auto &transition_log : value.second.TransitionPerState()) {
      if (!transition_log.second.isDummy()) {
        Transition reduced = transition_log.second.ReducedTransition();
        if (reduced.Source() != reduced.Target()) {
          new_map[value.first].push_back(std::move(reduced));
        }
      }
    }
  }

  return new_map;
}

const ArgValueStates::ArgMap &
ArgValueStates::getArgTransitions(uint64_t index) const {
  static const ArgMap kEmpty;
  if (value_states_.size() <= index || !value_states_[index])
    return kEmpty;
  return *value_states_[index];
}

std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
ArgValueStates::getValueTransitionStates(const fitx::State &state) {
  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>> values;
  for (size_t arg_idx = 0; arg_idx < value_states_.size(); ++arg_idx) {
    const auto &arg_map = value_states_[arg_idx];
    if (!arg_map || arg_map->empty())
      continue;
    bool any_match = false;
    for (const auto &value : *arg_map) {
      for (const auto &transition_logs : value.second.TransitionPerState()) {
        if (transition_logs.second.isDummy())
          continue;
        if (transition_logs.second.LeastSignificantSource().isInitState() &&
            transition_logs.second.MostSignificantTarget() == state) {
          any_match = true;
          break;
        }
      }
      if (any_match)
        break;
    }
    if (!any_match)
      continue;

    ArgMap &mutable_map = ensureMutableArg(arg_idx);
    for (auto &value : mutable_map) {
      for (auto &transition_logs : value.second.TransitionPerState()) {
        if (transition_logs.second.isDummy())
          continue;
        if (transition_logs.second.LeastSignificantSource().isInitState() &&
            transition_logs.second.MostSignificantTarget() == state) {
          values.emplace_back(value.first, &transition_logs.second);
        }
      }
    }
  }
  return values;
}

namespace {
ArgValueStates::ArgMap::const_iterator
findArgEntry(const ArgValueStates::ArgMap &map,
             const std::shared_ptr<fitx::Value> &key) {
  const fitx::Value *raw = key.get();
  auto it = std::lower_bound(map.begin(), map.end(), raw,
                             [](const ArgValueStates::ArgEntry &entry,
                                const fitx::Value *k) {
                               return entry.first.get() < k;
                             });
  if (it != map.end() && it->first.get() == raw)
    return it;
  return map.end();
}

bool isArgMapSubsumed(const ArgValueStates::ArgMap &dest,
                      const ArgValueStates::ArgMap &incoming) {
  size_t i = 0;
  size_t j = 0;
  const size_t n = dest.size();
  const size_t m = incoming.size();
  while (i < n && j < m) {
    const fitx::Value *kd = dest[i].first.get();
    const fitx::Value *ki = incoming[j].first.get();
    if (kd < ki) {
      ++i;
    } else if (ki < kd) {
      if (!incoming[j].second.empty())
        return false;
      ++j;
    } else {
      if (dest[i].second.wouldChangeFrom(incoming[j].second))
        return false;
      ++i;
      ++j;
    }
  }
  while (j < m) {
    if (!incoming[j].second.empty())
      return false;
    ++j;
  }
  return true;
}

ArgValueStates::ArgMap mergeArgMapsTwoPointer(
    const ArgValueStates::ArgMap &dest,
    const ArgValueStates::ArgMap &incoming) {
  ArgValueStates::ArgMap merged;
  merged.reserve(dest.size() + incoming.size());
  size_t i = 0;
  size_t j = 0;
  const size_t n = dest.size();
  const size_t m = incoming.size();
  while (i < n && j < m) {
    const fitx::Value *kd = dest[i].first.get();
    const fitx::Value *ki = incoming[j].first.get();
    if (kd < ki) {
      merged.push_back(dest[i++]);
    } else if (ki < kd) {
      if (!incoming[j].second.empty())
        merged.push_back(incoming[j]);
      ++j;
    } else {
      merged.push_back(dest[i]);
      if (!incoming[j].second.empty())
        merged.back().second.addArgTransitions(incoming[j].second);
      ++i;
      ++j;
    }
  }
  while (i < n)
    merged.push_back(dest[i++]);
  while (j < m) {
    if (!incoming[j].second.empty())
      merged.push_back(incoming[j]);
    ++j;
  }
  return merged;
}
} // namespace

bool ArgValueStates::ValueExistsInArg(
    uint64_t arg, const std::shared_ptr<Value> &value) const {
  if (value_states_.size() <= arg || !value_states_[arg])
    return false;
  return findArgEntry(*value_states_[arg], value) != value_states_[arg]->end();
}

void ArgValueStates::addArgValueState(const ArgValueStates &states) {
  if (states.empty())
    return;
  if (value_states_.empty() && arg_num_ == states.arg_num_) {
    value_states_ = states.value_states_;
    return;
  }
  const size_t limit = std::min(static_cast<size_t>(arg_num_),
                                states.value_states_.size());
  if (limit == 0)
    return;
  ensureSlots();
  for (size_t arg_idx = 0; arg_idx < limit; arg_idx++) {
    const auto &incoming_ptr = states.value_states_[arg_idx];
    if (!incoming_ptr || incoming_ptr->empty())
      continue;
    auto &dest_ptr = value_states_[arg_idx];
    if (dest_ptr == incoming_ptr)
      continue;
    if (!dest_ptr || dest_ptr->empty()) {
      dest_ptr = incoming_ptr;
      continue;
    }
    if (isArgMapSubsumed(*dest_ptr, *incoming_ptr))
      continue;

    ArgMap merged = mergeArgMapsTwoPointer(*dest_ptr, *incoming_ptr);
    if (dest_ptr.use_count() == 1)
      *dest_ptr = std::move(merged);
    else
      dest_ptr = std::make_shared<ArgMap>(std::move(merged));
  }
}

bool ArgValueStates::transitionState(
    llvm::ArrayRef<Transition> transitions,
    const std::shared_ptr<fitx::Value> &value,
    const std::shared_ptr<fitx::Instruction> &instruction) {
  if (auto argument = fitx::shared_dyn_cast<fitx::Argument>(value)) {
    uint64_t arg_index = argument->ArgNum();
    if (arg_num_ <= arg_index || transitions.empty())
      return false;

    if (arg_index < value_states_.size() && value_states_[arg_index]) {
      const auto &ro_map = *value_states_[arg_index];
      auto ro_it = findArgEntry(ro_map, value);
      if (ro_it != ro_map.end() && !ro_it->second.wouldChange(transitions))
        return false;
    }

    ArgMap &arg_map = ensureMutableArg(arg_index);
    const fitx::Value *raw = value.get();
    auto pos = std::lower_bound(arg_map.begin(), arg_map.end(), raw,
                                [](const ArgEntry &entry, const fitx::Value *k) {
                                  return entry.first.get() < k;
                                });
    if (pos == arg_map.end() || pos->first.get() != raw) {
      ArgTransitions tmp;
      if (!tmp.addTransition(transitions, instruction))
        return false;
      arg_map.emplace(pos, value, std::move(tmp));
      return true;
    }
    return pos->second.addTransition(transitions, instruction);
  }
  return false;
}

void ArgValueStates::print() {
  for (const auto &arg : value_states_) {
    if (!arg)
      continue;
    for (const auto &value : *arg) {
      llvm::errs() << value.first << "\n";
      for (const auto &states : value.second.TransitionPerState()) {
        if (!states.second.isDummy())
          llvm::errs() << states.second.ReducedTransition() << "\n";
      }
    }
  }
}

bool ArgValueStates::operator==(const ArgValueStates &states) const {
  if (arg_num_ != states.arg_num_)
    return false;
  if (value_states_.empty() && states.value_states_.empty())
    return true;
  if (value_states_.empty())
    return states.empty();
  if (states.value_states_.empty())
    return empty();
  for (size_t i = 0; i < value_states_.size(); ++i) {
    const auto &lhs = value_states_[i];
    const auto &rhs = states.value_states_[i];
    if (lhs == rhs)
      continue;
    const bool lhs_empty = !lhs || lhs->empty();
    const bool rhs_empty = !rhs || rhs->empty();
    if (lhs_empty && rhs_empty)
      continue;
    if (lhs_empty || rhs_empty)
      return false;
    if (*lhs != *rhs)
      return false;
  }
  return true;
}

bool BasicBlockInformation::operator==(
    const fitx::BasicBlockInformation &prev_block_info) const {
  return value_states_ == prev_block_info.value_states_ &&
         arg_value_states_ == prev_block_info.arg_value_states_ &&
         return_values_ == prev_block_info.return_values_;
}

void BasicBlockInformation::addReturnValues(
    const std::set<std::shared_ptr<fitx::Value>> &return_values) {
  return_values_.insert(return_values.begin(), return_values.end());
}

void BasicBlockInformation::addReturnValue(std::shared_ptr<fitx::Value> value) {
  return_values_.insert(std::move(value));
}

bool BasicBlockInformation::ReturnValueSatisfiable(long value) {
  switch (getBlockStatus()) {
  case BasicBlockInformation::ERROR:
    if (!value)
      return false;
    break;
  case BasicBlockInformation::SUCCESS:
    if (value)
      return false;
    break;
  default:
    break;
  }
  return true;
}

void BasicBlockInformation::removeReturnvalue(int value) {
  auto found_value = std::find_if(
      return_values_.begin(), return_values_.end(), [value](auto ret_val) {
        if (auto const_ret = fitx::shared_dyn_cast<ConstValue>(ret_val))
          return value == const_ret->getConstValue();
        return false;
      });
  if (found_value != return_values_.end())
    return_values_.erase(found_value);
}

void BasicBlockInformation::setPartialStates(bool partial_states) {
  is_partial_states_ = partial_states;
}

bool BasicBlockInformation::IsInSamelinePredecessor(
    std::shared_ptr<fitx::BasicBlock> block) {
  return std::find_if(same_line_predecessors_.begin(),
                      same_line_predecessors_.end(), [&block](auto predecessor) {
                        return block == predecessor.lock();
                      }) != same_line_predecessors_.end();
}

void BasicBlockInformation::addSameLinePredecessor(
    std::shared_ptr<fitx::BasicBlock> block) {
  same_line_predecessors_.push_back(std::move(block));
}

void BasicBlockInformation::addSameLinePredecessor(
    std::vector<std::weak_ptr<fitx::BasicBlock>> blocks) {
  same_line_predecessors_.insert(same_line_predecessors_.end(), blocks.begin(),
                                 blocks.end());
}

void BasicBlockInformation::readCurrentStates() {
  generateWarning(basic_block_.get(), "--- Local States ---");
  value_states_.print();

  generateWarning(basic_block_.get(), "--- Arg States ---");
  arg_value_states_.print();
}

} // namespace fitx
