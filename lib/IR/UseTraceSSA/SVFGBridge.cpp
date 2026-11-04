#include "IR/UseTraceSSA/SVFGBridge.h"

#include "IR/UseTraceSSA/InstructionLabels.h"
#include "IR/UseTraceSSA/TemporalHistory.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace usetracessa {
namespace {

using NativeNode = analysis::SVFGNode;
using NativeEdge = analysis::SVFGEdge;

class PhaseTimer {
  using Clock = std::chrono::steady_clock;
  const NativeHistoryOptions &Options;
  Clock::time_point Start = Clock::now();

public:
  explicit PhaseTimer(const NativeHistoryOptions &options) : Options(options) {}
  void finish(const char *name) {
    if (!Options.phaseTiming)
      return;
    auto end = Clock::now();
    Options.phaseTiming(
        name, std::chrono::duration<double, std::milli>(end - Start).count());
    Start = Clock::now();
  }
};

struct Placement {
  BlockID block = InvalidID;
  EdgeID edge = InvalidID;
  std::size_t position = 0;
};

struct Layout {
  FunctionLayout function;
  std::map<const llvm::BasicBlock *, BlockID> blocks;
  std::vector<const llvm::BasicBlock *> nativeBlocks;
  std::map<const llvm::Instruction *, SiteID> before, returned, after;
  std::map<const llvm::BasicBlock *, SiteID> phi;
  std::map<std::pair<const llvm::BasicBlock *, unsigned>, EdgeID> edges;
  std::map<SiteID, Placement> placements;
  SiteID exit = InvalidID;
};

const llvm::Function *owner(const NativeNode &node) {
  if (const auto *function = node.getFunction()) return function;
  if (const auto *instruction = node.getInstruction()) return instruction->getFunction();
  if (const auto *call = node.getCallSite()) return call->getFunction();
  if (const auto *value = llvm::dyn_cast_or_null<llvm::Instruction>(node.getValue()))
    return value->getFunction();
  return nullptr;
}

const llvm::Instruction *anchor(const NativeNode &node) {
  if (const auto *instruction = node.getInstruction()) return instruction;
  if (const auto *load = llvm::dyn_cast<analysis::LoadMuSVFGNode>(&node))
    return load->getLoadInst();
  if (const auto *store = llvm::dyn_cast<analysis::StoreChiSVFGNode>(&node))
    return store->getStoreInst();
  if (const auto *call = node.getCallSite()) return call;
  return llvm::dyn_cast_or_null<llvm::Instruction>(node.getValue());
}

bool expectedEntry(const NativeNode &node) {
  using analysis::SVFGK;
  switch (node.getNodeKind()) {
  case SVFGK::FormalParm:
  case SVFGK::FormalIn:
  case SVFGK::EntryChi:
  case SVFGK::VarArg:
  case SVFGK::NullPtr:
    return true;
  default:
    return llvm::isa_and_nonnull<llvm::GlobalValue>(node.getValue());
  }
}

Layout makeLayout(const llvm::Function &function, FunctionID id,
                  const detail::InstructionLabels &labels) {
  Layout layout;
  layout.function.id = id;
  layout.function.name = function.getName().str();
  Program &program = layout.function.control;
  auto addBlockSite = [&](BlockID block, std::string label) {
    SiteID site = program.addOperation(block, std::move(label));
    layout.placements.emplace(site, Placement{block, InvalidID,
                                      program.blocks()[block].operations.size() - 1});
    return site;
  };
  for (const auto &block : function) {
    layout.blocks.emplace(&block, program.addBlock(block.getName().str()));
    layout.nativeBlocks.push_back(&block);
  }
  for (const auto &block : function) {
    const auto *terminator = block.getTerminator();
    for (unsigned index = 0; index < terminator->getNumSuccessors(); ++index) {
      const auto *successor = terminator->getSuccessor(index);
      EdgeID edge = program.addEdge(layout.blocks.at(&block),
                                    layout.blocks.at(successor),
                                    "successor " + std::to_string(index));
      layout.edges.emplace(std::make_pair(&block, index), edge);
    }
  }
  BlockID exit = program.addBlock("usetracessa.exit");
  for (const auto &block : function)
    if (llvm::isa<llvm::ReturnInst>(block.getTerminator()))
      program.addEdge(layout.blocks.at(&block), exit, "return");
  for (const auto &block : function) {
    BlockID current = layout.blocks.at(&block);
    layout.phi.emplace(&block, addBlockSite(current, "phi definitions"));
    for (const auto &instruction : block) {
      if (llvm::isa<llvm::PHINode>(instruction)) continue;
      const std::string &description = labels.get(instruction);
      layout.before.emplace(&instruction,
                            addBlockSite(current, "before " + description));
      if (llvm::isa<llvm::CallBase>(instruction))
        layout.returned.emplace(&instruction,
                                addBlockSite(current, "returned " + description));
      layout.after.emplace(&instruction,
                           addBlockSite(current, "after " + description));
    }
  }
  layout.exit = addBlockSite(exit, "function exit");
  return layout;
}

struct ResourceProperty {
  Event events;
  Event sources;
  Event required;
  bool exits;
};

ResourceProperty resourceProperty(NativeHistoryMode mode) {
  const Event heap = Event::Allocate | Event::Release;
  switch (mode) {
  case NativeHistoryMode::DoubleFree: return {heap, heap, Event::Release, false};
  case NativeHistoryMode::UseAfterFree:
    return {heap | Event::Dereference, heap, Event::Release, false};
  case NativeHistoryMode::MemoryLeak:
    return {heap | Event::Escape, Event::Allocate, Event::Allocate, true};
  case NativeHistoryMode::FileLeak:
    return {Event::Open | Event::Close | Event::Escape, Event::Open, Event::Open, true};
  case NativeHistoryMode::Full:
    return {heap | Event::Dereference | Event::Open | Event::Close,
            Event::None, Event::None, true};
  }
  throw std::invalid_argument("UseTraceSSA: invalid native history mode");
}

Event resourceEvent(const llvm::Instruction &instruction, const ResourceProperty &property) {
  if (hasEvent(property.events, Event::Escape))
    if (const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
      if (ret->getReturnValue() && ret->getReturnValue()->getType()->isPointerTy())
        return Event::Escape;
  if (hasEvent(property.events, Event::Dereference) &&
      (llvm::isa<llvm::LoadInst>(instruction) ||
       llvm::isa<llvm::StoreInst>(instruction)))
    return Event::Dereference;
  const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
  const auto *callee = call ? call->getCalledFunction() : nullptr;
  if (!callee) return Event::None;
  auto name = callee->getName();
  Event event = Event::None;
  if (name == "free" && call->arg_size()) event = Event::Release;
  if ((name == "malloc" || name == "calloc") && call->getType()->isPointerTy())
    event = Event::Allocate;
  if (name == "fopen" && call->getType()->isPointerTy()) event = Event::Open;
  if (name == "fclose" && call->arg_size()) event = Event::Close;
  return hasEvent(property.events, event) ? event : Event::None;
}

bool structuralCall(const llvm::Instruction &instruction) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
  if (!call) return false;
  const auto *callee = call->getCalledFunction();
  // Internal calls retain their control/return behavior even when the callee
  // has no resource event. Unknown calls retain a conservative placeholder.
  if (!callee || !callee->isDeclaration()) return true;
  if (callee->isIntrinsic()) return false;
  auto name = callee->getName();
  return name != "free" && name != "malloc" && name != "calloc" &&
         name != "fopen" && name != "fclose";
}

struct ResourcePlan {
  ResourceProperty property;
  bool hasRequired = false;
  ObjectSet universe = ObjectSet::unknown();
  std::map<const llvm::Instruction *, GuardedEventEffect> effects = {};
  std::vector<const llvm::CallBase *> calls = {};
  std::vector<std::string> issues = {};
  bool keeps(const llvm::Instruction &instruction) const {
    return effects.count(&instruction) || structuralCall(instruction);
  }
};

Layout makeResourceLayout(const llvm::Function &function, FunctionID id,
                          const ResourcePlan &plan, const detail::InstructionLabels &labels) {
  Layout layout;
  layout.function.id = id;
  layout.function.name = function.getName().str();
  Program &program = layout.function.control;
  // Contract empty single-successor blocks before expanding CFG edges for SSA.
  // Keep all event/call sites, branches, terminal blocks and the external
  // entry. An all-empty cycle gets a representative self-loop, never a return
  // port.
  std::vector<const llvm::BasicBlock *> blocks;
  llvm::DenseMap<const llvm::BasicBlock *, ID> numbers;
  for (const auto &block : function) {
    numbers[&block] = blocks.size();
    blocks.push_back(&block);
  }
  std::vector<BlockID> resolved(blocks.size(), InvalidID);
  std::vector<bool> visiting(blocks.size());
  auto retain = [&](ID index) {
    const auto *block = blocks[index];
    auto id = program.addBlock(block->getName().str());
    layout.blocks.emplace(block, id);
    layout.nativeBlocks.push_back(block);
    resolved[index] = id;
  };
  for (ID index = 0; index < blocks.size(); ++index) {
    const auto &block = *blocks[index];
    bool keep = index == 0 || block.getTerminator()->getNumSuccessors() != 1;
    for (const auto &instruction : block)
      keep |= plan.keeps(instruction);
    if (keep)
      retain(index);
  }
  for (ID index = 0; index < blocks.size(); ++index) {
    if (resolved[index] != InvalidID)
      continue;
    std::vector<ID> path;
    ID current = index;
    while (resolved[current] == InvalidID && !visiting[current]) {
      visiting[current] = true;
      path.push_back(current);
      current =
          numbers.lookup(blocks[current]->getTerminator()->getSuccessor(0));
    }
    if (resolved[current] == InvalidID)
      retain(current);
    for (auto member : path) {
      resolved[member] = resolved[current];
      visiting[member] = false;
    }
  }
  for (const auto *block : layout.nativeBlocks) {
    const auto *terminator = block->getTerminator();
    for (unsigned successor = 0; successor < terminator->getNumSuccessors(); ++successor)
      program.addEdge(
          layout.blocks.at(block),
          resolved[numbers.lookup(terminator->getSuccessor(successor))]);
  }
  for (const auto &block : function) {
    auto retained = layout.blocks.find(&block);
    if (retained == layout.blocks.end())
      continue;
    BlockID current = retained->second;
    for (const auto &instruction : block) {
      if (!plan.keeps(instruction)) continue;
      bool dereference = llvm::isa<llvm::LoadInst>(instruction) ||
                         llvm::isa<llvm::StoreInst>(instruction);
      SiteID returned = InvalidID;
      if (structuralCall(instruction) && plan.effects.count(&instruction))
        returned = program.addOperation(current, "returned " + labels.get(instruction));
      SiteID site = program.addOperation(current,
                           std::string(dereference ? "before " : "after ") +
                           labels.get(instruction));
      if (dereference) layout.before.emplace(&instruction, site);
      else {
        layout.after.emplace(&instruction, site);
        layout.returned.emplace(&instruction, returned == InvalidID ? site : returned);
      }
    }
  }
  return layout;
}

SiteID definitionSite(const NativeNode &node, const Layout &layout) {
  using analysis::SVFGK;
  switch (node.getNodeKind()) {
  case SVFGK::FormalParm:
  case SVFGK::FormalIn:
  case SVFGK::EntryChi:
  case SVFGK::VarArg:
    return InvalidID;
  case SVFGK::FormalOut:
  case SVFGK::FormalRet:
  case SVFGK::RetMu:
    return layout.exit;
  default:
    break;
  }
  if (const auto *instruction = anchor(node)) {
    if (llvm::isa<llvm::PHINode>(instruction))
      return layout.phi.at(instruction->getParent());
    bool before = node.getNodeKind() == SVFGK::LoadMu ||
                  node.getNodeKind() == SVFGK::ActualIn ||
                  node.getNodeKind() == SVFGK::ActualParm ||
                  node.getNodeKind() == SVFGK::CallMu;
    if (node.getNodeKind() == SVFGK::ActualRet ||
        node.getNodeKind() == SVFGK::ActualOut ||
        node.getNodeKind() == SVFGK::CallChi)
      return layout.returned.at(instruction);
    return before ? layout.before.at(instruction) : layout.after.at(instruction);
  }
  if (const auto *icfg = node.getICFGNode()) {
    if (const auto *block = icfg->getBasicBlock()) {
      auto found = layout.phi.find(block);
      if (found != layout.phi.end()) return found->second;
    }
  }
  // Constants and global addresses have no instruction definition.
  return InvalidID;
}

std::vector<SiteID> phiConsumers(const NativeNode &from, const NativeNode &to,
                                  Layout &layout) {
  const auto *phi = llvm::dyn_cast_or_null<llvm::PHINode>(to.getInstruction());
  if (!phi) return {};
  std::vector<SiteID> result;
  for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
    if (phi->getIncomingValue(index) != from.getValue()) continue;
    const auto *predecessor = phi->getIncomingBlock(index);
    unsigned occurrence = 0;
    for (unsigned earlier = 0; earlier < index; ++earlier)
      if (phi->getIncomingBlock(earlier) == predecessor) ++occurrence;
    unsigned seen = 0;
    const auto *terminator = predecessor->getTerminator();
    for (unsigned successor = 0; successor < terminator->getNumSuccessors(); ++successor) {
      if (terminator->getSuccessor(successor) != phi->getParent()) continue;
      if (seen++ != occurrence) continue;
      EdgeID edge = layout.edges.at({predecessor, successor});
      SiteID site = layout.function.control.addEdgeOperation(edge, "phi input");
      layout.placements.emplace(site, Placement{InvalidID, edge,
                             layout.function.control.edges()[edge].operations.size() - 1});
      result.push_back(site);
      break;
    }
  }
  return result;
}

std::vector<SiteID> memoryPhiConsumers(const NativeNode &from,
                                        const NativeNode &to, Layout &layout) {
  const auto *phi = llvm::dyn_cast<analysis::MSSAPhiSVFGNode>(&to);
  const auto *icfg = to.getICFGNode();
  const auto *block = icfg ? icfg->getBasicBlock() : nullptr;
  if (!phi || !block) return {};
  std::vector<SiteID> result;
  std::vector<std::pair<const llvm::BasicBlock *, unsigned>> incoming;
  for (const auto *predecessor : llvm::predecessors(block)) {
    const auto *terminator = predecessor->getTerminator();
    for (unsigned successor = 0; successor < terminator->getNumSuccessors(); ++successor)
      if (terminator->getSuccessor(successor) == block)
        incoming.emplace_back(predecessor, successor);
  }
  for (auto it = phi->opVerBegin(); it != phi->opVerEnd(); ++it) {
    if (it->second.memReg != from.getMemReg() ||
        it->second.version != from.getSSAVersion() || it->first >= incoming.size())
      continue;
    EdgeID edge = layout.edges.at(incoming[it->first]);
    SiteID site = layout.function.control.addEdgeOperation(edge, "memory phi input");
    layout.placements.emplace(site, Placement{InvalidID, edge,
                           layout.function.control.edges()[edge].operations.size() - 1});
    result.push_back(site);
  }
  return result;
}

bool dominatesUse(const Layout &layout, const llvm::DominatorTree &dominators,
                  SiteID definition, SiteID consumer) {
  if (definition == InvalidID) return true;
  Placement from = layout.placements.at(definition);
  Placement to = layout.placements.at(consumer);
  if (from.edge != InvalidID)
    return from.edge == to.edge && from.position < to.position;
  if (to.edge != InvalidID) {
    BlockID predecessor = layout.function.control.edges()[to.edge].from;
    if (from.block == predecessor) return true;
    to.block = predecessor;
  } else if (from.block == to.block) {
    return from.position < to.position;
  }
  const llvm::BasicBlock *source = from.block < layout.nativeBlocks.size() ?
                                     layout.nativeBlocks[from.block] : nullptr;
  const llvm::BasicBlock *target = to.block < layout.nativeBlocks.size() ?
                                     layout.nativeBlocks[to.block] : nullptr;
  return source && target && dominators.dominates(source, target);
}

ObjectSet resourceObjects(const analysis::SVFG &svfg, const llvm::Value *pointer) {
  const auto &pointsTo = svfg.getObjectIds(pointer);
  if (pointsTo.empty()) return ObjectSet::unknown();
  std::vector<ObjectID> objects;
  for (auto object : pointsTo) {
    if (svfg.isUnknownObject(object)) return ObjectSet::unknown();
    objects.push_back(object);
  }
  return ObjectSet::known(std::move(objects));
}

ResourcePlan planResources(const analysis::SVFG &svfg, const llvm::Module &module,
                           NativeHistoryMode mode) {
  ResourcePlan plan{resourceProperty(mode)};
  plan.hasRequired = mode == NativeHistoryMode::Full;
  std::set<const llvm::Function *> called;
  std::map<const llvm::Instruction *, ObjectID> synthetic;
  ObjectID nextSynthetic = ObjectID(1) << 63;
  std::vector<std::pair<const llvm::Instruction *, Event>> eventSites;
  for (const auto &function : module) for (const auto &block : function)
    for (const auto &instruction : block) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      const auto *callee = call ? call->getCalledFunction() : nullptr;
      if (call) {
        plan.calls.push_back(call);
        if (!callee || (callee->isDeclaration() && !callee->isIntrinsic()))
          plan.issues.push_back("call in " + function.getName().str() +
                                " has no complete native effect model");
      }
      if (callee && !callee->isDeclaration()) called.insert(callee);
      auto event = resourceEvent(instruction, plan.property);
      if (event != Event::None)
        eventSites.emplace_back(&instruction, event);
      if (mode != NativeHistoryMode::Full && plan.property.exits &&
          hasEvent(event, plan.property.sources))
        synthetic.emplace(&instruction, nextSynthetic++);
    }
  auto directAcquisition = [](const llvm::Value *pointer, llvm::StringRef name) {
    const auto *call = llvm::dyn_cast<llvm::CallBase>(pointer->stripPointerCasts());
    const auto *callee = call ? call->getCalledFunction() : nullptr;
    return callee && callee->getName() == name;
  };
  std::unordered_map<const llvm::Value *, ObjectSet> objectGuards;
  std::unordered_map<const llvm::Value *, ObjectSet> projectedGuards;
  auto effectFor = [&](const llvm::Instruction &instruction, Event event) {
    const llvm::Value *pointer = nullptr;
    Certainty certainty = Certainty::May;
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
      pointer = load->getPointerOperand();
    else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
      pointer = store->getPointerOperand();
    else if (const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
      pointer = ret->getReturnValue(); certainty = Certainty::Must;
    } else {
      const auto &call = llvm::cast<llvm::CallBase>(instruction);
      pointer = hasEvent(event, Event::Allocate | Event::Open) ? &instruction :
                                                               call.getArgOperand(0);
      if ((mode == NativeHistoryMode::MemoryLeak && event == Event::Release &&
           (directAcquisition(pointer, "malloc") || directAcquisition(pointer, "calloc"))) ||
          (mode == NativeHistoryMode::FileLeak && event == Event::Close &&
           directAcquisition(pointer, "fopen")))
        certainty = Certainty::Must;
    }
    bool project =
        !hasEvent(event, plan.property.sources) && !plan.universe.isUnknown();
    auto &cache = project ? projectedGuards : objectGuards;
    auto cached = cache.find(pointer);
    if (cached == cache.end()) {
      auto raw = objectGuards.find(pointer);
      auto objects = raw == objectGuards.end() ? resourceObjects(svfg, pointer)
                                               : raw->second;
      if (objects.isUnknown()) {
        const auto *site =
            llvm::dyn_cast<llvm::Instruction>(pointer->stripPointerCasts());
        auto fallback = synthetic.find(site);
        if (fallback != synthetic.end())
          objects = ObjectSet::known({fallback->second});
      }
      if (project && !objects.isUnknown()) {
        std::vector<ObjectID> relevant;
        const auto &universe = plan.universe.objects();
        std::set_intersection(objects.objects().begin(),
                              objects.objects().end(), universe.begin(),
                              universe.end(), std::back_inserter(relevant));
        objects = ObjectSet::known(std::move(relevant));
      }
      cached = cache.emplace(pointer, std::move(objects)).first;
    }
    return GuardedEventEffect{event, cached->second, certainty};
  };

  std::vector<ObjectID> candidates;
  bool unknownSources = mode == NativeHistoryMode::Full;
  for (const auto &site : eventSites) {
    const auto &instruction = *site.first;
    auto event = site.second;
    if (!hasEvent(event, plan.property.sources))
      continue;
    auto effect = effectFor(instruction, event);
    plan.hasRequired |=
        hasEvent(event, plan.property.required) && !effect.objects.empty();
    unknownSources |= effect.objects.isUnknown();
    candidates.insert(candidates.end(), effect.objects.objects().begin(),
                      effect.objects.objects().end());
    plan.effects.emplace(&instruction, std::move(effect));
  }
  if (!plan.hasRequired) return plan;
  if (!unknownSources) plan.universe = ObjectSet::known(std::move(candidates));

  for (const auto &site : eventSites) {
    const auto &instruction = *site.first;
    const auto &function = *instruction.getFunction();
    auto event = site.second;
    if (event == Event::None || hasEvent(event, plan.property.sources))
      continue;
    if (event == Event::Escape &&
        (called.count(&function) || function.hasAddressTaken()))
      continue;
    auto effect = effectFor(instruction, event);
    if (!effect.objects.empty())
      plan.effects.emplace(&instruction, std::move(effect));
  }
  return plan;
}

void appendTemporalFacts(
    SVFGHistoryResult &result, const analysis::SVFG &svfg,
    const std::map<const llvm::Function *, Layout> &layouts,
    FunctionID firstLayer, const ResourcePlan &plan,
    const NativeHistoryOptions &options) {
  PhaseTimer timer(options);
  std::map<const llvm::Function *, std::vector<TemporalEffect>> effects;
  // Only event sites consume native provenance. A sparse resource check must
  // not allocate a tree entry for every instruction in a multi-million-node
  // SVFG.
  std::unordered_map<const llvm::Instruction *, NativeID> provenance;
  provenance.reserve(plan.effects.size());
  for (const auto &effect : plan.effects)
    provenance.emplace(effect.first, NoNativeID);
  if (!provenance.empty())
    for (const auto &item : svfg)
      if (const auto *instruction = anchor(*item.second)) {
        auto it = provenance.find(instruction);
        if (it != provenance.end())
          it->second = std::min<NativeID>(it->second, item.second->getId());
      }
  for (const auto &entry : plan.effects) {
    const auto *instruction = entry.first;
    const auto &effect = entry.second;
    const auto &layout = layouts.at(instruction->getFunction());
    SiteID site = effect.events == Event::Dereference ? layout.before.at(instruction) :
                                                       layout.after.at(instruction);
    auto native = provenance.find(instruction);
    effects[instruction->getFunction()].push_back({site, effect.objects, effect.events,
        effect.certainty, native == provenance.end() ? NoNativeID : native->second});
  }
  std::map<const llvm::Function *, TemporalHistory> histories;
  timer.finish("usetracessa_provenance");
  for (const auto &item : layouts) {
    auto history = TemporalHistory::append(
        result.graph, firstLayer++, item.second.function.name + ".temporal",
        item.second.function.control, effects[item.first], false);
    // A CFG sink such as unreachable/resume is not a normal return port.
    // Full layouts also have one explicit synthetic join of normal returns.
    for (auto exit = history.exits.begin(); exit != history.exits.end();) {
      bool normal = item.second.function.control.blocks()[exit->first].name == "usetracessa.exit";
      if (exit->first < item.second.nativeBlocks.size())
        normal = llvm::isa<llvm::ReturnInst>(
            item.second.nativeBlocks[exit->first]->getTerminator());
      if (normal) {
        if (plan.property.exits &&
            result.graph.layer(history.id)
                .history.use(exit->second, history.execution))
          result.graph.annotate(
              result.graph.after(history.id, exit->second, history.execution),
              Event::Exit);
        ++exit;
      } else
        exit = history.exits.erase(exit);
    }
    histories.emplace(item.first, std::move(history));
  }
  for (const auto &effect : plan.effects) {
    const auto *instruction = effect.first;
    const auto &location = instruction->getDebugLoc();
    if (!location)
      continue;
    const auto &layout = layouts.at(instruction->getFunction());
    const auto &history = histories.at(instruction->getFunction());
    SiteID site = effect.second.events == Event::Dereference
                      ? layout.before.at(instruction)
                      : layout.after.at(instruction);
    if (!result.graph.layer(history.id)
             .history.use(history.sites.at(site), history.execution))
      continue;
    result.graph.setSourceLocation(history.after(result.graph, site),
                                   location->getFilename().str() + ':' +
                                       std::to_string(location.getLine()) +
                                       ':' + std::to_string(location.getCol()));
  }
  timer.finish("usetracessa_history");
  for (const auto *call : plan.calls) {
    const auto *callee = call->getCalledFunction();
    if (!callee || !histories.count(callee))
      continue;
    const auto &caller = histories.at(call->getFunction());
    SiteID site = layouts.at(call->getFunction()).returned.at(call);
    if (!result.graph.layer(caller.id).history.use(caller.sites.at(site),
                                                   caller.execution))
      continue; // Unreachable CFG site.
    if (llvm::isa<llvm::InvokeInst>(call)) {
      result.graph.addIssue(
          "exceptional temporal call exits require edge-specific modeling");
      continue;
    }
    TemporalHistory::connectDirectCall(result.graph, caller, site,
                                       reinterpret_cast<std::uintptr_t>(call),
                                       histories.at(callee));
  }
  timer.finish("usetracessa_calls");
}

} // namespace

SVFGHistoryResult buildUseTraceSSAFromLotusSVFG(const analysis::SVFG &svfg,
                                                const llvm::Module &module,
                                                NativeHistoryMode mode,
                                                NativeHistoryOptions options) {
  SVFGConstructionInput input;
  std::map<const llvm::Function *, Layout> layouts;
  PhaseTimer timer(options);
  const auto plan = planResources(svfg, module, mode);
  timer.finish("usetracessa_plan");
  if (!plan.hasRequired) {
    SVFGHistoryResult result;
    result.graph.addIssue("required native source events are absent for the selected property");
    return result;
  }
  FunctionID nextFunction = 1;
  {
    detail::InstructionLabels labels(
        module,
        [&](const llvm::Instruction &instruction) {
          return mode == NativeHistoryMode::Full
                     ? !llvm::isa<llvm::PHINode>(instruction)
                     : plan.keeps(instruction);
        },
        options.instructionText);
    timer.finish("usetracessa_labels");
    for (const auto &function : module) {
      if (function.isDeclaration()) continue;
      if (mode == NativeHistoryMode::Full)
        layouts.emplace(&function, makeLayout(function, nextFunction++, labels));
      else
        layouts.emplace(&function, makeResourceLayout(function, nextFunction++, plan, labels));
    }
  }
  timer.finish("usetracessa_layout");
  std::map<const llvm::Function *, llvm::DominatorTree> dominators;
  if (mode == NativeHistoryMode::Full)
    for (const auto &entry : layouts)
      dominators[entry.first].recalculate(*const_cast<llvm::Function *>(entry.first));
  if (mode != NativeHistoryMode::Full) {
    SVFGHistoryResult result;
    appendTemporalFacts(result, svfg, layouts, 1, plan, options);
    for (const auto &issue : plan.issues)
      result.graph.addIssue(issue);
    if (!plan.universe.isUnknown())
      result.graph.setResourceUniverse(plan.universe.objects());
    return result;
  }
  FunctionLayout globals;
  globals.id = 0;
  globals.name = "module constants";
  globals.control.addBlock("entry");

  std::vector<const NativeNode *> nodes;
  for (const auto &item : svfg) nodes.push_back(item.second);
  std::sort(nodes.begin(), nodes.end(),
            [](const NativeNode *a, const NativeNode *b) { return a->getId() < b->getId(); });
  for (const NativeNode *node : nodes) {
    const llvm::Function *function = owner(*node);
    auto found = layouts.find(function);
    LocatedSVFGNode located;
    located.id = node->getId();
    located.function = found == layouts.end() ? 0 : found->second.function.id;
    located.definitionSite = found == layouts.end() ? InvalidID :
                             definitionSite(*node, found->second);
    located.label = "SVFG " + std::to_string(node->getId());
    if (function && found == layouts.end())
      input.issues.push_back("SVFG node " + std::to_string(node->getId()) +
                             " belongs to a function without a body");
    if (found != layouts.end() && located.definitionSite == InvalidID &&
        !expectedEntry(*node))
      input.issues.push_back("SVFG node " + std::to_string(node->getId()) +
                             " has no precise definition site");
    if (!function && !expectedEntry(*node))
      input.issues.push_back("SVFG node " + std::to_string(node->getId()) +
                             " has no owning function");
    input.nodes.push_back(std::move(located));
  }

  input.issues = plan.issues;

  NativeID nextEdge = 1;
  for (const NativeNode *node : nodes) {
    std::vector<const NativeEdge *> edges(node->getOutEdges().begin(),
                                           node->getOutEdges().end());
    std::sort(edges.begin(), edges.end(), [](const NativeEdge *a, const NativeEdge *b) {
      return std::make_tuple(a->getDstNode()->getId(), a->getEdgeKind()) <
             std::make_tuple(b->getDstNode()->getId(), b->getEdgeKind());
    });
    for (const NativeEdge *edge : edges) {
      const NativeNode &target = *edge->getDstNode();
      const llvm::Function *sourceFunction = owner(*node);
      const llvm::Function *targetFunction = owner(target);
      auto layout = layouts.find(targetFunction);
      std::vector<SiteID> sites;
      if (sourceFunction == targetFunction && layout != layouts.end()) {
        sites = phiConsumers(*node, target, layout->second);
        if (sites.empty()) sites = memoryPhiConsumers(*node, target, layout->second);
        if (sites.empty() &&
            (llvm::isa_and_nonnull<llvm::PHINode>(target.getInstruction()) ||
             llvm::isa<analysis::MSSAPhiSVFGNode>(&target)))
          input.issues.push_back("SVFG edge into phi " +
                                 std::to_string(target.getId()) +
                                 " has no matched incoming CFG edge");
        if (sites.empty()) sites.push_back(definitionSite(target, layout->second));
      } else {
        sites.push_back(InvalidID);
      }
      for (SiteID site : sites) {
        bool boundary = sourceFunction != targetFunction || site == InvalidID;
        bool nonDominating = false;
        if (!boundary &&
            !dominatesUse(layout->second, dominators.at(targetFunction),
                          definitionSite(*node, layout->second), site)) {
          input.issues.push_back("SVFG edge " + std::to_string(nextEdge) +
                                 " has a path-specific non-dominating source");
          boundary = true;
          nonDominating = true;
          site = InvalidID;
        }
        auto located = locateLotusSVFGEdge(*edge, nextEdge++, site, boundary);
        for (auto object : edge->getPointsTo())
          if (svfg.isUnknownObject(object)) located.objects = ObjectSet::unknown();
        if (sourceFunction && targetFunction && sourceFunction != targetFunction &&
            located.kind != FlowKind::Call && located.kind != FlowKind::Return)
          input.issues.push_back("cross-function SVFG edge " +
                                 std::to_string(located.id) +
                                 " has no call/return context");
        if ((located.kind == FlowKind::Call || located.kind == FlowKind::Return) &&
            located.callSite == NoNativeID) {
          const auto *call = node->getCallSite() ? node->getCallSite() : target.getCallSite();
          if (call) located.callSite = reinterpret_cast<std::uintptr_t>(call);
          else {
            located.kind = FlowKind::Summary;
            input.issues.push_back("SVFG edge " + std::to_string(located.id) +
                                   " has no call-site context");
          }
        }
        if (site == InvalidID && sourceFunction == targetFunction && !nonDominating)
          input.issues.push_back("SVFG edge " + std::to_string(located.id) +
                                 " has no local consumer site");
        input.edges.push_back(std::move(located));
      }
    }
  }
  input.functions.push_back(std::move(globals));
  for (const auto &item : layouts) input.functions.push_back(item.second.function);
  timer.finish("usetracessa_native");
  auto result = SVFGImporter::build(input);
  timer.finish("usetracessa_import");
  appendTemporalFacts(result, svfg, layouts, nextFunction, plan, options);
  return result;
}

} // namespace usetracessa
} // namespace lotus
