#include "Checker/FiTx/Frontend/State.h"

#include "Checker/FiTx/Frontend/PropagationConstraint.h"
#include "Checker/FiTx/Frontend/StateTransition.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fitx {

/* State Class */
State::State(int ID, std::string name, StateType type, StateMergeMethod method,
             bool early_notification, TriggerConstraint constraint,
             BugNotificationTiming timing)
    : ID_(ID), name_(std::move(name)), type_(type), method_(method),
      timing_(timing), trigger_constraint_(constraint),
      early_notification_(early_notification) {}

State::State(int ID, StateArgs &args)
    : ID_(ID), name_(args.name_), type_(args.type_), method_(args.method_),
      timing_(args.timing_), trigger_constraint_(args.trigger_constraint_),
      early_notification_(args.early_notification_) {}

void State::addTransition(State &state, TransitionRule &rule) {
  (void)state;
  (void)rule;
}

bool State::operator<(const StateArgs &args) const {
  if (type_ == args.type_)
    return name_ < args.name_;

  return type_ < args.type_;
}

bool State::operator==(const StateArgs &args) const {
  return name_ == args.name_ && type_ == args.type_;
}

void StateManager::rebindInitState() {
  init_state_ = nullptr;
  for (const auto &s : states_) {
    if (s.isInitState()) {
      init_state_ = const_cast<State *>(&s);
      return;
    }
  }
}

StateManager::StateManager()
    : propagation_constraint_(nullptr), init_state_(nullptr),
      early_state_transition_(false) {
  StateArgs init_args = fitx::StateArgs("init", fitx::StateType::INIT);
  init_state_ = &createState(init_args);

  transition_manager_ = std::make_shared<StateTransitionManager>();
}

StateManager::StateManager(std::set<State> states)
    : states_(std::move(states)), early_state_transition_(false) {
  StateArgs init_args = fitx::StateArgs("init", fitx::StateType::INIT);
  init_state_ = &createState(init_args);

  transition_manager_ = std::make_shared<StateTransitionManager>();
}

StateManager::StateManager(const StateManager &other)
    : states_(other.states_), bug_states_(other.bug_states_),
      transition_manager_(other.transition_manager_),
      propagation_constraint_(other.propagation_constraint_),
      early_state_transition_(other.early_state_transition_) {
  rebindInitState();
}

StateManager::StateManager(StateManager &&other) noexcept
    : states_(std::move(other.states_)),
      bug_states_(std::move(other.bug_states_)),
      transition_manager_(std::move(other.transition_manager_)),
      propagation_constraint_(std::move(other.propagation_constraint_)),
      early_state_transition_(other.early_state_transition_) {
  other.init_state_ = nullptr;
  rebindInitState();
}

StateManager &StateManager::operator=(const StateManager &other) {
  if (this != &other) {
    states_ = other.states_;
    bug_states_ = other.bug_states_;
    transition_manager_ = other.transition_manager_;
    propagation_constraint_ = other.propagation_constraint_;
    early_state_transition_ = other.early_state_transition_;
    rebindInitState();
  }
  return *this;
}

StateManager &StateManager::operator=(StateManager &&other) noexcept {
  if (this != &other) {
    states_ = std::move(other.states_);
    bug_states_ = std::move(other.bug_states_);
    transition_manager_ = std::move(other.transition_manager_);
    propagation_constraint_ = std::move(other.propagation_constraint_);
    early_state_transition_ = other.early_state_transition_;
    other.init_state_ = nullptr;
    rebindInitState();
  }
  return *this;
}

State &StateManager::createState(StateArgs &args) {
  auto searched_states = std::find(states_.begin(), states_.end(), args);
  if (searched_states != states_.end())
    return const_cast<State &>(*searched_states);

  int id = args.type_ == BUG ? State::kBugStateIDBase + states_.size()
                             : states_.size();
  State new_state = State(id, args);
  auto inserted_state = states_.insert(new_state);
  return const_cast<State &>(*inserted_state.first);
}

StateManager StateManager::createWithInitStates(std::vector<StateArgs> &args) {
  StateManager manager;
  for (auto arg : args) {
    manager.createState(arg);
  }
  return manager;
}

std::set<State> StateManager::createStates(std::vector<StateArgs> &states) {
  std::set<State> new_states;
  for (auto state : states) {
    State new_state = createState(state);
    new_states.insert(new_state);
  }
  return new_states;
}

State &StateManager::getByName(const std::string &name) {
  auto state = std::find_if(
      states_.begin(), states_.end(),
      [&name](const State &t) -> bool { return name == t.Name(); });
  if (state == states_.end())
    throw std::out_of_range("Could not find the state");
  return const_cast<State &>(*state);
}

std::shared_ptr<fitx::StateTransitionManager>
StateManager::TransitionManager() {
  return transition_manager_;
}

void StateManager::addTransition(State &source, State &target,
                                 TransitionRule &rule) {
  fitx::Transition transition =
      transition_manager_->createTransition(source, target);

  transition_manager_->addTransitionRule(transition, rule);
}

const std::set<State> &StateManager::getStates() { return states_; }
const std::set<State> &StateManager::getBugStates() {
  if (!bug_states_.empty())
    return bug_states_;

  for (const auto &state : states_) {
    if (state.isBugState())
      bug_states_.insert(state);
  }
  return bug_states_;
}

State &StateManager::getInitState() {
  if (!init_state_)
    rebindInitState();
  return *init_state_;
}

void StateManager::setEarlyStateTransition(bool early_transition) {
  early_state_transition_ = early_transition;
}

void StateManager::enableStatefulConstraint(
    std::shared_ptr<StatefulConstraint> constraint) {
  propagation_constraint_ = std::move(constraint);
}

Transition::Transition(const State &source, const State &target)
    : source_(source), target_(target) {}

Transition::Transition(const Transition &transition)
    : source_(transition.source_), target_(transition.target_) {}

void Transition::addTransitionRule(std::weak_ptr<TransitionRule> arg) {
  (void)arg;
}

Transition &Transition::operator=(const Transition &transition) {
  source_ = transition.source_;
  target_ = transition.target_;
  return *this;
}

bool Transition::operator<(const Transition &transition) const {
  if (source_ == transition.source_)
    return target_ < transition.target_;
  return source_ < transition.source_;
}

bool Transition::operator==(const Transition &transition) const {
  return source_ == transition.source_ && target_ == transition.target_;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                              const fitx::Transition &transition) {
  ostream << transition.Source().Name() << " to " << transition.Target().Name();
  return ostream;
}

TransitionLogs::TransitionLogs() : transition_logs_(), warned_(false) {}

TransitionLogs::TransitionLogs(Transition transition,
                               std::shared_ptr<fitx::Instruction> instruction)
    : transition_logs_{Log{transition, std::move(instruction)}},
      least_significant_source_(transition.Source()),
      most_significant_target_(transition.Target()), warned_(false) {}

const State &TransitionLogs::CurrentState() const {
  assert(!transition_logs_.empty());
  return transition_logs_.back().transition.Target();
}

void TransitionLogs::addTransition(
    const fitx::Transition &transition,
    std::shared_ptr<fitx::Instruction> instruction) {
  transition_logs_.push_back(Log{transition, std::move(instruction)});

  if (!least_significant_source_ ||
      transition.Source() < *least_significant_source_)
    least_significant_source_ = transition.Source();

  if (!most_significant_target_ ||
      *most_significant_target_ < transition.Target())
    most_significant_target_ = transition.Target();
}

void TransitionLogs::setWarned() { warned_ = true; }

void TransitionLogs::generateLog(llvm::raw_ostream &stream) const {
  for (const auto &log : transition_logs_) {
    const auto &transition = log.transition;
    fitx::generateLog(stream, log.instruction.get(),
                      "[Transition] " + transition.Source().Name() + " to " +
                          transition.Target().Name());
  }
}

std::vector<TransitionLogs::TraceEntry>
TransitionLogs::getTraceEntries() const {
  std::vector<TraceEntry> entries;
  entries.reserve(transition_logs_.size());
  for (const Log &log : transition_logs_) {
    entries.push_back(TraceEntry{log.transition, log.instruction});
  }
  return entries;
}

void TransitionLogs::logicalTerminate(
    std::shared_ptr<fitx::Instruction> instruction) {
  auto null_transition = Transition(transition_logs_.back().transition.Target(),
                                    NullState::GetInstance());
  addTransition(null_transition, std::move(instruction));
}

Transition TransitionLogs::ReducedTransition() const {
  return Transition(transition_logs_.front().transition.Source(),
                    transition_logs_.back().transition.Target());
}

bool TransitionLogs::operator==(const TransitionLogs &logs) const {
  return transition_logs_ == logs.transition_logs_;
}

Transition StateTransitionManager::createTransition(State &source,
                                                    State &target) {
  return Transition(source, target);
}

void StateTransitionManager::addTransitionRule(Transition &transition,
                                               TransitionRule &arg) {
  std::shared_ptr<TransitionRule> rule;
  switch (arg.Trigger()) {
  case TransitionTrigger::FUNCTION_ARG: {
    auto function_arg_rule = std::make_shared<FunctionArgTransitionRule>(
        *static_cast<FunctionArgTransitionRule *>(&arg));
    registerFunctionArgTransition(function_arg_rule->FunctionArgs(),
                                  transition);
    rule = function_arg_rule;
    break;
  }
  case TransitionTrigger::STORE_VALUE: {
    auto store_value_rule = std::make_shared<StoreValueTransitionRule>(
        *static_cast<StoreValueTransitionRule *>(&arg));
    registerStoreTransition(store_value_rule, transition);
    rule = store_value_rule;
    break;
  }
  case TransitionTrigger::USE_VALUE: {
    auto use_value_transition = std::make_shared<UseValueTransitionRule>(
        *static_cast<UseValueTransitionRule *>(&arg));
    registerUseTransition(use_value_transition, transition);
    rule = use_value_transition;
    break;
  }
  case TransitionTrigger::ALIASED_VALUE: {
    auto alias_value_transition = std::make_shared<AliasValueTransitionRule>(
        *static_cast<AliasValueTransitionRule *>(&arg));
    registerAliasTransition(alias_value_transition, transition);
    rule = alias_value_transition;
    break;
  }
  }

  rules_[rule->Trigger()].push_back(rule);
  transition.addTransitionRule(rule);
}

void StateTransitionManager::registerFunctionArgTransition(
    const FunctionArgTransitionRule::FunctionArg &arg,
    const fitx::Transition &transition) {
  function_transitions_[arg].push_back(transition);
}

void StateTransitionManager::registerFunctionArgTransition(
    const std::vector<FunctionArgTransitionRule::FunctionArg> &args,
    const fitx::Transition &transition) {
  for (const auto &arg : args) {
    registerFunctionArgTransition(arg, transition);
  }
}

void StateTransitionManager::registerStoreTransition(
    std::shared_ptr<StoreValueTransitionRule> rule,
    const fitx::Transition &transition) {
  store_transitions_[rule->Type()].push_back(transition);

  if (rule->Type() == fitx::StoreValueTransitionRule::CALL_FUNC) {
    for (const auto &func : rule->FunctionNames()) {
      call_store_transitions_[func].push_back(transition);
    }
  }

  if (rule->ConsiderNullBranch()) {
    auto new_rule =
        static_cast<fitx::StoreValueTransitionRule::StoreValueType>(
            rule->Type() + 4);
    store_transitions_[new_rule].push_back(transition);
  }
}

void StateTransitionManager::registerUseTransition(
    std::shared_ptr<UseValueTransitionRule> rule,
    const fitx::Transition &transition) {
  (void)rule;
  use_transitions_.push_back(transition);
}

void StateTransitionManager::registerAliasTransition(
    std::shared_ptr<AliasValueTransitionRule> rule,
    const fitx::Transition &transition) {
  (void)rule;
  alias_transitions_.push_back(transition);
}

bool StateTransitionManager::existsInFunctionArgTransition(
    const FunctionArgTransitionRule::FunctionArg &arg) const {
  return function_transitions_.find(arg) != function_transitions_.end();
}

const std::pair<const FunctionArgTransitionRule::FunctionArg,
                std::vector<fitx::Transition>> *
StateTransitionManager::findFunctionArgTransitions(const std::string &name,
                                                   unsigned int arg_num) const {
  FunctionArgTransitionRule::FunctionArg arg(name, arg_num);
  auto it = function_transitions_.find(arg);
  if (it != function_transitions_.end())
    return &(*it);
  return nullptr;
}

const std::pair<FunctionArgTransitionRule::FunctionArg,
                std::vector<fitx::Transition>>
StateTransitionManager::getFunctionArgTransitions(const std::string &name,
                                                  unsigned int arg_num) {
  FunctionArgTransitionRule::FunctionArg arg(name, arg_num);
  auto it = function_transitions_.find(arg);
  if (it != function_transitions_.end()) {
    return *it;
  }
  return std::make_pair(arg, std::vector<fitx::Transition>());
}

const std::vector<fitx::Transition> &
StateTransitionManager::getStoreArgTransitions(
    fitx::StoreValueTransitionRule::StoreValueType type,
    const std::string &name) const {
  static const std::vector<fitx::Transition> kEmpty;
  if (type == fitx::StoreValueTransitionRule::CALL_FUNC) {
    auto it = call_store_transitions_.find(name);
    if (it != call_store_transitions_.end())
      return it->second;
  } else {
    auto it = store_transitions_.find(type);
    if (it != store_transitions_.end())
      return it->second;
  }
  return kEmpty;
}

const std::vector<fitx::Transition> &
StateTransitionManager::getUseValueTransitions() const {
  return use_transitions_;
}

const std::vector<fitx::Transition> &
StateTransitionManager::getAliasTransitions() const {
  return alias_transitions_;
}

bool StateTransitionManager::canTriggerFromInit(const llvm::Module &M) const {
  auto hasInitTransition = [](const std::vector<fitx::Transition> &vec) {
    for (const auto &t : vec) {
      if (t.Source().isInitState())
        return true;
    }
    return false;
  };

  if (hasInitTransition(use_transitions_) ||
      hasInitTransition(alias_transitions_))
    return true;

  for (const auto &kv : store_transitions_) {
    if (kv.first == fitx::StoreValueTransitionRule::CALL_FUNC)
      continue;
    if (hasInitTransition(kv.second))
      return true;
  }

  for (const auto &kv : call_store_transitions_) {
    if (hasInitTransition(kv.second) && M.getFunction(kv.first) != nullptr)
      return true;
  }

  for (const auto &kv : function_transitions_) {
    if (hasInitTransition(kv.second) &&
        M.getFunction(kv.first.function_name) != nullptr)
      return true;
  }

  return false;
}

}; // namespace fitx
