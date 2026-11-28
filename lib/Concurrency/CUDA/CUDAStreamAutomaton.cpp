#include "Concurrency/CUDA/CUDAStreamAutomaton.h"

#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/CUDA/CUDASemantics.h"

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>

namespace concurrency::cuda {

namespace {

bool isNullStream(const llvm::Value *stream) {
  if (!stream) {
    return true;
  }
  if (const auto *cv = llvm::dyn_cast<llvm::Constant>(stream)) {
    return cv->isNullValue();
  }
  if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(stream)) {
    if (ce->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(ce->getOperand(0))) {
        return ci->isZero();
      }
    }
  }
  return false;
}

bool mustExecuteInstruction(const llvm::Instruction *inst) {
  if (!inst || !inst->getFunction() || inst->getFunction()->empty()) {
    return false;
  }
  llvm::PostDominatorTree post_dom_tree;
  post_dom_tree.recalculate(*const_cast<llvm::Function *>(inst->getFunction()));
  const llvm::BasicBlock *entry = &inst->getFunction()->getEntryBlock();
  return post_dom_tree.getNode(inst->getParent()) &&
         post_dom_tree.getNode(entry) &&
         post_dom_tree.dominates(inst->getParent(), entry);
}

bool identityPrecedesLoad(const llvm::Value *identity,
                          const llvm::LoadInst *load) {
  const auto *create = llvm::dyn_cast_or_null<llvm::Instruction>(identity);
  if (!create || !load || create->getFunction() != load->getFunction()) {
    return true;
  }
  llvm::DominatorTree dom_tree(
      *const_cast<llvm::Function *>(load->getFunction()));
  if (create->getParent() == load->getParent()) {
    return create->comesBefore(load);
  }
  return dom_tree.dominates(create, load);
}

} // anonymous namespace

CUDASteamAutomatonBuilder::CUDASteamAutomatonBuilder(CUDAAbstractState &state)
    : m_state(state) {}

void CUDASteamAutomatonBuilder::recordOutputAliases(
    const llvm::Value *output_slot, const llvm::Value *identity,
    std::map<const llvm::Value *, const llvm::Value *> &aliases) {
  if (!output_slot) {
    return;
  }
  const llvm::Value *slot = output_slot->stripPointerCasts();
  const llvm::Value *canonical = identity ? identity : slot;
  aliases[output_slot] = canonical;
  aliases[slot] = canonical;
  for (const llvm::User *user : slot->users()) {
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
      if (identityPrecedesLoad(identity, load)) {
        aliases[load] = canonical;
      }
    } else if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(user)) {
      aliases[cast] = canonical;
      for (const llvm::User *cast_user : cast->users()) {
        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(cast_user)) {
          if (identityPrecedesLoad(identity, load)) {
            aliases[load] = canonical;
          }
        }
      }
    }
  }
}

const llvm::Value *
CUDASteamAutomatonBuilder::canonicalizeStream(const llvm::Value *stream) const {
  if (!stream) {
    return nullptr;
  }
  auto it = m_stream_aliases.find(stream);
  if (it != m_stream_aliases.end()) {
    return it->second;
  }
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(stream)) {
    const llvm::Value *slot = load->getPointerOperand()->stripPointerCasts();
    it = m_stream_aliases.find(slot);
    if (it != m_stream_aliases.end()) {
      return it->second;
    }
  }
  return stream;
}

const llvm::Value *
CUDASteamAutomatonBuilder::canonicalizeEvent(const llvm::Value *event) const {
  if (!event) {
    return nullptr;
  }
  auto it = m_event_aliases.find(event);
  if (it != m_event_aliases.end()) {
    return it->second;
  }
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(event)) {
    const llvm::Value *slot = load->getPointerOperand()->stripPointerCasts();
    it = m_event_aliases.find(slot);
    if (it != m_event_aliases.end()) {
      return it->second;
    }
  }
  return event;
}

void CUDASteamAutomatonBuilder::addStreamCreate(
    const llvm::Instruction *create_inst, const llvm::Value *output_slot) {
  if (!create_inst || !output_slot) {
    return;
  }
  recordOutputAliases(output_slot, create_inst, m_stream_aliases);
  const llvm::Value *stream = canonicalizeStream(output_slot);
  addStream(stream);
  for (auto &pair : m_stream_automata) {
    if (pair.second.stream != stream) {
      continue;
    }
    CUDAStreamTransition transition;
    transition.inst = create_inst;
    transition.from_state = StreamState::Unknown;
    transition.to_state = StreamState::Created;
    pair.second.transitions.push_back(transition);
    if (!mustExecuteInstruction(create_inst)) {
      pair.second.current_state = StreamState::Unknown;
    }
  }
}

void CUDASteamAutomatonBuilder::addStream(const llvm::Value *stream) {
  stream = canonicalizeStream(stream);
  if (!stream || m_seen_streams.count(stream)) {
    return;
  }
  m_seen_streams.insert(stream);

  size_t class_id = m_stream_automata.size();
  CUDAStreamAutomaton &automaton = m_stream_automata[class_id];
  automaton.stream_class_id = class_id;
  automaton.stream = stream;
  automaton.current_state = StreamState::Created;
  automaton.is_null_stream = isNullStream(stream);
}

void CUDASteamAutomatonBuilder::addStreamOperation(
    const llvm::Instruction *operation_inst, const llvm::Value *stream) {
  if (!operation_inst) {
    return;
  }
  stream = canonicalizeStream(stream);
  if (stream) {
    addStream(stream);
  } else if (!m_seen_streams.count(nullptr)) {
    m_seen_streams.insert(nullptr);
    size_t class_id = m_stream_automata.size();
    CUDAStreamAutomaton &automaton = m_stream_automata[class_id];
    automaton.stream_class_id = class_id;
    automaton.current_state = StreamState::Created;
    automaton.is_null_stream = true;
  }
  for (auto &pair : m_stream_automata) {
    if (pair.second.stream != stream) {
      continue;
    }
    CUDAStreamTransition transition;
    transition.inst = operation_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Active;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = mustExecuteInstruction(operation_inst)
                                    ? StreamState::Active
                                    : StreamState::Unknown;
    pair.second.pending_operations.push_back(operation_inst);
  }
}

void CUDASteamAutomatonBuilder::addEventCreate(
    const llvm::Instruction *create_inst, const llvm::Value *output_slot) {
  if (!create_inst || !output_slot) {
    return;
  }
  recordOutputAliases(output_slot, create_inst, m_event_aliases);
  const llvm::Value *event = canonicalizeEvent(output_slot);
  addEventObject(event);
  for (auto &pair : m_event_automata) {
    if (pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = create_inst;
    transition.from_state = EventState::Unknown;
    transition.to_state = EventState::Created;
    pair.second.transitions.push_back(transition);
    if (!mustExecuteInstruction(create_inst)) {
      pair.second.current_state = EventState::Unknown;
    }
  }
}

void CUDASteamAutomatonBuilder::addEventObject(const llvm::Value *event) {
  event = canonicalizeEvent(event);
  if (!event || m_seen_events.count(event)) {
    return;
  }
  m_seen_events.insert(event);

  size_t class_id = m_event_automata.size();
  CUDAEventAutomaton &automaton = m_event_automata[class_id];
  automaton.event_class_id = class_id;
  automaton.event = event;
  automaton.current_state = EventState::Created;
}

void CUDASteamAutomatonBuilder::addEvent(const llvm::Instruction *record_inst,
                                         const llvm::Value *event,
                                         const llvm::Value *stream) {
  if (!record_inst) {
    return;
  }

  event = canonicalizeEvent(event);
  stream = canonicalizeStream(stream);

  if (event) {
    addEventObject(event);
  }
  if (stream) {
    addStream(stream);
  } else {
    if (!m_seen_streams.count(nullptr)) {
      m_seen_streams.insert(nullptr);
      size_t class_id = m_stream_automata.size();
      CUDAStreamAutomaton &automaton = m_stream_automata[class_id];
      automaton.stream_class_id = class_id;
      automaton.stream = nullptr;
      automaton.current_state = StreamState::Created;
      automaton.is_null_stream = true;
    }
  }

  addStreamOperation(record_inst, stream);

  if (!event) {
    return;
  }
  for (auto &pair : m_event_automata) {
    if (pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = record_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Recorded;
    transition.is_ordering_boundary = false;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = mustExecuteInstruction(record_inst)
                                    ? EventState::Recorded
                                    : EventState::Unknown;
    pair.second.recorded_stream = stream;
    pair.second.has_record = true;
  }
}

void CUDASteamAutomatonBuilder::addEventWait(const llvm::Instruction *wait_inst,
                                             const llvm::Value *event,
                                             const llvm::Value *stream) {
  if (!wait_inst) {
    return;
  }

  event = canonicalizeEvent(event);
  stream = canonicalizeStream(stream);

  if (event) {
    addEventObject(event);
  }
  if (stream) {
    addStream(stream);
  } else if (!m_seen_streams.count(nullptr)) {
    m_seen_streams.insert(nullptr);
    size_t class_id = m_stream_automata.size();
    CUDAStreamAutomaton &automaton = m_stream_automata[class_id];
    automaton.stream_class_id = class_id;
    automaton.current_state = StreamState::Created;
    automaton.is_null_stream = true;
  }

  for (auto &pair : m_stream_automata) {
    if (pair.second.stream != stream) {
      continue;
    }
    CUDAStreamTransition transition;
    transition.inst = wait_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Active;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = mustExecuteInstruction(wait_inst)
                                    ? StreamState::Active
                                    : StreamState::Unknown;
    pair.second.pending_operations.push_back(wait_inst);
  }

  if (!event) {
    return;
  }
  for (auto &pair : m_event_automata) {
    if (pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = wait_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Waited;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = mustExecuteInstruction(wait_inst)
                                    ? EventState::Waited
                                    : EventState::Unknown;
    pair.second.pending_waits.push_back(wait_inst);
    pair.second.has_wait = true;
  }
}

void CUDASteamAutomatonBuilder::addEventSync(const llvm::Instruction *sync_inst,
                                             const llvm::Value *event) {
  if (!sync_inst) {
    return;
  }
  event = canonicalizeEvent(event);
  if (event) {
    addEventObject(event);
  }

  if (!event) {
    return;
  }
  for (auto &pair : m_event_automata) {
    if (pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = sync_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Synchronized;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    if (mustExecuteInstruction(sync_inst)) {
      pair.second.current_state = EventState::Synchronized;
      pair.second.pending_waits.clear();
    } else {
      pair.second.current_state = EventState::Unknown;
    }
  }
}

void CUDASteamAutomatonBuilder::addEventDestroy(
    const llvm::Instruction *destroy_inst, const llvm::Value *event) {
  if (!destroy_inst) {
    return;
  }
  event = canonicalizeEvent(event);
  if (event) {
    addEventObject(event);
  }
  for (auto &pair : m_event_automata) {
    if (pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = destroy_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Destroyed;
    transition.is_ordering_boundary = false;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = mustExecuteInstruction(destroy_inst)
                                    ? EventState::Destroyed
                                    : EventState::Unknown;
  }
}

void CUDASteamAutomatonBuilder::addStreamSync(
    const llvm::Instruction *sync_inst, const llvm::Value *stream) {
  if (!sync_inst) {
    return;
  }

  stream = canonicalizeStream(stream);

  if (stream) {
    addStream(stream);
  }

  for (auto &pair : m_stream_automata) {
    if (pair.second.stream == stream) {
      CUDAStreamTransition transition;
      transition.inst = sync_inst;
      transition.from_state = pair.second.current_state;
      transition.to_state = StreamState::Synchronized;
      transition.is_ordering_boundary = true;
      pair.second.transitions.push_back(transition);
      if (mustExecuteInstruction(sync_inst)) {
        pair.second.current_state = StreamState::Synchronized;
        pair.second.is_ordered = true;
        pair.second.pending_operations.clear();
      } else {
        pair.second.current_state = StreamState::Unknown;
        pair.second.is_ordered = false;
      }
    }
  }
}

void CUDASteamAutomatonBuilder::addStreamDestroy(
    const llvm::Instruction *destroy_inst, const llvm::Value *stream) {
  if (!destroy_inst) {
    return;
  }
  stream = canonicalizeStream(stream);
  if (stream) {
    addStream(stream);
  }

  for (auto &pair : m_stream_automata) {
    if (pair.second.stream != stream) {
      continue;
    }
    CUDAStreamTransition transition;
    transition.inst = destroy_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Destroyed;
    transition.is_ordering_boundary = false;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = mustExecuteInstruction(destroy_inst)
                                    ? StreamState::Destroyed
                                    : StreamState::Unknown;
  }
}

void CUDASteamAutomatonBuilder::addDeviceSync(
    const llvm::Instruction *sync_inst) {
  if (!sync_inst) {
    return;
  }

  for (auto &pair : m_stream_automata) {
    CUDAStreamTransition transition;
    transition.inst = sync_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Synchronized;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    if (mustExecuteInstruction(sync_inst)) {
      pair.second.current_state = StreamState::Synchronized;
      pair.second.is_ordered = true;
      pair.second.pending_operations.clear();
    } else {
      pair.second.current_state = StreamState::Unknown;
      pair.second.is_ordered = false;
    }
  }
}

void CUDASteamAutomatonBuilder::finalize() {
  for (auto &pair : m_stream_automata) {
    pair.second.is_exact = pair.second.current_state != StreamState::Unknown;
    m_state.stream_automata.push_back(pair.second);
    m_state.stream_automaton_by_class[pair.first] = pair.second;
  }

  for (auto &pair : m_event_automata) {
    pair.second.is_exact = pair.second.current_state != EventState::Unknown;
    m_state.event_automata.push_back(pair.second);
    m_state.event_automaton_by_class[pair.first] = pair.second;
  }
}

} // namespace concurrency::cuda
