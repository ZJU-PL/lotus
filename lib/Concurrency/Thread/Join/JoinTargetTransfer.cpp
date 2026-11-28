/**
 * @file JoinTargetTransfer.cpp
 * @brief State transfer and summary logic for join-target analysis
 */

#include "Concurrency/Thread/Join/JoinTargetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/API/CppThreading.h"

#include <algorithm>
#include <deque>
#include <stack>

#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace mhp {

namespace {

constexpr uint8_t lifecycleBit(ThreadLifecycle lifecycle) {
  return static_cast<uint8_t>(lifecycle);
}

bool threadInstanceLess(const ThreadInstance &lhs, const ThreadInstance &rhs) {
  if (lhs.fork_site != rhs.fork_site) {
    return lhs.fork_site < rhs.fork_site;
  }
  if (lhs.epoch_class != rhs.epoch_class) {
    return lhs.epoch_class < rhs.epoch_class;
  }
  return static_cast<uint8_t>(lhs.execution_class) <
         static_cast<uint8_t>(rhs.execution_class);
}

template <typename T, typename Less>
void sortAndUnique(std::vector<T> &values, Less less) {
  std::sort(values.begin(), values.end(), less);
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

template <typename T>
void sortAndUnique(std::vector<T> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

void addLifecycle(InstanceState &state, ThreadLifecycle lifecycle) {
  state.lifecycle_mask |= lifecycleBit(lifecycle);
}

void removeLifecycle(InstanceState &state, ThreadLifecycle lifecycle) {
  state.lifecycle_mask &= ~lifecycleBit(lifecycle);
}

bool transitionInstance(InstanceState &state, ThreadLifecycle terminal,
                        bool keepJoinable = false) {
  const InstanceState before = state;
  if (state.mayBeJoinable() && !keepJoinable) {
    removeLifecycle(state, ThreadLifecycle::Joinable);
  }
  addLifecycle(state, terminal);
  return !(before == state);
}

bool mergeInstanceState(InstanceState &dst, const InstanceState &src) {
  const InstanceState before = dst;
  dst.lifecycle_mask |= src.lifecycle_mask;
  dst.creation_may_fail |= src.creation_may_fail;
  dst.join_may_fail |= src.join_may_fail;
  return !(before == dst);
}

bool transitionHandleState(HandleState &state, ThreadLifecycle terminal,
                           bool keepJoinable = false) {
  bool changed = false;
  for (auto &entry : state.instances) {
    changed |= transitionInstance(entry.second, terminal, keepJoinable);
  }
  if (terminal == ThreadLifecycle::Joined ||
      terminal == ThreadLifecycle::Detached ||
      terminal == ThreadLifecycle::Overwritten) {
    if (!state.preserved_arg_inputs.empty()) {
      state.preserved_arg_inputs.clear();
      changed = true;
    }
  }
  return changed;
}

bool transitionLocationFamily(JoinTargetStateMap &state,
                              const HandleLocation &location,
                              ThreadLifecycle terminal,
                              bool keepJoinable = false) {
  bool changed = false;
  auto transitionEntry = [&](HandleState &handleState) {
    if (keepJoinable) {
      handleState.path_sensitivity_lost = true;
    }
    changed |= transitionHandleState(handleState, terminal, keepJoinable);
  };

  if (location.is_base_wildcard) {
    for (auto &entry : state) {
      if (entry.first.base == location.base) {
        transitionEntry(entry.second);
      }
    }
    return changed;
  }

  auto it = state.find(location);
  if (it != state.end()) {
    transitionEntry(it->second);
  }

  HandleLocation wildcard = location;
  wildcard.offsets.clear();
  wildcard.is_base_wildcard = true;
  auto wildcardIt = state.find(wildcard);
  if (wildcardIt != state.end()) {
    transitionEntry(wildcardIt->second);
  }

  return changed;
}

std::vector<ThreadInstance> collectInstances(const HandleState &state,
                                             bool feasibleOnly) {
  std::vector<ThreadInstance> instances;
  for (const auto &entry : state.instances) {
    if (feasibleOnly && !entry.second.mayBeJoinable()) {
      continue;
    }
    instances.push_back(entry.first);
  }
  sortAndUnique(instances, threadInstanceLess);
  return instances;
}

std::vector<const Instruction *>
collectForks(const std::vector<ThreadInstance> &instances) {
  std::vector<const Instruction *> forks;
  forks.reserve(instances.size());
  for (const ThreadInstance &instance : instances) {
    if (instance.fork_site) {
      forks.push_back(instance.fork_site);
    }
  }
  sortAndUnique(forks);
  return forks;
}

std::vector<const Value *> collectRoots(const HandleState &state) {
  std::vector<const Value *> roots(state.provenance_roots.begin(),
                                   state.provenance_roots.end());
  sortAndUnique(roots);
  return roots;
}

bool sameInstances(const std::vector<ThreadInstance> &lhs,
                   const std::vector<ThreadInstance> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (!(lhs[i] == rhs[i])) {
      return false;
    }
  }
  return true;
}

bool isMayLocationSet(
    const std::unordered_set<HandleLocation, HandleLocationHash> &locations) {
  return locations.size() != 1 ||
         (!locations.empty() && locations.begin()->is_base_wildcard);
}

int64_t getCanonicalOffset(const std::vector<int64_t> &offsets) {
  return offsets.empty() ? 0 : offsets.front();
}

void setCanonicalOffset(std::vector<int64_t> &offsets, int64_t offset) {
  offsets.clear();
  if (offset != 0) {
    offsets.push_back(offset);
  }
}

bool isEmptyHandleState(const HandleState &state) {
  return state.instances.empty() && state.preserved_arg_inputs.empty() &&
         state.provenance_roots.empty() && !state.has_unknown_live_fork &&
         !state.path_sensitivity_lost;
}

bool isCppThreadMove(const Function *callee) {
  if (!callee) {
    return false;
  }
  const StringRef name = callee->getName();
  return CppThreadingModel::isStdThreadEntity(name) &&
         (name.contains("C1EOS_") || name.contains("C2EOS_") ||
          name.contains("aSEOS_"));
}

} // namespace

std::size_t ThreadInstanceHash::operator()(
    const ThreadInstance &instance) const {
  std::size_t seed = std::hash<const Instruction *>{}(instance.fork_site);
  seed ^= std::hash<uint8_t>{}(instance.epoch_class) + 0x9e3779b9 +
          (seed << 6) + (seed >> 2);
  seed ^= std::hash<uint8_t>{}(static_cast<uint8_t>(instance.execution_class)) +
          0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

void JoinTargetAnalysis::buildFunctionSummaries() {
  // Only functions transitively called from join-containing functions can ever
  // have their summaries consulted by analyzeFunction(). Restricting summary
  // computation to this downward call closure avoids analyzing tens of
  // thousands of unrelated functions in large modules.
  std::unordered_map<const Function *, std::vector<const Function *>> callees,
      callers;
  std::unordered_set<const Function *> neededFuncs;
  std::deque<const Function *> reachQueue;
  for (const Instruction *joinInst : m_joinInsts) {
    if (joinInst && joinInst->getFunction() &&
        !joinInst->getFunction()->isDeclaration()) {
      reachQueue.push_back(joinInst->getFunction());
    }
  }

  std::unordered_set<const Function *> scannedForCallees;
  while (!reachQueue.empty()) {
    const Function *cur = reachQueue.front();
    reachQueue.pop_front();
    if (!scannedForCallees.insert(cur).second) {
      continue;
    }
    DenseSet<const Function *> seenCallees;
    for (const Instruction &inst : instructions(*cur)) {
      if (const auto *cb = dyn_cast<CallBase>(&inst)) {
        if (const Function *target = m_threadAPI->getCallee(cb)) {
          if (!target->isDeclaration() && seenCallees.insert(target).second) {
            callees[cur].push_back(target);
            callers[target].push_back(cur);
            if (neededFuncs.insert(target).second) {
              reachQueue.push_back(target);
            }
          }
        }
      }
    }
  }

  if (neededFuncs.empty()) {
    return;
  }

  // Mark functions that directly contain or transitively reach thread
  // lifecycle operations (fork/join/detach/std::thread move). Functions outside
  // this set cannot create or transition thread instances, so only small pure
  // handle-forwarding helpers need full flow-sensitive summary construction.
  std::unordered_set<const Function *> threadOpReachable;
  std::deque<const Function *> opQueue;
  for (const Function *func : neededFuncs) {
    for (const Instruction &inst : instructions(*func)) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb) {
        continue;
      }
      const Function *callee = m_threadAPI->getCallee(cb);
      if (m_threadAPI->isTDFork(cb) || m_threadAPI->isTDJoin(cb) ||
          m_threadAPI->getType(cb) == ThreadAPI::TD_DETACH ||
          (callee && isCppThreadMove(callee))) {
        if (threadOpReachable.insert(func).second) {
          opQueue.push_back(func);
        }
        break;
      }
    }
  }
  while (!opQueue.empty()) {
    const Function *cur = opQueue.front();
    opQueue.pop_front();
    auto callersIt = callers.find(cur);
    if (callersIt == callers.end()) {
      continue;
    }
    for (const Function *caller : callersIt->second) {
      if (neededFuncs.count(caller) &&
          threadOpReachable.insert(caller).second) {
        opQueue.push_back(caller);
      }
    }
  }

  // Keep every function that transitively reaches a thread lifecycle operation
  // OR can read/write pointer arguments/globals/return values. Functions that
  // neither reach any thread operation nor accept/return pointers and do not
  // touch globals cannot affect thread handle states.
  std::unordered_set<const Function *> summaryFuncs;
  summaryFuncs.reserve(neededFuncs.size());
  for (const Function *func : neededFuncs) {
    if (threadOpReachable.count(func)) {
      summaryFuncs.insert(func);
      continue;
    }
    bool canTransportHandle = func->getReturnType()->isPointerTy() ||
                              func->getReturnType()->isIntegerTy(64);
    if (!canTransportHandle) {
      for (const Argument &arg : func->args()) {
        if (arg.getType()->isPointerTy()) {
          canTransportHandle = true;
          break;
        }
      }
    }
    if (canTransportHandle && !func->doesNotAccessMemory()) {
      summaryFuncs.insert(func);
    }
  }

  if (summaryFuncs.empty()) {
    return;
  }

  std::vector<const Function *> postOrder;
  postOrder.reserve(summaryFuncs.size());
  std::unordered_set<const Function *> visited;
  std::stack<std::pair<const Function *, bool>> dfsStack;
  for (const Function *func : summaryFuncs) {
    if (visited.count(func)) {
      continue;
    }
    dfsStack.push({func, false});
    while (!dfsStack.empty()) {
      auto [cur, done] = dfsStack.top();
      dfsStack.pop();
      if (done) {
        postOrder.push_back(cur);
        continue;
      }
      if (!visited.insert(cur).second) {
        continue;
      }
      dfsStack.push({cur, true});
      auto it = callees.find(cur);
      if (it == callees.end()) {
        continue;
      }
      for (const Function *target : it->second) {
        if (summaryFuncs.count(target) && !visited.count(target)) {
          dfsStack.push({target, false});
        }
      }
    }
  }

  std::deque<const Function *> funcWorklist(postOrder.begin(), postOrder.end());
  std::unordered_set<const Function *> funcInWorklist(postOrder.begin(),
                                                      postOrder.end());
  std::unordered_map<const Function *, unsigned> evalCount;
  constexpr unsigned kMaxSummaryEvals = 32;
  constexpr size_t kMaxBlockEvalsPerFunction = 2048;
  constexpr size_t kMaxStateLocations = 512;

  while (!funcWorklist.empty()) {
    const Function *funcPtr = funcWorklist.front();
    funcWorklist.pop_front();
    funcInWorklist.erase(funcPtr);
    if (!funcPtr || funcPtr->isDeclaration() || funcPtr->empty() ||
        ++evalCount[funcPtr] > kMaxSummaryEvals) {
      continue;
    }
    const Function &func = *funcPtr;

    std::unordered_map<const BasicBlock *, StateMap> inStates;
    std::unordered_map<const BasicBlock *, StateMap> outStates;
    std::deque<const BasicBlock *> bbWorklist;
    DenseSet<const BasicBlock *> bbInWorklist;
    const BasicBlock *entryBB = &func.getEntryBlock();
    bbWorklist.push_back(entryBB);
    bbInWorklist.insert(entryBB);
    size_t bbEvals = 0;
    bool stateOverflow = false;

    while (!bbWorklist.empty() && ++bbEvals <= kMaxBlockEvalsPerFunction) {
      const BasicBlock *block = bbWorklist.front();
      bbWorklist.pop_front();
      bbInWorklist.erase(block);

      StateMap inState;
      if (block == entryBB) {
        inState = getEntryStateForFunction(func);
      } else {
        for (const BasicBlock *pred : predecessors(block)) {
          auto predIt = outStates.find(pred);
          if (predIt != outStates.end()) {
            mergeStateInto(inState, predIt->second);
          }
        }
      }

      if (inState.size() > kMaxStateLocations) {
        stateOverflow = true;
        break;
      }

      auto inIt = inStates.find(block);
      const bool inChanged =
          (inIt == inStates.end() || !(inIt->second == inState));
      if (inChanged) {
        inStates[block] = inState;
      }

      StateMap outState = std::move(inState);
      for (const Instruction &inst : *block) {
        transferInstruction(inst, outState);
        if (outState.size() > kMaxStateLocations) {
          stateOverflow = true;
          break;
        }
      }
      if (stateOverflow) {
        break;
      }

      auto outIt = outStates.find(block);
      if (outIt == outStates.end() || !(outIt->second == outState)) {
        outStates[block] = std::move(outState);
        for (const BasicBlock *succ : successors(block)) {
          if (bbInWorklist.insert(succ).second) {
            bbWorklist.push_back(succ);
          }
        }
      }
    }

    FunctionSummary summary;
    if (stateOverflow || bbEvals > kMaxBlockEvalsPerFunction) {
      // Conservative widening if an unusually large helper exceeds the block
      // evaluation budget: mark pointer args as unknown/escaped.
      for (const Argument &arg : func.args()) {
        if (!arg.getType()->isPointerTy()) {
          continue;
        }
        SummaryLocation loc;
        loc.arg_no = arg.getArgNo();
        loc.is_base_wildcard = true;
        HandleState unknown;
        unknown.has_unknown_live_fork = true;
        unknown.path_sensitivity_lost = true;
        summary.location_exit_states[loc] = std::move(unknown);
      }
      summary.return_state.has_unknown_live_fork = true;
      summary.return_state.path_sensitivity_lost = true;
    } else {
      for (const BasicBlock &block : func) {
        const Instruction *term = block.getTerminator();
        if (!isa<ReturnInst>(term) && !isa<ResumeInst>(term)) {
          continue;
        }

        auto outIt = outStates.find(&block);
        if (outIt == outStates.end()) {
          continue;
        }

        for (const auto &entry : outIt->second) {
          const auto *arg = dyn_cast<Argument>(entry.first.base);
          if (arg && arg->getParent() == &func) {
            SummaryLocation summaryLocation;
            summaryLocation.arg_no = arg->getArgNo();
            summaryLocation.offsets = entry.first.offsets;
            summaryLocation.is_base_wildcard = entry.first.is_base_wildcard;
            mergeHandleState(summary.location_exit_states[summaryLocation],
                             entry.second);
          } else if (isa<GlobalValue>(entry.first.base)) {
            mergeHandleState(summary.global_exit_states[entry.first],
                             entry.second);
          }
        }

        if (const auto *ret = dyn_cast<ReturnInst>(term)) {
          if (const Value *returnValue = ret->getReturnValue()) {
            mergeHandleState(summary.return_state,
                             getStateForValue(returnValue, outIt->second));
          }
        }
      }
    }

    auto summaryIt = m_functionSummaries.find(&func);
    if (summaryIt == m_functionSummaries.end() ||
        summaryIt->second.location_exit_states !=
            summary.location_exit_states ||
        summaryIt->second.global_exit_states != summary.global_exit_states ||
        !(summaryIt->second.return_state == summary.return_state)) {
      m_functionSummaries[&func] = std::move(summary);
      auto callersIt = callers.find(&func);
      if (callersIt != callers.end()) {
        for (const Function *caller : callersIt->second) {
          if (summaryFuncs.count(caller) &&
              evalCount[caller] < kMaxSummaryEvals &&
              funcInWorklist.insert(caller).second) {
            funcWorklist.push_back(caller);
          }
        }
      }
    }
  }
}

JoinTargetAnalysis::StateMap
JoinTargetAnalysis::getEntryStateForFunction(const Function &func) const {
  StateMap state;
  for (const Argument &arg : func.args()) {
    if (!arg.getType()->isPointerTy()) {
      continue;
    }
    HandleState argState;
    argState.preserved_arg_inputs.insert(arg.getArgNo());
    argState.provenance_roots.insert(&arg);
    state[HandleLocation{&arg, {}, false}] = std::move(argState);
  }
  return state;
}

JoinTargetAnalysis::StateMap
JoinTargetAnalysis::mergePredecessorStates(const BasicBlock *block) const {
  StateMap merged;
  if (!block) {
    return merged;
  }

  if (block == &block->getParent()->getEntryBlock()) {
    return getEntryStateForFunction(*block->getParent());
  }

  for (const BasicBlock *pred : predecessors(block)) {
    auto it = m_blockOutStates.find(pred);
    if (it != m_blockOutStates.end()) {
      mergeStateInto(merged, it->second);
    }
  }
  return merged;
}

void JoinTargetAnalysis::analyzeFunction(const Function &func) {
  m_blockInStates.clear();
  m_blockOutStates.clear();
  if (func.empty()) {
    return;
  }

  std::deque<const BasicBlock *> bbWorklist;
  DenseSet<const BasicBlock *> bbInWorklist;
  const BasicBlock *entryBB = &func.getEntryBlock();
  bbWorklist.push_back(entryBB);
  bbInWorklist.insert(entryBB);

  constexpr size_t kMaxAnalyzeBlockEvals = 4096;
  size_t bbEvals = 0;

  while (!bbWorklist.empty() && ++bbEvals <= kMaxAnalyzeBlockEvals) {
    const BasicBlock *block = bbWorklist.front();
    bbWorklist.pop_front();
    bbInWorklist.erase(block);

    StateMap inState = mergePredecessorStates(block);
    auto inIt = m_blockInStates.find(block);
    if (inIt == m_blockInStates.end() || !(inIt->second == inState)) {
      m_blockInStates[block] = inState;
    }

    StateMap outState = std::move(inState);
    for (const Instruction &inst : *block) {
      if (m_threadAPI->isTDJoin(&inst)) {
        recordJoinState(&inst, outState);
      }
      transferInstruction(inst, outState);
    }

    auto outIt = m_blockOutStates.find(block);
    if (outIt == m_blockOutStates.end() || !(outIt->second == outState)) {
      m_blockOutStates[block] = std::move(outState);
      for (const BasicBlock *succ : successors(block)) {
        if (bbInWorklist.insert(succ).second) {
          bbWorklist.push_back(succ);
        }
      }
    }
  }
}

bool JoinTargetAnalysis::transferInstruction(const Instruction &inst,
                                             StateMap &state) const {
  if (const auto *store = dyn_cast<StoreInst>(&inst)) {
    HandleState storedState = getStateForValue(store->getValueOperand(), state);
    bool changed = false;
    const auto locations = resolveWriteLocations(store->getPointerOperand());
    const bool mayWrite = isMayLocationSet(locations);
    for (const HandleLocation &location : locations) {
      HandleState newState = storedState;
      if (mayWrite) {
        newState.path_sensitivity_lost = true;
        changed |= weakUpdateLocation(state, location, newState);
      } else {
        changed |= overwriteLocation(state, location, newState);
      }
      changed |= weakenMayAliasLocations(state, location, newState);
    }
    return changed;
  }

  if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
    HandleState unknown;
    unknown.has_unknown_live_fork = true;
    unknown.path_sensitivity_lost = true;
    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(rmw->getPointerOperand())) {
      changed |= weakUpdateLocation(state, location, unknown);
      changed |= weakenMayAliasLocations(state, location, unknown);
    }
    return changed;
  }

  if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(&inst)) {
    HandleState unknown;
    unknown.has_unknown_live_fork = true;
    unknown.path_sensitivity_lost = true;
    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(cmpxchg->getPointerOperand())) {
      changed |= weakUpdateLocation(state, location, unknown);
      changed |= weakenMayAliasLocations(state, location, unknown);
    }
    return changed;
  }

  if (isa<CallBase>(&inst)) {
    return applyCallEffect(inst, state);
  }

  return false;
}

ThreadInstance
JoinTargetAnalysis::makeThreadInstance(const Instruction &forkInst) const {
  ThreadInstance instance;
  instance.fork_site = &forkInst;
  const bool repeated =
      m_threadMultiplicity &&
      m_threadMultiplicity->instructionMayExecuteMultipleTimes(&forkInst);
  instance.execution_class = repeated ? ThreadExecutionClass::RepeatedExecution
                                      : ThreadExecutionClass::SingleExecution;
  instance.epoch_class = repeated ? 1 : 0;
  return instance;
}

bool JoinTargetAnalysis::applyCallEffect(const Instruction &inst,
                                         StateMap &state) const {
  const auto *call = dyn_cast<CallBase>(&inst);
  if (!call) {
    return false;
  }

  const Function *callee = m_threadAPI->getCallee(&inst);
  if (m_threadAPI->isTDFork(&inst)) {
    HandleState forkState;
    InstanceState instanceState;
    addLifecycle(instanceState, ThreadLifecycle::Joinable);
    forkState.instances.emplace(makeThreadInstance(inst), instanceState);
    forkState.provenance_roots.insert(
        traceThreadHandleRoot(m_threadAPI->getForkedThread(&inst), &m_module));

    bool changed = false;
    const auto locations =
        resolveWriteLocations(m_threadAPI->getForkedThread(&inst));
    const bool mayWrite = isMayLocationSet(locations);
    for (const HandleLocation &location : locations) {
      if (location.base) {
        forkState.provenance_roots.insert(location.base);
      }
      if (mayWrite) {
        forkState.path_sensitivity_lost = true;
        changed |= weakUpdateLocation(state, location, forkState);
      } else {
        changed |= overwriteLocation(state, location, forkState);
      }
      changed |= weakenMayAliasLocations(state, location, forkState);
    }
    return changed;
  }

  if (m_threadAPI->isTDJoin(&inst)) {
    bool changed = false;
    const auto locations =
        resolveWriteLocations(m_threadAPI->getJoinedThread(&inst));
    const bool mayTransition = isMayLocationSet(locations);
    for (const HandleLocation &location : locations) {
      changed |= transitionLocationFamily(state, location,
                                          ThreadLifecycle::Joined,
                                          mayTransition);
    }
    changed |= transitionMayAliasLocations(
        state, m_threadAPI->getJoinedThread(&inst), ThreadLifecycle::Joined);
    return changed;
  }

  if (m_threadAPI->getType(call) == ThreadAPI::TD_DETACH) {
    if (call->arg_size() < 1) {
      return false;
    }
    bool changed = false;
    const auto locations = resolveWriteLocations(call->getArgOperand(0));
    const bool mayTransition = isMayLocationSet(locations);
    for (const HandleLocation &location : locations) {
      changed |= transitionLocationFamily(state, location,
                                          ThreadLifecycle::Detached,
                                          mayTransition);
    }
    changed |= transitionMayAliasLocations(state, call->getArgOperand(0),
                                           ThreadLifecycle::Detached);
    return changed;
  }

  if (callee && isCppThreadMove(callee) && call->arg_size() >= 2) {
    HandleState sourceState = getStateForValue(call->getArgOperand(1), state);
    bool changed = false;
    const auto destinations = resolveWriteLocations(call->getArgOperand(0));
    const bool mayWrite = isMayLocationSet(destinations);
    for (const HandleLocation &destination : destinations) {
      if (mayWrite) {
        sourceState.path_sensitivity_lost = true;
        changed |= weakUpdateLocation(state, destination, sourceState);
      } else {
        changed |= overwriteLocation(state, destination, sourceState);
      }
      changed |= weakenMayAliasLocations(state, destination, sourceState);
    }

    const auto sources = resolveWriteLocations(call->getArgOperand(1));
    const bool mayTransition = isMayLocationSet(sources);
    for (const HandleLocation &source : sources) {
      changed |= transitionLocationFamily(state, source,
                                          ThreadLifecycle::Overwritten,
                                          mayTransition);
    }
    return changed;
  }

  if (callee && !callee->isDeclaration()) {
    return applyDirectCallSummary(*call, *callee, state);
  }

  if (call->doesNotAccessMemory() || call->onlyReadsMemory()) {
    return false;
  }

  bool changed = false;
  for (unsigned i = 0; i < call->arg_size(); ++i) {
    const Value *arg = call->getArgOperand(i);
    if (!arg || !arg->getType()->isPointerTy()) {
      continue;
    }
    if (call->paramHasAttr(i, Attribute::ReadNone) ||
        call->paramHasAttr(i, Attribute::ReadOnly)) {
      continue;
    }

    const auto argumentLocations = resolveWriteLocations(arg);
    for (auto &entry : state) {
      bool mayAffect = false;
      for (const HandleLocation &argumentLocation : argumentLocations) {
        if (argumentLocation.base == entry.first.base) {
          mayAffect = true;
          break;
        }
      }
      if (!mayAffect && m_aliasAnalysis && entry.first.base &&
          entry.first.base->getType()->isPointerTy()) {
        mayAffect = m_aliasAnalysis->mayAlias(arg, entry.first.base);
      }
      if (!mayAffect) {
        continue;
      }
      const HandleState before = entry.second;
      entry.second.has_unknown_live_fork = true;
      entry.second.path_sensitivity_lost = true;
      transitionHandleState(entry.second, ThreadLifecycle::Escaped, true);
      changed |= !(before == entry.second);
    }
  }
  if (!call->onlyAccessesArgMemory()) {
    for (auto &entry : state) {
      if (!isa<GlobalValue>(entry.first.base)) {
        continue;
      }
      const HandleState before = entry.second;
      entry.second.has_unknown_live_fork = true;
      entry.second.path_sensitivity_lost = true;
      transitionHandleState(entry.second, ThreadLifecycle::Escaped, true);
      changed |= !(before == entry.second);
    }
  }
  return changed;
}

bool JoinTargetAnalysis::applyDirectCallSummary(const CallBase &call,
                                                const Function &callee,
                                                StateMap &state) const {
  auto summaryIt = m_functionSummaries.find(&callee);
  if (summaryIt == m_functionSummaries.end()) {
    return false;
  }

  struct MappedEffect {
    HandleLocation location;
    HandleState state;
  };
  std::vector<MappedEffect> effects;

  for (const auto &entry : summaryIt->second.location_exit_states) {
    unsigned argNo = entry.first.arg_no;
    if (argNo >= call.arg_size()) {
      continue;
    }

    HandleState resolved;
    for (unsigned preservedArg : entry.second.preserved_arg_inputs) {
      if (preservedArg >= call.arg_size()) {
        continue;
      }
      mergeHandleState(
          resolved, getStateForValue(call.getArgOperand(preservedArg), state));
    }

    mergeHandleState(resolved, entry.second);

    for (const HandleLocation &location :
         resolveWriteLocations(call.getArgOperand(argNo))) {
      HandleLocation mappedLocation = location;
      if (mappedLocation.is_base_wildcard || entry.first.is_base_wildcard) {
        mappedLocation.offsets.clear();
        mappedLocation.is_base_wildcard = true;
        resolved.path_sensitivity_lost = true;
      } else {
        setCanonicalOffset(
            mappedLocation.offsets,
            getCanonicalOffset(mappedLocation.offsets) +
                getCanonicalOffset(entry.first.offsets));
      }
      effects.push_back({std::move(mappedLocation), resolved});
    }
  }

  for (const auto &entry : summaryIt->second.global_exit_states) {
    effects.push_back({entry.first, entry.second});
  }

  std::vector<bool> aliasUncertain(effects.size(), false);
  for (size_t i = 0; i < effects.size(); ++i) {
    for (size_t j = i + 1; j < effects.size(); ++j) {
      if (locationsMayAlias(effects[i].location, effects[j].location) &&
          !(effects[i].location == effects[j].location)) {
        aliasUncertain[i] = true;
        aliasUncertain[j] = true;
      }
    }
  }

  std::unordered_map<HandleLocation, HandleState, HandleLocationHash>
      mergedEffects;
  std::unordered_map<HandleLocation, unsigned, HandleLocationHash> effectCounts;
  for (size_t i = 0; i < effects.size(); ++i) {
    HandleState effectState = effects[i].state;
    effectState.path_sensitivity_lost |= aliasUncertain[i];
    mergeHandleState(mergedEffects[effects[i].location], effectState);
    ++effectCounts[effects[i].location];
  }

  bool changed = false;
  for (auto &entry : mergedEffects) {
    const bool uncertainEffect = entry.first.is_base_wildcard ||
                                 effectCounts[entry.first] > 1;
    if (uncertainEffect) {
      entry.second.path_sensitivity_lost = true;
      changed |= weakUpdateLocation(state, entry.first, entry.second);
    } else {
      changed |= overwriteLocation(state, entry.first, entry.second);
    }
    changed |= weakenMayAliasLocations(state, entry.first, entry.second);
  }

  if (!call.getType()->isVoidTy()) {
    HandleState resolvedReturn;
    for (unsigned preservedArg :
         summaryIt->second.return_state.preserved_arg_inputs) {
      if (preservedArg < call.arg_size()) {
        mergeHandleState(
            resolvedReturn,
            getStateForValue(call.getArgOperand(preservedArg), state));
      }
    }
    mergeHandleState(resolvedReturn, summaryIt->second.return_state);
    if (!resolvedReturn.instances.empty() ||
        resolvedReturn.has_unknown_live_fork ||
        !resolvedReturn.preserved_arg_inputs.empty()) {
      changed |= overwriteLocation(
          state, HandleLocation{&call, {}, false}, resolvedReturn);
    }
  }
  return changed;
}

JoinResolution
JoinTargetAnalysis::buildResolutionFromState(const HandleState &state) const {
  JoinResolution resolution;
  resolution.possible_instances = collectInstances(state, false);
  resolution.feasible_instances = collectInstances(state, true);
  resolution.possible_forks = collectForks(resolution.possible_instances);
  resolution.feasible_forks = collectForks(resolution.feasible_instances);
  resolution.related_handle_roots = collectRoots(state);
  resolution.has_unknown_live_instance = state.has_unknown_live_fork;
  if (state.path_sensitivity_lost) {
    addAmbiguityReason(resolution,
                       JoinAmbiguityReason::PathMergedAlternatives);
  }
  return resolution;
}

JoinResolution JoinTargetAnalysis::buildJoinResolution(const Instruction *joinInst,
                                                       const StateMap &state) const {
  JoinResolution resolution;
  if (!joinInst) {
    return resolution;
  }

  const Value *joinedValue = m_threadAPI->getJoinedThread(joinInst);
  const Value *stripped = joinedValue ? joinedValue->stripPointerCasts() : nullptr;

  auto appendAlternative = [&](JoinPathAlternative alternative) {
    if (alternative.incoming_block || !alternative.feasible_instances.empty() ||
        !alternative.possible_instances.empty() ||
        alternative.has_unknown_live_instance ||
        !alternative.related_handle_roots.empty()) {
      resolution.path_alternatives.push_back(std::move(alternative));
    }
  };

  if (const auto *phi = dyn_cast_or_null<PHINode>(stripped)) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      const BasicBlock *incomingBlock = phi->getIncomingBlock(i);
      const StateMap *incomingState = &state;
      auto outIt = m_blockOutStates.find(incomingBlock);
      if (outIt != m_blockOutStates.end()) {
        incomingState = &outIt->second;
      }
      JoinResolution altResolution =
          buildResolutionFromState(getStateForValue(phi->getIncomingValue(i),
                                                    *incomingState));
      for (JoinAmbiguityReason reason : altResolution.ambiguity_reasons) {
        addAmbiguityReason(resolution, reason);
      }
      JoinPathAlternative alternative;
      alternative.incoming_block = incomingBlock;
      alternative.possible_instances = std::move(altResolution.possible_instances);
      alternative.feasible_instances = std::move(altResolution.feasible_instances);
      alternative.possible_forks = std::move(altResolution.possible_forks);
      alternative.feasible_forks = std::move(altResolution.feasible_forks);
      alternative.related_handle_roots =
          std::move(altResolution.related_handle_roots);
      alternative.has_unknown_live_instance =
          altResolution.has_unknown_live_instance;
      appendAlternative(std::move(alternative));
    }
  } else if (const auto *select = dyn_cast_or_null<SelectInst>(stripped)) {
    for (const Value *arm : {select->getTrueValue(), select->getFalseValue()}) {
      JoinResolution altResolution =
          buildResolutionFromState(getStateForValue(arm, state));
      for (JoinAmbiguityReason reason : altResolution.ambiguity_reasons) {
        addAmbiguityReason(resolution, reason);
      }
      JoinPathAlternative alternative;
      alternative.possible_instances = std::move(altResolution.possible_instances);
      alternative.feasible_instances = std::move(altResolution.feasible_instances);
      alternative.possible_forks = std::move(altResolution.possible_forks);
      alternative.feasible_forks = std::move(altResolution.feasible_forks);
      alternative.related_handle_roots =
          std::move(altResolution.related_handle_roots);
      alternative.has_unknown_live_instance =
          altResolution.has_unknown_live_instance;
      appendAlternative(std::move(alternative));
    }
  }

  if (resolution.path_alternatives.empty()) {
    resolution = buildResolutionFromState(getStateForValue(joinedValue, state));
  } else {
    resolution.is_path_sensitive = resolution.path_alternatives.size() > 1;
    for (const JoinPathAlternative &alternative : resolution.path_alternatives) {
      resolution.possible_instances.insert(resolution.possible_instances.end(),
                                           alternative.possible_instances.begin(),
                                           alternative.possible_instances.end());
      resolution.feasible_instances.insert(resolution.feasible_instances.end(),
                                           alternative.feasible_instances.begin(),
                                           alternative.feasible_instances.end());
      resolution.possible_forks.insert(resolution.possible_forks.end(),
                                       alternative.possible_forks.begin(),
                                       alternative.possible_forks.end());
      resolution.feasible_forks.insert(resolution.feasible_forks.end(),
                                       alternative.feasible_forks.begin(),
                                       alternative.feasible_forks.end());
      resolution.related_handle_roots.insert(
          resolution.related_handle_roots.end(),
          alternative.related_handle_roots.begin(),
          alternative.related_handle_roots.end());
      resolution.has_unknown_live_instance |=
          alternative.has_unknown_live_instance;
    }

    sortAndUnique(resolution.possible_instances, threadInstanceLess);
    sortAndUnique(resolution.feasible_instances, threadInstanceLess);
    sortAndUnique(resolution.possible_forks);
    sortAndUnique(resolution.feasible_forks);
    sortAndUnique(resolution.related_handle_roots);

    const JoinPathAlternative &first = resolution.path_alternatives.front();
    for (size_t i = 1; i < resolution.path_alternatives.size(); ++i) {
      const JoinPathAlternative &alternative = resolution.path_alternatives[i];
      if (!sameInstances(first.feasible_instances, alternative.feasible_instances) ||
          first.has_unknown_live_instance !=
              alternative.has_unknown_live_instance) {
        addAmbiguityReason(resolution,
                           JoinAmbiguityReason::PathMergedAlternatives);
        break;
      }
    }
  }

  for (const HandleLocation &location : resolveReadLocations(joinedValue)) {
    if (location.is_base_wildcard) {
      addAmbiguityReason(resolution, JoinAmbiguityReason::WildcardLocation);
      break;
    }
  }

  return resolution;
}

void JoinTargetAnalysis::finalizeJoinResolution(
    const Instruction *joinInst, JoinResolution &resolution) const {
  if (!joinInst) {
    return;
  }

  if (resolution.feasible_instances.empty()) {
    addAmbiguityReason(resolution, JoinAmbiguityReason::NoFeasibleInstance);
  }
  if (resolution.feasible_instances.size() > 1) {
    addAmbiguityReason(resolution,
                       JoinAmbiguityReason::MultipleFeasibleInstances);
  }
  if (resolution.has_unknown_live_instance) {
    addAmbiguityReason(resolution, JoinAmbiguityReason::UnknownExternalEffect);
  }

  for (const ThreadInstance &instance : resolution.feasible_instances) {
    if (instance.execution_class == ThreadExecutionClass::RepeatedExecution) {
      addAmbiguityReason(resolution, JoinAmbiguityReason::RepeatedForkSite);
      break;
    }
  }

  resolution.unambiguous = resolution.feasible_instances.size() == 1 &&
                           !resolution.has_unknown_live_instance &&
                           resolution.ambiguity_reasons.empty();
  if (resolution.unambiguous) {
    const ThreadInstance &instance = resolution.feasible_instances.front();
    if (instance.execution_class == ThreadExecutionClass::RepeatedExecution) {
      resolution.unambiguous = false;
    }
  }
}

bool JoinTargetAnalysis::addAmbiguityReason(JoinResolution &resolution,
                                            JoinAmbiguityReason reason) const {
  if (reason == JoinAmbiguityReason::None) {
    return false;
  }
  if (std::find(resolution.ambiguity_reasons.begin(),
                resolution.ambiguity_reasons.end(),
                reason) != resolution.ambiguity_reasons.end()) {
    return false;
  }
  resolution.ambiguity_reasons.push_back(reason);
  return true;
}

void JoinTargetAnalysis::recordJoinState(const Instruction *join_inst,
                                         const StateMap &state) {
  if (!join_inst) {
    return;
  }

  m_joinResolutions[join_inst] = buildJoinResolution(join_inst, state);
}

bool JoinTargetAnalysis::overwriteLocation(StateMap &state,
                                           const HandleLocation &location,
                                           const HandleState &new_state) const {
  if (!location.base) {
    return false;
  }

  HandleState combined = new_state;
  auto existingIt = state.find(location);
  if (existingIt != state.end()) {
    HandleState preserved = existingIt->second;
    preserved.preserved_arg_inputs.clear();
    preserved.has_unknown_live_fork = false;
    preserved.path_sensitivity_lost = false;
    for (auto &entry : preserved.instances) {
      auto newIt = new_state.instances.find(entry.first);
      if (newIt == new_state.instances.end() || !newIt->second.mayBeJoinable()) {
        if (entry.second.mayBeJoinable()) {
          removeLifecycle(entry.second, ThreadLifecycle::Joinable);
          addLifecycle(entry.second, ThreadLifecycle::Overwritten);
        }
      }
    }
    mergeHandleState(combined, preserved);
  }

  auto stateIt = state.find(location);
  if (stateIt == state.end() && isEmptyHandleState(combined)) {
    return false;
  }
  if (stateIt != state.end() && stateIt->second == combined) {
    return false;
  }
  state[location] = std::move(combined);
  return true;
}

bool JoinTargetAnalysis::weakUpdateLocation(
    StateMap &state, const HandleLocation &location,
    const HandleState &new_state) const {
  if (!location.base) {
    return false;
  }

  bool changed = false;
  HandleState uncertainState = new_state;
  uncertainState.path_sensitivity_lost = true;

  if (location.is_base_wildcard) {
    bool foundFamily = false;
    for (auto &entry : state) {
      if (entry.first.base != location.base) {
        continue;
      }
      foundFamily = true;
      changed |= mergeHandleState(entry.second, uncertainState);
    }
    HandleLocation wildcard{location.base, {}, true};
    changed |= mergeHandleState(state[wildcard], uncertainState);
    return changed || !foundFamily;
  }

  changed |= mergeHandleState(state[location], uncertainState);
  return changed;
}

bool JoinTargetAnalysis::locationsMayAlias(const HandleLocation &lhs,
                                           const HandleLocation &rhs) const {
  if (!lhs.base || !rhs.base) {
    return true;
  }
  if (lhs.base == rhs.base) {
    return lhs.is_base_wildcard || rhs.is_base_wildcard ||
           lhs.offsets == rhs.offsets;
  }
  if (!m_aliasAnalysis || !lhs.base->getType()->isPointerTy() ||
      !rhs.base->getType()->isPointerTy()) {
    return false;
  }
  return m_aliasAnalysis->mayAlias(lhs.base, rhs.base);
}

bool JoinTargetAnalysis::weakenMayAliasLocations(
    StateMap &state, const HandleLocation &written_location,
    const HandleState &written_state) const {
  bool changed = false;
  HandleState uncertain = written_state;
  uncertain.path_sensitivity_lost = true;
  for (auto &entry : state) {
    if (entry.first == written_location ||
        !locationsMayAlias(entry.first, written_location)) {
      continue;
    }
    changed |= mergeHandleState(entry.second, uncertain);
  }
  return changed;
}

bool JoinTargetAnalysis::transitionMayAliasLocations(
    StateMap &state, const Value *pointer, ThreadLifecycle terminal) const {
  if (!pointer) {
    return false;
  }
  const auto locations = resolveWriteLocations(pointer);
  bool changed = false;
  for (auto &entry : state) {
    bool mayAlias = false;
    for (const HandleLocation &location : locations) {
      if (locationsMayAlias(entry.first, location)) {
        mayAlias = true;
        break;
      }
    }
    if (!mayAlias && locations.empty() && m_aliasAnalysis && entry.first.base &&
        entry.first.base->getType()->isPointerTy() &&
        pointer->getType()->isPointerTy()) {
      mayAlias = m_aliasAnalysis->mayAlias(pointer, entry.first.base);
    }
    if (!mayAlias) {
      continue;
    }
    const HandleState before = entry.second;
    entry.second.path_sensitivity_lost = true;
    transitionHandleState(entry.second, terminal, true);
    changed |= !(before == entry.second);
  }
  return changed;
}

HandleState JoinTargetAnalysis::getStateForValue(const Value *value,
                                                 const StateMap &state) const {
  HandleState result;
  if (!value) {
    return result;
  }

  auto directIt = state.find(HandleLocation{value, {}, false});
  if (directIt != state.end()) {
    return directIt->second;
  }

  auto locations = resolveReadLocations(value);
  if (!locations.empty()) {
    bool foundAny = false;
    for (const HandleLocation &location : locations) {
      if (location.is_base_wildcard) {
        result.path_sensitivity_lost = true;
      }

      auto it = state.find(location);
      if (it != state.end()) {
        mergeHandleState(result, it->second);
        foundAny = true;
      }

      HandleLocation wildcard = location;
      wildcard.offsets.clear();
      wildcard.is_base_wildcard = true;
      if (!(wildcard == location)) {
        auto wildcardIt = state.find(wildcard);
        if (wildcardIt != state.end()) {
          mergeHandleState(result, wildcardIt->second);
          foundAny = true;
        }
      }

      if (location.is_base_wildcard) {
        for (const auto &entry : state) {
          if (entry.first.base == location.base && !entry.first.is_base_wildcard) {
            mergeHandleState(result, entry.second);
            foundAny = true;
          }
        }
      }
    }
    if (foundAny) {
      return result;
    }
  }

  const Value *stripped = value->stripPointerCasts();
  if (isa<ConstantPointerNull>(stripped)) {
    return result;
  }

  if (const auto *arg = dyn_cast<Argument>(stripped)) {
    auto it = state.find(HandleLocation{arg, {}, false});
    if (it != state.end()) {
      return it->second;
    }
  }

  if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped) ||
      isa<Argument>(stripped)) {
    result.provenance_roots.insert(stripped);
    return result;
  }

  if (value->getType()->isPointerTy()) {
    result.has_unknown_live_fork = true;
  }
  if (const Value *root = traceThreadHandleRoot(value, &m_module)) {
    result.provenance_roots.insert(root);
  }
  return result;
}

bool JoinTargetAnalysis::mergeStateInto(StateMap &dst, const StateMap &src) const {
  bool changed = false;
  for (const auto &entry : src) {
    changed |= mergeHandleState(dst[entry.first], entry.second);
  }
  return changed;
}

bool JoinTargetAnalysis::mergeHandleState(HandleState &dst,
                                          const HandleState &src) const {
  bool changed = false;
  for (const auto &entry : src.instances) {
    auto [it, inserted] = dst.instances.emplace(entry.first, entry.second);
    if (inserted) {
      changed = true;
    } else {
      changed |= mergeInstanceState(it->second, entry.second);
    }
  }
  for (unsigned arg : src.preserved_arg_inputs) {
    changed |= dst.preserved_arg_inputs.insert(arg).second;
  }
  for (const Value *root : src.provenance_roots) {
    changed |= dst.provenance_roots.insert(root).second;
  }
  if (src.has_unknown_live_fork && !dst.has_unknown_live_fork) {
    dst.has_unknown_live_fork = true;
    changed = true;
  }
  if (src.path_sensitivity_lost && !dst.path_sensitivity_lost) {
    dst.path_sensitivity_lost = true;
    changed = true;
  }
  return changed;
}

} // namespace mhp
