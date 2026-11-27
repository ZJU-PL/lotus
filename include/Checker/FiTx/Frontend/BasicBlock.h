/// \file BasicBlock.h
/// \brief FiTx per-block analysis state: value typestates, arg summaries,
/// pending (return-code aware) propagation, and may-alias (paper §4.2, 4.3).
#pragma once
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Frontend/State.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace fitx {

/// Lazy per-state transition history for one value at one argument index;
/// stores only states that have actually taken at least one transition in a
/// compact sorted SmallVector (typical size 1-2, zero heap nodes).
class ArgTransitions {
public:
  using Entry = std::pair<fitx::State, TransitionLogs>;
  using Storage = llvm::SmallVector<Entry, 1>;

  ArgTransitions();
  explicit ArgTransitions(const std::set<fitx::State> &states);
  ArgTransitions(const ArgTransitions &arg_transitions);
  ArgTransitions(ArgTransitions &&arg_transitions) noexcept = default;
  ArgTransitions &operator=(const ArgTransitions &) = default;
  ArgTransitions &operator=(ArgTransitions &&) noexcept = default;

  bool operator==(const ArgTransitions &arg_transitions) const;

  bool wouldChangeFrom(const ArgTransitions &arg_transitions) const;
  void addArgTransitions(const ArgTransitions &arg_transitions);
  bool wouldChange(llvm::ArrayRef<Transition> transitions) const;
  bool addTransition(llvm::ArrayRef<Transition> transitions,
                     const std::shared_ptr<fitx::Instruction> &inst);
  void collectReducedTransitions(llvm::SmallVectorImpl<Transition> &out) const;
  TransitionLogs getTransitionLog(State state);
  Storage &TransitionPerState() { return transition_per_state_; }
  const Storage &TransitionPerState() const { return transition_per_state_; }
  bool empty() const { return transition_per_state_.empty(); }

private:
  Storage::iterator findEntry(const fitx::State &state);
  Storage::const_iterator findEntry(const fitx::State &state) const;

  Storage transition_per_state_;
};

/// Per-argument typestate summary: for each arg index, value -> ArgTransitions.
/// Uses per-argument copy-on-write sharing so identical predecessor summaries
/// propagate in O(1) without deep-copying std::map trees.
class ArgValueStates {
public:
  using ArgEntry = std::pair<std::shared_ptr<fitx::Value>, ArgTransitions>;
  using ArgMap = std::vector<ArgEntry>;

  ArgValueStates();
  ArgValueStates(uint64_t arg_num, const std::set<State> &states);
  ArgValueStates(const ArgValueStates &arg_value_states) = default;
  ArgValueStates(ArgValueStates &&arg_value_states) noexcept = default;

  bool operator==(const ArgValueStates &states) const;
  ArgValueStates &operator=(const ArgValueStates &arg_value_states) = default;
  ArgValueStates &operator=(ArgValueStates &&arg_value_states) noexcept = default;

  bool transitionState(llvm::ArrayRef<Transition> transitions,
                       const std::shared_ptr<fitx::Value> &value,
                       const std::shared_ptr<fitx::Instruction> &instruction);

  void addArgValueState(const ArgValueStates &states);
  uint64_t Size() const { return arg_num_; }
  bool empty() const {
    if (value_states_.empty())
      return true;
    for (const auto &arg_map : value_states_) {
      if (arg_map && !arg_map->empty())
        return false;
    }
    return true;
  }
  bool ValueExistsInArg(uint64_t arg, const std::shared_ptr<Value> &value) const;

  std::map<std::shared_ptr<fitx::Value>, std::vector<Transition>>
  getValueStateForArg(uint64_t index) const;

  const ArgMap &getArgTransitions(uint64_t index) const;

  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
  getValueTransitionStates(const fitx::State &state);

  void print();

private:
  void ensureSlots() {
    if (value_states_.empty() && arg_num_ > 0)
      value_states_.resize(arg_num_);
  }

  ArgMap &ensureMutableArg(uint64_t index) {
    ensureSlots();
    auto &slot = value_states_[index];
    if (!slot)
      slot = std::make_shared<ArgMap>();
    else if (slot.use_count() > 1)
      slot = std::make_shared<ArgMap>(*slot);
    return *slot;
  }

  uint64_t arg_num_ = 0;
  std::vector<std::shared_ptr<ArgMap>> value_states_;
};

/// Per-block map: value -> TransitionLogs (current typestate + transition
/// history). Copy-on-write shared across unchanged predecessor->successor edges.
class BasicBlockValueStates {
public:
  using ValueMap = std::map<std::shared_ptr<fitx::Value>, TransitionLogs>;

  BasicBlockValueStates() = default;
  BasicBlockValueStates(const BasicBlockValueStates &states) = default;
  BasicBlockValueStates(BasicBlockValueStates &&states) noexcept = default;
  BasicBlockValueStates &operator=(const BasicBlockValueStates &) = default;
  BasicBlockValueStates &operator=(BasicBlockValueStates &&) noexcept = default;
  bool operator==(const BasicBlockValueStates &states) const;

  bool valueExists(const std::shared_ptr<fitx::Value> &value) const;
  void updateReturnValue(std::shared_ptr<fitx::BasicBlock> block);
  bool transitionState(llvm::ArrayRef<Transition> transitions,
                       const std::shared_ptr<fitx::Value> &value,
                       const std::shared_ptr<fitx::Instruction> &instruction);

  void setValueState(const std::shared_ptr<fitx::Value> &value,
                     const fitx::Transition &states,
                     const std::shared_ptr<fitx::Instruction> &instruction);

  void setValueState(const std::shared_ptr<fitx::Value> &value,
                     const fitx::TransitionLogs &logs);

  void mergeFrom(const BasicBlockValueStates &pred);

  TransitionLogs &getTransitionLog(const std::shared_ptr<fitx::Value> &value);
  const TransitionLogs &
  getTransitionLog(const std::shared_ptr<fitx::Value> &value) const;

  std::vector<std::shared_ptr<fitx::Value>>
  getStateValues(const fitx::State &state);

  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
  getValueTransitionStates(const fitx::State &state);

  const State &getState(const std::shared_ptr<fitx::Value> &value) const {
    return value_states_->at(value).CurrentState();
  }

  const ValueMap &ValueStates() const {
    static const ValueMap kEmpty;
    return value_states_ ? *value_states_ : kEmpty;
  }

  void clear() { value_states_.reset(); }
  void print();

private:
  ValueMap &ensureMutable() {
    if (!value_states_)
      value_states_ = std::make_shared<ValueMap>();
    else if (value_states_.use_count() > 1)
      value_states_ = std::make_shared<ValueMap>(*value_states_);
    return *value_states_;
  }

  std::shared_ptr<ValueMap> value_states_;
};

class BasicBlockInformation {
public:
  constexpr static int kMaxTimeToLive = 5;

  enum BlockStatus { NONE, ERROR, SUCCESS, NUTRAL };

  BasicBlockInformation(std::shared_ptr<fitx::BasicBlock> basic_block,
                        const std::set<State> &states);
  BasicBlockInformation(const BasicBlockInformation &info);

  bool changeValueState(llvm::ArrayRef<Transition> transitions,
                        const std::shared_ptr<fitx::Value> &value,
                        const std::shared_ptr<fitx::Instruction> &instruction);
  bool valueHasState(const std::shared_ptr<fitx::Value> &value) const;
  void removeValueFromState(const std::shared_ptr<fitx::Value> &value,
                            const std::shared_ptr<fitx::Instruction> &instruction);

  void resetValueState(const std::shared_ptr<fitx::Value> &value,
                       const std::shared_ptr<fitx::Instruction> &instruction);

  fitx::BasicBlockValueStates &ValueStates() { return value_states_; }
  const fitx::BasicBlockValueStates &ValueStates() const {
    return value_states_;
  }

  bool hasPendingArgStatesFor(
      const std::shared_ptr<fitx::BasicBlock> &successor) const {
    auto it = pending_values_.find(successor);
    return it != pending_values_.end() && !it->second.arg_states.empty();
  }

  bool hasPendingReturnValuesFor(
      const std::shared_ptr<fitx::BasicBlock> &successor) const {
    auto it = pending_values_.find(successor);
    return it != pending_values_.end() && !it->second.return_values.empty();
  }

  std::pair<fitx::BasicBlockValueStates, fitx::ArgValueStates>
  ValueStatesForSuccessor(std::shared_ptr<fitx::BasicBlock> successor);

  std::set<std::shared_ptr<fitx::Value>>
  ReturnCodeForSuccessor(std::shared_ptr<fitx::BasicBlock> successor);

  fitx::ArgValueStates &getArgValueStates() { return arg_value_states_; }
  const fitx::ArgValueStates &getArgValueStates() const {
    return arg_value_states_;
  }
  void setPendingValueStates(std::weak_ptr<fitx::BasicBlock>,
                             const fitx::ArgValueStates &arg_value_state);

  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
  getValueTransitionStates(const State &state);

  void setPendingReturnValues(std::weak_ptr<fitx::BasicBlock>,
                              std::shared_ptr<fitx::ConstValue>);

  bool operator==(const fitx::BasicBlockInformation &prev_block_info) const;

  void setTimeToLive(int ttl) { time_to_live_ = ttl; }
  int TimeToLive() const { return time_to_live_; }

  const std::set<std::shared_ptr<fitx::Value>> &ReturnValues() const {
    return return_values_;
  }

  void addReturnValues(
      const std::set<std::shared_ptr<fitx::Value>> &return_values);
  void addReturnValue(std::shared_ptr<fitx::Value> value);

  bool ReturnValueSatisfiable(long value);

  void removeReturnvalue(int value);

  std::shared_ptr<fitx::BasicBlock> BasicBlock() { return basic_block_; }

  void setPartialStates(bool partial_states);
  bool PartialStates() const { return is_partial_states_; }

  void setPredecessorPartial(bool partial_states) {
    predecessor_partial_ = partial_states;
  }

  bool PredecessorPartial() const { return predecessor_partial_; }

  bool IsInSamelinePredecessor(std::shared_ptr<fitx::BasicBlock> block);
  void addSameLinePredecessor(std::shared_ptr<fitx::BasicBlock> block);
  void addSameLinePredecessor(
      std::vector<std::weak_ptr<fitx::BasicBlock>> blocks);

  std::vector<std::weak_ptr<fitx::BasicBlock>> SameLinePredecessors() {
    return same_line_predecessors_;
  }

  AliasValues &getAliasValues() { return alias_info_; }

  void readCurrentStates();

  void setBlockStatus(BlockStatus status) {
    if (status_ != NUTRAL)
      status_ = status;
  }
  BlockStatus getBlockStatus() const { return status_; }

  /// Release intra-procedural data while retaining `arg_value_states_` needed
  /// by callers that read this exit block's summary.
  void clearNonSummaryData() {
    value_states_.clear();
    alias_info_.clear();
    pending_values_.clear();
    return_values_.clear();
    same_line_predecessors_.clear();
  }

private:
  std::shared_ptr<fitx::BasicBlock> basic_block_;
  struct PendingValues {
    fitx::ArgValueStates arg_states;
    std::set<std::shared_ptr<fitx::ConstValue>> return_values;
  };

  int time_to_live_ = kMaxTimeToLive;
  bool is_partial_states_;
  bool predecessor_partial_;

  std::vector<std::weak_ptr<fitx::BasicBlock>> same_line_predecessors_;
  std::set<std::shared_ptr<fitx::Value>> return_values_;
  AliasValues alias_info_;

  std::map<std::weak_ptr<fitx::BasicBlock>, struct PendingValues,
           std::owner_less<std::weak_ptr<fitx::BasicBlock>>>
      pending_values_;

  fitx::ArgValueStates arg_value_states_;
  fitx::BasicBlockValueStates value_states_;

  const std::set<State> *states_ = nullptr;
  BlockStatus status_;
};

} // namespace fitx
