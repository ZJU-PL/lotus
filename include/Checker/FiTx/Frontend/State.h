/// \file State.h
/// Typestate FSM for FiTx: states, transitions, and bug-checking hooks.
/// Paper: Suzuki et al., USENIX ATC 2024, Section 4.1 (Typestate Property),
/// Table 6 (bug-checking hooks: IMMEDIATE, VAR_END, BLOCK_END, FUNC_END,
/// MOD_END).

#pragma once
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Frontend/PropagationConstraint.h"
#include "Checker/FiTx/Frontend/StateTransition.h"

#include <algorithm>
#include <climits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fitx {

enum StateType { INIT, NORMAL, BUG };
enum StateMergeMethod { STRICT, FLEX, CUSTOM };
enum BugNotificationTiming { IMMEDIATE, FUNCTION_END, END_OF_LIFE, MODULE_END };
enum TriggerConstraint { NONE = 0, NON_RETURN = 1, NON_ARG = 2 };

struct StateArgs {
  StateArgs(std::string name, StateType type = NORMAL,
            BugNotificationTiming timing = IMMEDIATE,
            bool early_notification = true, TriggerConstraint constraint = NONE,
            StateMergeMethod method = STRICT)
      : name_(std::move(name)), type_(type), method_(method), timing_(timing),
        early_notification_(early_notification),
        trigger_constraint_(constraint) {}
  std::string name_;
  StateType type_;
  StateMergeMethod method_;
  BugNotificationTiming timing_;
  bool early_notification_;
  TriggerConstraint trigger_constraint_;
};

class State {
public:
  constexpr static int kStateMaxNum = 100;
  constexpr static int kBugStateIDBase = INT_MAX - kStateMaxNum;

  State() = default;
  State(int ID, std::string name, StateType type = NORMAL,
        StateMergeMethod method = STRICT, bool early_notification = true,
        TriggerConstraint = NONE, BugNotificationTiming timing = IMMEDIATE);
  State(const State &state) = default;
  State(State &&state) noexcept = default;
  State(int ID, StateArgs &args);

  bool operator<(const State &state) const { return ID_ < state.ID_; }
  bool operator<(const StateArgs &args) const;
  bool operator==(const State &state) const { return ID_ == state.ID_; }
  bool operator!=(const State &state) const { return ID_ != state.ID_; }
  bool operator==(const std::string &name) const { return name == name_; }
  bool operator==(const StateArgs &args) const;
  State &operator=(const State &state) = default;
  State &operator=(State &&state) noexcept = default;

  const std::string &Name() const { return name_; }

  bool isInitState() const { return type_ == StateType::INIT; }
  bool isBugState() const { return type_ == StateType::BUG; }
  bool EarlyNotification() const { return early_notification_; }

  TriggerConstraint getTriggerConstraint() const { return trigger_constraint_; }
  bool isTriggerConstraintSet(TriggerConstraint constraint);

  fitx::StateMergeMethod MergeMethod() const { return method_; }
  fitx::BugNotificationTiming NotificationTiming() const { return timing_; }

  void addTransition(State &state, TransitionRule &rule);

private:
  int ID_ = 0;
  std::string name_;
  StateType type_ = NORMAL;
  StateMergeMethod method_ = STRICT;
  BugNotificationTiming timing_ = IMMEDIATE;
  TriggerConstraint trigger_constraint_ = NONE;
  bool early_notification_ = true;
};

class NullState : public State {
public:
  static NullState &GetInstance() {
    static NullState *state = new NullState();
    return *state;
  }

private:
  NullState() : State(INT_MIN, "Null") {}
};

class Transition {
public:
  Transition(const State &source, const State &target);
  Transition(const Transition &transition);

  void addTransitionRule(std::weak_ptr<TransitionRule> arg);
  const State &Source() const { return source_; }
  const State &Target() const { return target_; }

  Transition &operator=(const Transition &transition);
  bool operator<(const Transition &transition) const;
  bool operator==(const Transition &transition) const;

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                                       const fitx::Transition &transition);

private:
  State source_;
  State target_;
};

class TransitionLogs {
public:
  struct TraceEntry {
    fitx::Transition transition;
    std::shared_ptr<fitx::Instruction> instruction;
  };

  TransitionLogs();
  TransitionLogs(const TransitionLogs &logs) = default;
  TransitionLogs(TransitionLogs &&logs) noexcept = default;
  TransitionLogs(Transition transition,
                 std::shared_ptr<fitx::Instruction> instruction);

  const State &CurrentState() const;
  const std::shared_ptr<fitx::Instruction> &CurrentInstruction() const {
    return transition_logs_.back().instruction;
  }

  void addTransition(const fitx::Transition &transition,
                     std::shared_ptr<fitx::Instruction> instruction);

  void setWarned();

  bool operator==(const TransitionLogs &logs) const;
  TransitionLogs &operator=(const TransitionLogs &logs) = default;
  TransitionLogs &operator=(TransitionLogs &&logs) noexcept = default;

  void generateLog(llvm::raw_ostream &stream) const;
  void logicalTerminate(std::shared_ptr<fitx::Instruction> instruction);
  std::vector<TraceEntry> getTraceEntries() const;

  const State &LeastSignificantSource() const {
    return *least_significant_source_;
  }
  const State &MostSignificantTarget() const {
    return *most_significant_target_;
  }
  Transition ReducedTransition() const;

  bool isDummy() const { return transition_logs_.empty(); }

private:
  struct Log {
    fitx::Transition transition;
    std::shared_ptr<fitx::Instruction> instruction;

    bool operator==(const fitx::TransitionLogs::Log &log) const {
      return transition == log.transition && instruction == log.instruction;
    }
  };

  llvm::SmallVector<Log, 1> transition_logs_;
  std::optional<State> least_significant_source_;
  std::optional<State> most_significant_target_;
  bool warned_ = false;
};

class StateTransitionManager {
public:
  StateTransitionManager() = default;

  Transition createTransition(State &source, State &target);
  Transition createTransition(State &source, State &target,
                              TransitionRule rule);

  void addTransitionRule(Transition &transition, TransitionRule &rule);

  bool existsInFunctionArgTransition(
      const FunctionArgTransitionRule::FunctionArg &arg) const;
  const std::pair<const FunctionArgTransitionRule::FunctionArg,
                  std::vector<fitx::Transition>> *
  findFunctionArgTransitions(const std::string &name, unsigned int arg) const;
  const std::pair<FunctionArgTransitionRule::FunctionArg,
                  std::vector<fitx::Transition>>
  getFunctionArgTransitions(const std::string &name, unsigned int arg);

  const std::vector<fitx::Transition> &getStoreArgTransitions(
      fitx::StoreValueTransitionRule::StoreValueType type,
      const std::string &name = std::string()) const;

  const std::vector<fitx::Transition> &getUseValueTransitions() const;
  const std::vector<fitx::Transition> &getAliasTransitions() const;

  /// Returns false if it is mathematically impossible for any value in M to
  /// leave the init state under this FSM (e.g., all init transitions require
  /// calling functions that are not declared anywhere in M).
  bool canTriggerFromInit(const llvm::Module &M) const;

private:
  void registerFunctionArgTransition(
      const FunctionArgTransitionRule::FunctionArg &arg,
      const fitx::Transition &transition);
  void registerFunctionArgTransition(
      const std::vector<FunctionArgTransitionRule::FunctionArg> &arg,
      const fitx::Transition &transition);

  void registerStoreTransition(std::shared_ptr<StoreValueTransitionRule> rule,
                               const fitx::Transition &transition);

  void registerUseTransition(std::shared_ptr<UseValueTransitionRule> rule,
                             const fitx::Transition &transition);

  void registerAliasTransition(std::shared_ptr<AliasValueTransitionRule> rule,
                               const fitx::Transition &transition);

  std::map<FunctionArgTransitionRule::FunctionArg,
           std::vector<fitx::Transition>>
      function_transitions_;

  std::map<std::string, std::vector<fitx::Transition>> call_store_transitions_;

  std::map<fitx::StoreValueTransitionRule::StoreValueType,
           std::vector<fitx::Transition>>
      store_transitions_;

  std::vector<fitx::Transition> use_transitions_;
  std::vector<fitx::Transition> alias_transitions_;

  std::set<Transition> transitions_;

  std::map<fitx::TransitionTrigger,
           std::vector<std::shared_ptr<TransitionRule>>>
      rules_;
};

class StateManager {
public:
  StateManager();
  StateManager(std::set<State> states);
  StateManager(const StateManager &other);
  StateManager(StateManager &&other) noexcept;
  StateManager &operator=(const StateManager &other);
  StateManager &operator=(StateManager &&other) noexcept;

  static StateManager createWithInitStates(std::vector<StateArgs> &args);

  State &createState(StateArgs &args);
  std::set<State> createStates(std::vector<StateArgs> &states);

  State &getByName(const std::string &name);
  State getByID(int id);

  void addTransition(State &source, State &target, TransitionRule &rule);

  void set(State state);
  void set(State state, std::set<fitx::Value>);

  void setEarlyStateTransition(bool early_transition);

  State &getInitState();
  const std::set<State> &getStates();
  const std::set<State> &getBugStates();

  std::shared_ptr<fitx::StateTransitionManager> TransitionManager();

  void enableStatefulConstraint(std::shared_ptr<StatefulConstraint> constraint);

  const std::shared_ptr<StatefulConstraint> &getStatefulConstraint() const {
    return propagation_constraint_;
  }

private:
  void rebindInitState();

  std::set<State> states_;
  std::set<State> bug_states_;
  std::shared_ptr<fitx::StateTransitionManager> transition_manager_;
  std::shared_ptr<fitx::StatefulConstraint> propagation_constraint_;

  State *init_state_ = nullptr;

  bool early_state_transition_ = false;
};

}; // namespace fitx
