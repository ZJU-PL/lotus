/*
 * Flow-Sensitive Pointer Analysis for Millions of Lines of Code, CGO'11.
 */

#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"

#include "Alias/InclusionBased/FlowSensitive/Support/SVFGOrder.h"
#include "IR/ICFG/CallGraph.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>
#include <unordered_set>

#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace lotus::analysis;

namespace lotus::alias {

FlowSensitivePTA::FlowSensitivePTA(const SVFG &graph)
    : FlowSensitivePTA(graph, Config{}) {}

FlowSensitivePTA::FlowSensitivePTA(const SVFG &graph, Config config)
    : graph_(&graph), config_(config) {}

bool FlowSensitivePTA::timeLimitExceeded() const {
  return hasDeadline_ && std::chrono::steady_clock::now() >= deadline_;
}

std::string FlowSensitivePTA::statisticsReport() const {
  std::ostringstream out;
  out << "FlowSensitivePTA statistics\n";
  out << "  nodes: " << stats_.nodes << "\n";
  out << "  sccs: " << stats_.sccs << " (max " << stats_.maxSccSize << ")\n";
  out << "  node processes: " << stats_.nodeProcesses << "\n";
  out << "  solve iterations: " << stats_.solveIterations << "\n";
  out << "  processed addr/copy/gep/load/store/phi: " << stats_.processedAddr
      << "/" << stats_.processedCopy << "/" << stats_.processedGep << "/"
      << stats_.processedLoad << "/" << stats_.processedStore << "/"
      << stats_.processedPhi << "\n";
  out << "  processed actual-param/formal-ret/mssa: "
      << stats_.processedActualParam << "/" << stats_.processedFormalRet << "/"
      << stats_.processedMSSA << "\n";
  out << "  topology epochs: " << stats_.topologyEpochs << "\n";
  out << "  top-level facts: " << stats_.topLevelFacts << "\n";
  out << "  top-level pts size max/avg: " << stats_.maxTopLevelPtsSize << "/"
      << stats_.averageTopLevelPtsSize << "\n";
  out << "  pointers/objects: " << stats_.totalPointers << " / "
      << stats_.totalObjects << " (FI " << stats_.fieldInsensitiveObjects
      << ", FS " << stats_.fieldSensitiveObjects << ")\n";
  out << "  pts size max/avg: " << stats_.maxPtsSize << " / "
      << stats_.averagePtsSize << "\n";
  out << "  address-taken vars: " << stats_.addrTakenVars << " (max/avg pts "
      << stats_.maxAddrTakenPtsSize << "/" << stats_.averageAddrTakenPtsSize
      << ")\n";
  out << "  clustered objects: " << stats_.clusteredObjects << " ("
      << stats_.mergedObjectClusters << " merged classes)\n";
  out << "  memory in/out facts: " << stats_.memoryInFacts << " / "
      << stats_.memoryOutFacts << "\n";
  out << "  strong updates: " << stats_.strongUpdates << " (executions "
      << stats_.strongUpdateExecutions << ")\n";
  out << "  weak updates: " << stats_.weakUpdates << " (executions "
      << stats_.weakUpdateExecutions << ")\n";
  out << "  indirect call edges: " << stats_.indirectCallEdges << "\n";
  out << "  indirect calls rejected by bound: " << stats_.indirectCallsRejected
      << "\n";
  if (stats_.unboundedIndirectCalls != 0)
    out << "  WARNING unbounded on-the-fly edges: "
        << stats_.unboundedIndirectCalls
        << " (no indirect-call target filter configured)\n";
  if (stats_.unresolvedIndirectCalls != 0)
    out << "  WARNING unresolved indirect callsites: "
        << stats_.unresolvedIndirectCalls << "\n";
  out << "  dynamic field-insensitive collapses: "
      << stats_.fieldInsensitiveCollapses << "\n";
  out << "  svfgHasSU: " << (stats_.svfgHasSU ? "yes" : "no") << "\n";
  out << "  time: solve " << stats_.solveSeconds << "s, initialize "
      << stats_.initializeSeconds << "s, otf " << stats_.otfSeconds << "s\n";
  out << "  timed out: " << (stats_.timedOut ? "yes" : "no") << "\n";
  return out.str();
}

bool FlowSensitivePTA::writeAnalysisResultToFile(
    const std::string &filename) const {
  std::ofstream output(filename, std::ios::trunc);
  if (!output)
    return false;
  output << "LOTUS_SPARSEPTA 2\n";
  // SVF has no separate field-insensitive object: getFIObjVar returns the
  // base. Lotus allocates a dedicated representative, so record the identity
  // map explicitly instead of rewriting solving IDs.
  for (ObjectID object : graph_->getAllObjectIds()) {
    const ObjectID canonical = graph_->canonicalObjectId(object);
    if (canonical != object)
      output << "M " << object << ' ' << canonical << '\n';
  }
  for (const auto &[node, pointsTo] : topLevelPointsTo_) {
    const PointsToSet &set = materialize(pointsTo);
    output << "T " << node << ' ' << set.size();
    for (ObjectID object : set)
      output << ' ' << object;
    output << '\n';
  }
  auto writeMemory =
      [&](char kind, const std::unordered_map<NodeID, MemoryState> &states) {
        for (const auto &[node, state] : states)
          for (const auto &[object, pointsTo] : state) {
            const PointsToSet &set = materialize(pointsTo);
            output << kind << ' ' << node << ' ' << object << ' ' << set.size();
            for (ObjectID target : set)
              output << ' ' << target;
            output << '\n';
          }
      };
  writeMemory('I', dfIn_);
  writeMemory('O', dfOut_);
  output << "E\n";
  return output.good();
}

bool FlowSensitivePTA::readAnalysisResultFromFile(const std::string &filename) {
  std::ifstream input(filename);
  std::string magic;
  unsigned formatVersion = 0;
  if (!(input >> magic >> formatVersion) || magic != "LOTUS_SPARSEPTA" ||
      (formatVersion != 1 && formatVersion != 2))
    return false;

  topLevelPointsTo_.clear();
  dfIn_.clear();
  dfOut_.clear();
  initialMemory_.clear();
  strongUpdateSites_.clear();
  weakUpdateSites_.clear();
  if (config_.setBackend == PointsToSetBackend::HashConsed)
    arena_.reset();
  loadedCanonicalObjects_.clear();
  stats_ = {};

  auto makeStored = [&](const PointsToSet &set) {
    StoredSet stored;
    if (config_.setBackend == PointsToSetBackend::HashConsed)
      stored.interned = arena_.intern(set);
    else
      stored.mutableSet = set;
    return stored;
  };
  auto readSet = [&](std::size_t count, PointsToSet &set) {
    for (std::size_t i = 0; i < count; ++i) {
      ObjectID value = 0;
      if (!(input >> value))
        return false;
      set.insert(value);
    }
    return true;
  };
  std::string record;
  while (input >> record) {
    if (record == "E")
      break;
    if (record == "T") {
      NodeID node = 0;
      std::size_t count = 0;
      PointsToSet set;
      if (!(input >> node >> count) || !readSet(count, set))
        return false;
      topLevelPointsTo_[node] = makeStored(set);
    } else if (record == "I" || record == "O") {
      NodeID node = 0;
      ObjectID object = 0;
      std::size_t count = 0;
      PointsToSet set;
      if (!(input >> node >> object >> count) || !readSet(count, set))
        return false;
      (record == "I" ? dfIn_ : dfOut_)[node][object] = makeStored(set);
    } else if (record == "M") {
      // Field-insensitive identity record. The solving IDs are unchanged, so
      // the mapping is informational and only queried through
      // canonicalObject().
      ObjectID representative = 0;
      ObjectID base = 0;
      if (!(input >> representative >> base))
        return false;
      loadedCanonicalObjects_[representative] = base;
    } else {
      return false;
    }
  }
  reconnectIndirectCallsAfterRead();
  return input.good() || input.eof();
}

FlowSensitivePTA::ObjectID
FlowSensitivePTA::canonicalObject(ObjectID object) const {
  const auto it = loadedCanonicalObjects_.find(object);
  if (it != loadedCanonicalObjects_.end())
    return it->second;
  return graph_ ? graph_->canonicalObjectId(object) : object;
}

bool FlowSensitivePTA::hasSameSolution(const FlowSensitivePTA &other,
                                       std::string *difference) const {
  return compareSolution(other, false, difference);
}

bool FlowSensitivePTA::compareSolution(const FlowSensitivePTA &other,
                                       bool comparePresence,
                                       std::string *difference) const {
  if (difference)
    difference->clear();
  auto mismatch = [&](std::string message) {
    if (difference)
      *difference = std::move(message);
    return false;
  };
  if (graph_ != other.graph_)
    return mismatch("different graphs");
  for (const auto &entry : *graph_)
    if (pointsTo(entry.second) != other.pointsTo(entry.second))
      return mismatch("top-level points-to set at node " +
                      std::to_string(entry.first));
  auto sameStates = [&](const auto &left, const auto &right,
                        const char *channel) {
    for (const auto &entry : *graph_) {
      const auto a = left.find(entry.first), b = right.find(entry.first);
      static const MemoryState empty;
      const auto &aState = a == left.end() ? empty : a->second;
      const auto &bState = b == right.end() ? empty : b->second;
      if (comparePresence && aState.size() != bState.size())
        return mismatch(std::string(channel) + " entry count at node " +
                        std::to_string(entry.first) + " (" +
                        std::to_string(aState.size()) + " versus " +
                        std::to_string(bState.size()) + ") " +
                        entry.second->toString());
      for (const auto &fact : aState) {
        const auto found = bState.find(fact.first);
        if (found == bState.end()
                ? comparePresence || !materialize(fact.second).empty()
                : materialize(fact.second) != other.materialize(found->second))
          return mismatch(std::string(channel) + " at node " +
                          std::to_string(entry.first) + " object " +
                          std::to_string(fact.first));
      }
      for (const auto &fact : bState)
        if (aState.count(fact.first) == 0 &&
            (comparePresence || !other.materialize(fact.second).empty()))
          return mismatch(std::string(channel) + " at node " +
                          std::to_string(entry.first) + " object " +
                          std::to_string(fact.first));
    }
    return true;
  };
  return sameStates(dfIn_, other.dfIn_, "memory-IN") &&
         sameStates(dfOut_, other.dfOut_, "memory-OUT");
}

bool FlowSensitivePTA::isFixedPoint(std::string *difference) const {
  Config config = config_;
  config.parallel = false;
  config.connectIndirectCall = {};
  config.setBackend = PointsToSetBackend::Mutable;
  FlowSensitivePTA inputs(*graph_, config);
  FlowSensitivePTA evaluated(*graph_, config);
  auto copySet = [&](const StoredSet &set) {
    StoredSet result;
    result.mutableSet = materialize(set);
    return result;
  };
  auto copyStates = [&](const auto &source, auto &destination) {
    for (const auto &node : source)
      for (const auto &fact : node.second)
        destination[node.first].emplace(fact.first, copySet(fact.second));
  };
  for (const auto &entry : topLevelPointsTo_)
    inputs.topLevelPointsTo_.emplace(entry.first, copySet(entry.second));
  copyStates(dfIn_, inputs.dfIn_);
  copyStates(dfOut_, inputs.dfOut_);
  for (const auto &fact : initialMemory_)
    evaluated.initialMemory_.emplace(fact.first, copySet(fact.second));
  evaluated.recursiveFunctions_ = recursiveFunctions_;
  evaluated.topRead_ = [&](const SVFGNode *node) -> const StoredSet & {
    return inputs.topSet(node);
  };
  evaluated.memoryRead_ = [&](const SVFGNode *node, bool outgoing,
                              const PointsToSet *) -> const MemoryState & {
    return outgoing ? inputs.outState(node) : inputs.inState(node);
  };
  for (const auto &entry : *graph_)
    if (inScope(entry.second))
      evaluated.transfer(*entry.second);
  evaluated.topRead_ = {};
  evaluated.memoryRead_ = {};
  return compareSolution(evaluated, true, difference);
}

bool FlowSensitivePTA::inScope(const SVFGNode *node) const {
  return node && (!config_.scope || config_.scope->contains(node));
}

FlowSensitivePTA::StoredSet FlowSensitivePTA::singleton(ObjectID object) {
  StoredSet set;
  if (persistentSets_)
    set.sharedSet = std::make_shared<const PointsToSet>(PointsToSet{object});
  else if (config_.setBackend == PointsToSetBackend::HashConsed)
    set.interned = arena_.singleton(object);
  else
    set.mutableSet.insert(object);
  return set;
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::materialize(const StoredSet &set) const {
  if (set.sharedSet)
    return *set.sharedSet;
  return config_.setBackend == PointsToSetBackend::HashConsed
             ? arena_.get(set.interned)
             : set.mutableSet;
}

bool FlowSensitivePTA::merge(StoredSet &destination, const StoredSet &source) {
  if (persistentSets_) {
    const PointsToSet &left = materialize(destination);
    const PointsToSet &right = materialize(source);
    if (right.empty() ||
        (destination.sharedSet && destination.sharedSet == source.sharedSet) ||
        std::includes(left.begin(), left.end(), right.begin(), right.end()))
      return false;
    if (left.empty() && source.sharedSet) {
      destination = source;
      return true;
    }
    auto result = std::make_shared<PointsToSet>(left);
    result->insert(right.begin(), right.end());
    destination.mutableSet.clear();
    destination.sharedSet = std::move(result);
    return true;
  }
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    const auto old = destination.interned;
    destination.interned = arena_.unite(old, source.interned);
    return old != destination.interned;
  }
  const std::size_t oldSize = destination.mutableSet.size();
  destination.mutableSet.insert(source.mutableSet.begin(),
                                source.mutableSet.end());
  return oldSize != destination.mutableSet.size();
}

bool FlowSensitivePTA::assign(StoredSet &destination, const StoredSet &source) {
  if (persistentSets_) {
    if ((destination.sharedSet && destination.sharedSet == source.sharedSet) ||
        materialize(destination) == materialize(source))
      return false;
    StoredSet result;
    if (source.sharedSet)
      result.sharedSet = source.sharedSet;
    else if (!materialize(source).empty())
      result.sharedSet =
          std::make_shared<const PointsToSet>(materialize(source));
    destination = std::move(result);
    return true;
  }
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    if (destination.interned == source.interned)
      return false;
    destination.interned = source.interned;
    return true;
  }
  if (destination.mutableSet == source.mutableSet)
    return false;
  destination.mutableSet = source.mutableSet;
  return true;
}

bool FlowSensitivePTA::mergeState(MemoryState &destination,
                                  const MemoryState &source) {
  bool changed = false;
  for (const auto &[object, pointsTo] : source)
    changed |= merge(destination[object], pointsTo);
  return changed;
}

bool FlowSensitivePTA::assignState(MemoryState &destination,
                                   const MemoryState &source) {
  if (destination.size() == source.size()) {
    bool equal = true;
    for (const auto &[object, pointsTo] : source) {
      auto current = destination.find(object);
      if (current == destination.end() ||
          materialize(current->second) != materialize(pointsTo)) {
        equal = false;
        break;
      }
    }
    if (equal)
      return false;
  }
  destination = source;
  return true;
}

const FlowSensitivePTA::StoredSet &
FlowSensitivePTA::topSet(const SVFGNode *node) const {
  if (topRead_)
    return topRead_(node);
  static const StoredSet empty;
  if (!node)
    return empty;
  auto it = topLevelPointsTo_.find(node->getId());
  return it == topLevelPointsTo_.end() ? empty : it->second;
}

const FlowSensitivePTA::MemoryState &
FlowSensitivePTA::outState(const SVFGNode *node,
                           const PointsToSet *objects) const {
  if (memoryRead_)
    return memoryRead_(node, true, objects);
  static const MemoryState empty;
  if (!node)
    return empty;
  auto it = dfOut_.find(node->getId());
  return it == dfOut_.end() ? empty : it->second;
}

const FlowSensitivePTA::MemoryState &
FlowSensitivePTA::inState(const SVFGNode *node,
                          const PointsToSet *objects) const {
  if (memoryRead_)
    return memoryRead_(node, false, objects);
  static const MemoryState empty;
  if (!node)
    return empty;
  auto it = dfIn_.find(node->getId());
  return it == dfIn_.end() ? empty : it->second;
}

FlowSensitivePTA::PointsToSet
FlowSensitivePTA::expandIndirectObjects(const PointsToSet &objects) const {
  PointsToSet expanded = objects;
  for (ObjectID object : objects) {
    if (!graph_->isFieldInsensitiveObject(object))
      continue;
    const PointsToSet fields = graph_->getFieldObjects(object);
    expanded.insert(fields.begin(), fields.end());
  }
  return mapObjects(expanded);
}

FlowSensitivePTA::ObjectID FlowSensitivePTA::mapObject(ObjectID object) const {
  return objectClustering_.empty() ? object : objectClustering_.map(object);
}

FlowSensitivePTA::PointsToSet
FlowSensitivePTA::mapObjects(const PointsToSet &objects) const {
  return objectClustering_.empty() ? objects
                                   : objectClustering_.mapAll(objects);
}

FlowSensitivePTA::StoredSet
FlowSensitivePTA::buildStored(const PointsToSet &set) {
  StoredSet stored;
  if (persistentSets_)
    stored.sharedSet = std::make_shared<const PointsToSet>(set);
  else if (config_.setBackend == PointsToSetBackend::HashConsed)
    stored.interned = arena_.intern(set);
  else
    stored.mutableSet = set;
  return stored;
}

void FlowSensitivePTA::mapStoredInPlace(StoredSet &set) {
  if (objectClustering_.empty())
    return;
  const PointsToSet &objects = materialize(set);
  PointsToSet mapped;
  bool changed = false;
  for (ObjectID object : objects) {
    const ObjectID representative = mapObject(object);
    changed |= representative != object;
    mapped.insert(representative);
  }
  if (changed)
    set = buildStored(mapped);
}

void FlowSensitivePTA::buildObjectClustering() {
  objectClustering_ = {};
  stats_.clusteredObjects = 0;
  stats_.mergedObjectClusters = 0;
  if (config_.clusterObjects == Config::ClusterMethod::None)
    return;
  objectClustering_ = flow_sensitive::clusterObjectsByType(*graph_);
  stats_.clusteredObjects = objectClustering_.mergedObjects();
  stats_.mergedObjectClusters = objectClustering_.mergedClusters();
}

FlowSensitivePTA::ObjectID
FlowSensitivePTA::fieldInsensitiveRepresentative(ObjectID base) {
  if (base == 0)
    return 0;
  auto cached = fieldInsensitiveObjects_.find(base);
  if (cached != fieldInsensitiveObjects_.end())
    return cached->second;
  // The graph is mutated only for object metadata, exactly like the
  // on-the-fly edge insertion performed through Config::connectIndirectCall.
  auto &mutableGraph = const_cast<SVFG &>(*graph_);
  const ObjectID existing = mutableGraph.getFieldInsensitiveObject(base);
  const ObjectID representative =
      mutableGraph.getOrCreateFieldInsensitiveObject(base);
  if (existing == 0 && representative != 0)
    ++stats_.fieldInsensitiveCollapses;
  fieldInsensitiveObjects_[base] = representative;
  return representative;
}

void FlowSensitivePTA::initializeRecursiveFunctions() {
  recursiveFunctions_.clear();
  if (const LTCallGraph *callGraph = graph_->getRefinedCallGraph()) {
    std::unordered_map<const Function *, int> index;
    std::unordered_map<const Function *, int> lowlink;
    std::unordered_set<const Function *> onStack;
    std::vector<const Function *> stack;
    int nextIndex = 0;
    std::function<void(const Function *)> visit =
        [&](const Function *function) {
          index[function] = lowlink[function] = nextIndex++;
          stack.push_back(function);
          onStack.insert(function);
          const LTCallGraphNode *node = (*callGraph)[function];
          for (const auto &edge : *node) {
            const LTCallGraphNode *calleeNode = edge.second;
            const Function *callee =
                calleeNode ? calleeNode->getFunction() : nullptr;
            if (!callee || callee->isDeclaration())
              continue;
            if (index.find(callee) == index.end()) {
              visit(callee);
              lowlink[function] = std::min(lowlink[function], lowlink[callee]);
            } else if (onStack.count(callee) != 0) {
              lowlink[function] = std::min(lowlink[function], index[callee]);
            }
          }
          if (lowlink[function] != index[function])
            return;
          std::vector<const Function *> component;
          do {
            const Function *member = stack.back();
            stack.pop_back();
            onStack.erase(member);
            component.push_back(member);
            if (member == function)
              break;
          } while (!stack.empty());
          bool recursive = component.size() > 1;
          if (!recursive && !component.empty()) {
            const LTCallGraphNode *single = (*callGraph)[component.front()];
            for (const auto &edge : *single)
              recursive |= edge.second == single;
          }
          if (recursive)
            recursiveFunctions_.insert(component.begin(), component.end());
        };
    for (const auto &entry : *callGraph) {
      const Function *function = entry.first;
      if (function && !function->isDeclaration() &&
          index.find(function) == index.end())
        visit(function);
    }
    return;
  }

  Module *module = nullptr;
  for (const auto &entry : *graph_) {
    if (const Function *function = entry.second->getFunction()) {
      module = const_cast<Module *>(function->getParent());
      break;
    }
  }
  if (!module)
    return;
  CallGraph callGraph(*module);
  for (scc_iterator<CallGraph *> it = scc_begin(&callGraph),
                                 end = scc_end(&callGraph);
       it != end; ++it) {
    const std::vector<CallGraphNode *> &component = *it;
    bool recursive = component.size() > 1;
    if (!recursive && component.size() == 1) {
      CallGraphNode *node = component.front();
      for (const auto &edge : *node)
        recursive |= edge.second == node;
    }
    if (!recursive)
      continue;
    for (CallGraphNode *node : component)
      if (node && node->getFunction())
        recursiveFunctions_.insert(node->getFunction());
  }
}

FlowSensitivePTA::StoredSet
FlowSensitivePTA::constantPointsTo(const Constant *constant) {
  StoredSet result;
  if (!constant || isa<ConstantPointerNull>(constant) ||
      !constant->getType()->isPointerTy())
    return result;
  if (const auto *expression = dyn_cast<ConstantExpr>(constant)) {
    if (expression->getOpcode() == Instruction::GetElementPtr) {
      const auto *gep = cast<GEPOperator>(expression);
      const Value *underlying = gep->getPointerOperand()->stripPointerCasts();
      const Module *module = nullptr;
      if (const auto *global = dyn_cast<GlobalValue>(underlying))
        module = global->getParent();
      else if (const auto *instruction = dyn_cast<Instruction>(underlying))
        module = instruction->getModule();
      if (module) {
        APInt offset(module->getDataLayout().getIndexTypeSizeInBits(
                         gep->getPointerOperandType()),
                     0);
        if (gep->accumulateConstantOffset(module->getDataLayout(), offset)) {
          StoredSet bases =
              constantPointsTo(dyn_cast<Constant>(gep->getPointerOperand()));
          StoredSet exact;
          for (ObjectID base : materialize(bases)) {
            ObjectID canonicalBase = base;
            uint64_t baseOffset = 0;
            if (const SVFG::ObjectInfo *info = graph_->getObjectInfo(base)) {
              if (info->baseObjId != 0)
                canonicalBase = info->baseObjId;
              if (info->hasFieldOffset)
                baseOffset = info->fieldOffset;
            }
            const uint64_t gepOffset = offset.getZExtValue();
            if (gepOffset > std::numeric_limits<uint64_t>::max() - baseOffset)
              continue;
            const uint64_t totalOffset = baseOffset + gepOffset;
            const ObjectID field =
                totalOffset == 0
                    ? canonicalBase
                    : graph_->getOffsetObject(canonicalBase, totalOffset);
            if (field != 0)
              merge(exact, singleton(field));
          }
          if (!materialize(exact).empty()) {
            mapStoredInPlace(exact);
            return exact;
          }
        }
      }
      const PointsToSet &known = graph_->getObjectIds(constant);
      for (ObjectID object : known)
        merge(result, singleton(object));
      if (!known.empty()) {
        mapStoredInPlace(result);
        return result;
      }
      return constantPointsTo(dyn_cast<Constant>(expression->getOperand(0)));
    }
  }
  const PointsToSet &known = graph_->getObjectIds(constant);
  for (ObjectID object : known)
    merge(result, singleton(object));
  if (!known.empty()) {
    mapStoredInPlace(result);
    return result;
  }
  if (const auto *expression = dyn_cast<ConstantExpr>(constant))
    if (expression->isCast())
      return constantPointsTo(dyn_cast<Constant>(expression->getOperand(0)));
  const Value *stripped = constant->stripPointerCasts();
  if (const ObjectID object = graph_->getObjectId(stripped))
    merge(result, singleton(mapObject(object)));
  mapStoredInPlace(result);
  return result;
}

void FlowSensitivePTA::initializeGlobalMemory() {
  initialMemory_.clear();
  const Module *module = nullptr;
  for (const auto &entry : *graph_) {
    if (const Function *function = entry.second->getFunction()) {
      module = function->getParent();
      break;
    }
  }
  if (!module)
    return;
  for (const GlobalVariable &global : module->globals()) {
    if (!global.hasInitializer())
      continue;
    PointsToSet storageBases;
    if (const ObjectID object = graph_->getObjectId(&global))
      storageBases.insert(object);
    if (storageBases.empty()) {
      const PointsToSet &known = graph_->getObjectIds(&global);
      storageBases.insert(known.begin(), known.end());
    }
    auto underlyingGlobal = [](const Value *value) {
      const Value *current = value;
      std::unordered_set<const Value *> visited;
      while (current && visited.insert(current).second) {
        current = current->stripPointerCasts();
        if (const auto *gep = dyn_cast<GEPOperator>(current)) {
          current = gep->getPointerOperand();
          continue;
        }
        break;
      }
      return dyn_cast_or_null<GlobalVariable>(current);
    };
    auto storageAtOffset = [&](uint64_t offset) {
      PointsToSet storage;
      for (ObjectID base : storageBases) {
        const SVFG::ObjectInfo *baseInfo = graph_->getObjectInfo(base);
        const ObjectID canonicalBase =
            baseInfo && baseInfo->baseObjId != 0 ? baseInfo->baseObjId : base;
        const ObjectID object =
            offset == 0 ? canonicalBase
                        : graph_->getOffsetObject(canonicalBase, offset);
        if (object != 0)
          storage.insert(object);
      }
      for (const auto &[object, label] : graph_->getObjectDebugMap()) {
        if (underlyingGlobal(graph_->getObjectValue(object)) != &global)
          continue;
        const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
        if (info && info->hasFieldOffset && info->fieldOffset == offset)
          storage.insert(object);
      }
      return mapObjects(storage);
    };

    const DataLayout &layout = module->getDataLayout();
    std::function<void(const Constant *, Type *, uint64_t)> seedInitializer =
        [&](const Constant *constant, Type *type, uint64_t offset) {
          if (!constant || !type)
            return;
          if (type->isPointerTy()) {
            StoredSet pointsTo = constantPointsTo(constant);
            if (materialize(pointsTo).empty())
              return;
            for (ObjectID storage : storageAtOffset(offset))
              merge(initialMemory_[storage], pointsTo);
            return;
          }
          if (auto *structure = dyn_cast<StructType>(type)) {
            const StructLayout *structureLayout =
                layout.getStructLayout(structure);
            for (unsigned field = 0; field < structure->getNumElements();
                 ++field) {
              seedInitializer(constant->getAggregateElement(field),
                              structure->getElementType(field),
                              offset +
                                  structureLayout->getElementOffset(field));
            }
            return;
          }
          if (auto *array = dyn_cast<ArrayType>(type)) {
            for (uint64_t element = 0; element < array->getNumElements();
                 ++element)
              seedInitializer(constant->getAggregateElement(element),
                              array->getElementType(), offset);
          }
        };
    seedInitializer(global.getInitializer(), global.getValueType(), 0);
  }
}

const Value *FlowSensitivePTA::accessPointer(const Instruction *instruction) {
  if (const auto *load = dyn_cast_or_null<LoadInst>(instruction))
    return load->getPointerOperand();
  if (const auto *store = dyn_cast_or_null<StoreInst>(instruction))
    return store->getPointerOperand();
  if (const auto *rmw = dyn_cast_or_null<AtomicRMWInst>(instruction))
    return rmw->getPointerOperand();
  if (const auto *cmpxchg = dyn_cast_or_null<AtomicCmpXchgInst>(instruction))
    return cmpxchg->getPointerOperand();
  return nullptr;
}

const Value *FlowSensitivePTA::storedValue(const Instruction *instruction) {
  if (const auto *store = dyn_cast_or_null<StoreInst>(instruction))
    return store->getValueOperand();
  if (const auto *rmw = dyn_cast_or_null<AtomicRMWInst>(instruction))
    return rmw->getValOperand();
  if (const auto *cmpxchg = dyn_cast_or_null<AtomicCmpXchgInst>(instruction))
    return cmpxchg->getNewValOperand();
  return nullptr;
}

FlowSensitivePTA::StoredSet
FlowSensitivePTA::pointerTargets(const Value *pointer) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return {};
  return topSet(graph_->getValueNode(pointer));
}

FlowSensitivePTA::PointsToSet
FlowSensitivePTA::accessTargets(const Value *pointer,
                                const PointsToSet &memoryPointsTo) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return expandIndirectObjects(memoryPointsTo);
  if (graph_->getValueNode(pointer))
    return expandIndirectObjects(materialize(pointerTargets(pointer)));
  // The value has no SVFG node (for example a constant-expression GEP or a
  // hand-built graph): fall back to the object set recorded for the value,
  // then to the MemorySSA access region.  An empty flow-sensitive set for an
  // existing node means the pointer is bottom here; using pre-analysis
  // targets would let a dead store clobber memory or a dead load read it.
  PointsToSet known = graph_->getObjectIds(pointer);
  if (known.empty())
    known = memoryPointsTo;
  return expandIndirectObjects(known);
}

FlowSensitivePTA::StoredSet
FlowSensitivePTA::directInput(const SVFGNode &node) {
  StoredSet result;
  for (const SVFGEdge *edge : node.getInEdges()) {
    if (edge && inScope(edge->getSrcNode()) &&
        isDirectVFGEdge(edge->getEdgeKind()))
      merge(result, topSet(edge->getSrcNode()));
  }
  return result;
}

bool FlowSensitivePTA::isStrongUpdate(const PointsToSet &targets) const {
  if (targets.size() != 1)
    return false;
  const ObjectID object = *targets.begin();
  // A Lotus merged object class cannot support strong updates: assigning
  // to one member must not kill the other members' contents.
  if (objectClustering_.clusterSize(object) > 1)
    return false;
  const SVFG::ObjectInfo *info = graph_->getObjectInfo(object);
  if (!info || info->isUnknown || info->isHeap || info->isArray ||
      info->isFieldInsensitive)
    return false;
  if (info->baseObjId != 0 && graph_->isFieldInsensitiveObject(info->baseObjId))
    return false;
  const Value *allocation = graph_->getObjectValue(object);
  if (!allocation && info->baseObjId != 0)
    allocation = graph_->getObjectValue(info->baseObjId);
  if (const auto *instruction = dyn_cast_or_null<Instruction>(allocation))
    if (recursiveFunctions_.count(instruction->getFunction()) != 0)
      return false;
  return true;
}

bool FlowSensitivePTA::resolveIndirectCalls(const SVFGNode &node,
                                            const StoredSet &pointsTo) {
  const NodeID functionPointer =
      node.hasValueId() ? node.getValueId() : node.getId();
  const auto &callSites = graph_->getIndCallSites(functionPointer);
  if (callSites.empty())
    return false;
  if (!config_.connectIndirectCall) {
    // Same condition SVF guards with `ander`: without a resolved call graph
    // the analysis silently loses inter-procedural flow. Surface it instead.
    stats_.unresolvedIndirectCalls += callSites.size();
    return false;
  }
  const auto otfStarted = std::chrono::steady_clock::now();
  bool changed = false;
  for (const CallBase *callSite : callSites) {
    for (ObjectID object : materialize(pointsTo)) {
      const auto *target =
          dyn_cast_or_null<Function>(graph_->getObjectValue(object));
      if (!target)
        continue;
      // SVF bounds every on-the-fly edge with the auxiliary Andersen call
      // graph (FlowSensitive::updateCallGraph). Apply the same bound here so a
      // non-bounding connector cannot widen the analysis.
      if (config_.isIndirectCallTargetAllowed &&
          !config_.isIndirectCallTargetAllowed(callSite, target)) {
        ++stats_.indirectCallsRejected;
        continue;
      }
      if (config_.connectIndirectCall(callSite, target)) {
        ++stats_.indirectCallEdges;
        if (!config_.isIndirectCallTargetAllowed)
          ++stats_.unboundedIndirectCalls;
        changed = true;
      }
    }
  }
  stats_.otfSeconds += std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - otfStarted)
                           .count();
  return changed;
}

void FlowSensitivePTA::reconnectIndirectCallsAfterRead() {
  if (!config_.connectIndirectCall)
    return;

  // The file stores solved facts but not the edges added by on-the-fly
  // call-graph construction. Recover them from the loaded solution: for each
  // indirect callsite, connect the callees its function pointer points to.
  std::vector<std::pair<const CallBase *, const Function *>> connections;
  std::unordered_set<const CallBase *> seenCallsites;
  for (const auto &[functionPointer, callSites] : graph_->getIndCallSiteMap()) {
    for (const CallBase *callSite : callSites) {
      if (!callSite || !seenCallsites.insert(callSite).second)
        continue;
      const Value *called = callSite->getCalledOperand();
      if (!called)
        continue;
      called = called->stripPointerCasts();
      const SVFGNode *node = graph_->getValueNode(called);
      if (!node)
        continue;
      for (ObjectID object : materialize(topSet(node))) {
        const auto *target =
            dyn_cast_or_null<Function>(graph_->getObjectValue(object));
        if (target)
          connections.emplace_back(callSite, target);
      }
    }
  }

  for (const auto &[callSite, target] : connections) {
    if (graph_->hasConnectedCallee(callSite, target))
      continue;
    if (config_.connectIndirectCall(callSite, target))
      ++stats_.indirectCallEdges;
  }
}

bool FlowSensitivePTA::transfer(const SVFGNode &node) {
  ++stats_.nodeProcesses;
  bool changed = false;
  MemoryState incoming;
  bool hasIndirectPredecessor = false;
  for (const SVFGEdge *edge : node.getInEdges()) {
    if (!edge || !inScope(edge->getSrcNode()) ||
        !isIndirectVFGEdge(edge->getEdgeKind()))
      continue;
    hasIndirectPredecessor = true;
    const SVFGNode *source = edge->getSrcNode();
    PointsToSet guarded = edge->getPointsTo();
    const bool wildcard =
        std::any_of(guarded.begin(), guarded.end(), [&](ObjectID object) {
          return graph_->isUnknownObject(object);
        });
    if (guarded.empty())
      continue;
    if (!wildcard)
      guarded = expandIndirectObjects(guarded);
    const PointsToSet *observed = wildcard ? nullptr : &guarded;
    const MemoryState &sourceState =
        isa<StoreSVFGNode, ActualOutSVFGNode>(source)
            ? outState(source, observed)
            : inState(source, observed);
    if (wildcard) {
      mergeState(incoming, sourceState);
      continue;
    }
    for (ObjectID object : guarded) {
      auto value = sourceState.find(object);
      if (value != sourceState.end()) {
        merge(incoming[object], value->second);
      } else {
        // A wildcard fact is a conservative substitute only while no fact is
        // available for the precise guarded object. Unconditionally joining
        // it would collapse unrelated memory regions after one imprecise
        // access (for example a global function-pointer field and stack data).
        for (const auto &[sourceObject, sourceValue] : sourceState)
          if (graph_->isUnknownObject(sourceObject))
            merge(incoming[object], sourceValue);
      }
    }
  }
  if (!hasIndirectPredecessor) {
    const auto *actualOut = dyn_cast<ActualOutSVFGNode>(&node);
    const CallBase *call = actualOut ? actualOut->getCallSite() : nullptr;
    const bool pendingIndirectReturn =
        call && !call->getCalledFunction() &&
        graph_->getConnectedCallees(call).empty();
    // An unresolved call return is not a memory root. Publishing global
    // initializers here would make later loads retain a killed value when a
    // writing callee is connected. No-op callees get explicit bypass edges.
    if (!pendingIndirectReturn)
      mergeState(incoming, initialMemory_);
  }
  changed |= assignState(dfIn_[node.getId()], incoming);

  // Materialise the memory-OUT state lazily: most nodes (addresses, copies,
  // phis, loads) never write memory, so their OUT equals IN and no copy is
  // needed.  OUT is only materialised on the first write.
  MemoryState outgoing;
  bool outgoingWritten = false;
  auto outgoingState = [&]() -> MemoryState & {
    if (!outgoingWritten) {
      outgoing = incoming;
      outgoingWritten = true;
    }
    return outgoing;
  };
  StoredSet top = topSet(&node);
  if (const auto *address = dyn_cast<AddrSVFGNode>(&node)) {
    ++stats_.processedAddr;
    ObjectID object = address->getObjectId();
    if (object == 0 && address->getValue())
      object = graph_->getObjectId(address->getValue());
    if (object != 0)
      top = singleton(object);
    if (isa_and_nonnull<CallBase>(address->getValue()))
      merge(top, directInput(node));
  } else if (const auto *load = dyn_cast<LoadSVFGNode>(&node)) {
    ++stats_.processedLoad;
    top = {};
    // SVF's pointer-only SVFG has no VFG node for a non-pointer load, so its
    // result never carries pointer facts.  Reading memory here would only
    // propagate values the loaded scalar cannot hold.
    if (!config_.pointerOnlySVFG || !load->getInstruction() ||
        load->getInstruction()->getType()->isPointerTy()) {
      PointsToSet targetSet = accessTargets(
          accessPointer(load->getInstruction()), load->getMemoryPointsTo());
      for (ObjectID object : targetSet) {
        if (graph_->isConstantObject(object))
          continue;
        auto value = incoming.find(object);
        if (value != incoming.end()) {
          merge(top, value->second);
        } else {
          for (const auto &[memoryObject, memoryValue] : incoming)
            if (graph_->isUnknownObject(memoryObject))
              merge(top, memoryValue);
        }
      }
    }
  } else if (const auto *store = dyn_cast<StoreSVFGNode>(&node)) {
    ++stats_.processedStore;
    const auto *instruction = store->getInstruction();
    const Value *valOp = storedValue(instruction);
    const Value *ptrOp = accessPointer(instruction);
    // SVF's processStore() only handles stores of pointer-typed values
    // (`store->getSrcNode()->isPointer()`); a scalar store cannot change the
    // pointer contents of its destination.  The node still forwards its IN
    // state to OUT below, so a following load observes the same memory.
    const bool pointerStore =
        !config_.pointerOnlySVFG || !valOp || valOp->getType()->isPointerTy();
    if (pointerStore) {
      StoredSet stored = valOp ? pointerTargets(valOp) : directInput(node);
      const PointsToSet targetSet =
          ptrOp ? accessTargets(ptrOp, store->getMemoryPointsTo())
                : expandIndirectObjects(store->getMemoryPointsTo());
      // Do not publish IN as OUT before the LHS has a target. Otherwise a
      // load can observe the old value before a later strong update, and
      // monotone top-level propagation cannot retract that speculative fact.
      if (targetSet.empty()) {
        // Evaluators in the parallel solvers expect all three result
        // channels to be materialized, including a suspended store's OUT.
        merge(topLevelPointsTo_[node.getId()], top);
        const MemoryState previous = outState(&node);
        assignState(dfOut_[node.getId()], previous);
        return changed;
      }
      const bool strong = isStrongUpdate(targetSet);
      bool updatedAny = false;
      for (ObjectID object : targetSet) {
        if (graph_->isConstantObject(object))
          continue;
        if (strong) {
          assign(outgoingState()[object], stored);
        } else {
          merge(outgoingState()[object], stored);
        }
        updatedAny = true;
      }
      if (updatedAny) {
        if (strong) {
          weakUpdateSites_.erase(node.getId());
          strongUpdateSites_.insert(node.getId());
          ++stats_.strongUpdateExecutions;
        } else {
          strongUpdateSites_.erase(node.getId());
          weakUpdateSites_.insert(node.getId());
          ++stats_.weakUpdateExecutions;
        }
      }
    }
  } else if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(&node)) {
    ++stats_.processedMSSA;
    const auto *intrinsic =
        dyn_cast_or_null<IntrinsicInst>(actualOut->getCallSite());
    if (intrinsic &&
        (intrinsic->getIntrinsicID() == Intrinsic::memcpy ||
         intrinsic->getIntrinsicID() == Intrinsic::memmove ||
         intrinsic->getIntrinsicID() == Intrinsic::memset) &&
        intrinsic->arg_size() >= 2) {
      const Intrinsic::ID intrinsicId = intrinsic->getIntrinsicID();
      auto objectsWithFields = [&](const Value *pointer) {
        PointsToSet objects = materialize(pointerTargets(pointer));
        if (objects.empty()) {
          const PointsToSet &preAnalysis = graph_->getObjectIds(pointer);
          for (ObjectID object : preAnalysis)
            objects.insert(mapObject(object));
        }
        PointsToSet expanded = objects;
        for (ObjectID object : objects) {
          const PointsToSet fields = graph_->getFieldObjects(object);
          expanded.insert(fields.begin(), fields.end());
        }
        auto underlyingBase = [](const Value *value) {
          const Value *current = value;
          std::unordered_set<const Value *> visited;
          while (current && visited.insert(current).second) {
            current = current->stripPointerCasts();
            if (const auto *gep = dyn_cast<GEPOperator>(current)) {
              current = gep->getPointerOperand();
              continue;
            }
            break;
          }
          return current;
        };
        const Value *base = underlyingBase(pointer);
        if (base) {
          for (const auto &[candidate, nodeID] : graph_->getValueNodeMap()) {
            if (!candidate || !candidate->getType()->isPointerTy() ||
                underlyingBase(candidate) != base)
              continue;
            const PointsToSet &candidateObjects =
                graph_->getObjectIds(candidate);
            expanded.insert(candidateObjects.begin(), candidateObjects.end());
            if (ObjectID candidateObject = graph_->getObjectId(candidate))
              expanded.insert(candidateObject);
          }
        }
        return mapObjects(expanded);
      };

      struct RootLocation {
        ObjectID base = 0;
        uint64_t offset = 0;
      };
      auto rootLocations = [&](const Value *pointer) {
        PointsToSet roots = materialize(pointerTargets(pointer));
        if (roots.empty()) {
          const PointsToSet &preAnalysis = graph_->getObjectIds(pointer);
          for (ObjectID object : preAnalysis)
            roots.insert(mapObject(object));
        }
        std::vector<RootLocation> locations;
        for (ObjectID root : roots) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(root);
          locations.push_back(
              {info && info->baseObjId != 0 ? info->baseObjId : root,
               info && info->hasFieldOffset ? info->fieldOffset : 0});
        }
        return locations;
      };

      MemoryState allInputs;
      for (const SVFGNode *actualInNode :
           graph_->getActualIns(actualOut->getCallSite())) {
        mergeState(allInputs, inState(actualInNode));
        mergeState(outgoingState(), inState(actualInNode));
      }
      for (const SVFGEdge *edge : node.getInEdges()) {
        const SVFGNode *source = edge ? edge->getSrcNode() : nullptr;
        if (isa_and_nonnull<StoreSVFGNode>(source))
          mergeState(allInputs, outState(source));
      }

      const std::vector<RootLocation> destinationRoots =
          rootLocations(intrinsic->getArgOperand(0));
      const auto *lengthConstant =
          intrinsic->arg_size() >= 3
              ? dyn_cast<ConstantInt>(intrinsic->getArgOperand(2))
              : nullptr;
      const bool hasExactLength = lengthConstant != nullptr;
      const uint64_t copyLength =
          lengthConstant ? lengthConstant->getZExtValue() : 0;
      auto pointerSizeForObject = [&](ObjectID object) {
        for (const auto &entry : graph_->getValueNodeMap()) {
          const Value *value = entry.first;
          const auto *gep = dyn_cast_or_null<GetElementPtrInst>(value);
          if (!gep || graph_->getObjectIds(gep).count(object) == 0)
            continue;
          const auto *pointerType =
              dyn_cast<PointerType>(gep->getResultElementType());
          if (pointerType)
            return static_cast<uint64_t>(
                intrinsic->getModule()->getDataLayout().getPointerSize(
                    pointerType->getAddressSpace()));
        }
        return static_cast<uint64_t>(
            intrinsic->getModule()->getDataLayout().getPointerSize());
      };
      auto fullyCopied = [&](uint64_t relativeOffset, uint64_t pointerSize) {
        return !hasExactLength || (relativeOffset <= copyLength &&
                                   pointerSize <= copyLength - relativeOffset);
      };
      ObjectID unknownObject = 0;
      for (const auto &[object, label] : graph_->getObjectDebugMap())
        if (graph_->isUnknownObject(object)) {
          unknownObject = object;
          break;
        }

      if (hasExactLength) {
        const PointsToSet destinationObjects =
            objectsWithFields(intrinsic->getArgOperand(0));
        StoredSet empty;
        for (ObjectID destination : destinationObjects) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(destination);
          const ObjectID destinationBase =
              info && info->baseObjId != 0 ? info->baseObjId : destination;
          const uint64_t destinationOffset =
              info && info->hasFieldOffset ? info->fieldOffset : 0;
          for (const RootLocation &root : destinationRoots) {
            if (root.base != destinationBase || destinationOffset < root.offset)
              continue;
            const uint64_t relativeOffset = destinationOffset - root.offset;
            if (fullyCopied(relativeOffset,
                            pointerSizeForObject(destination))) {
              if (intrinsicId == Intrinsic::memset) {
                const auto *fill =
                    dyn_cast<ConstantInt>(intrinsic->getArgOperand(1));
                if (fill && fill->isZero())
                  assign(outgoingState()[mapObject(destination)], empty);
                else if (unknownObject != 0)
                  assign(outgoingState()[mapObject(destination)],
                         singleton(unknownObject));
              } else {
                assign(outgoingState()[mapObject(destination)], empty);
              }
            } else if (relativeOffset < copyLength && unknownObject != 0) {
              assign(outgoingState()[mapObject(destination)],
                     singleton(unknownObject));
            }
          }
        }
      } else if (intrinsicId == Intrinsic::memset) {
        const auto *fill = dyn_cast<ConstantInt>(intrinsic->getArgOperand(1));
        if ((!fill || !fill->isZero()) && unknownObject != 0) {
          const PointsToSet destinationObjects =
              objectsWithFields(intrinsic->getArgOperand(0));
          for (ObjectID destination : destinationObjects)
            merge(outgoingState()[mapObject(destination)],
                  singleton(unknownObject));
        }
      }

      if (intrinsicId == Intrinsic::memcpy ||
          intrinsicId == Intrinsic::memmove) {
        const std::vector<RootLocation> sourceRoots =
            rootLocations(intrinsic->getArgOperand(1));
        MemoryState copiedByDestination;
        for (const auto &[source, value] : allInputs) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(source);
          const ObjectID sourceBase =
              info && info->baseObjId != 0 ? info->baseObjId : source;
          const uint64_t sourceOffset =
              info && info->hasFieldOffset ? info->fieldOffset : 0;
          for (const RootLocation &sourceRoot : sourceRoots) {
            if (sourceRoot.base != sourceBase ||
                sourceOffset < sourceRoot.offset)
              continue;
            const uint64_t relativeOffset = sourceOffset - sourceRoot.offset;
            if (!fullyCopied(relativeOffset, pointerSizeForObject(source)))
              continue;
            for (const RootLocation &destinationRoot : destinationRoots) {
              const uint64_t destinationOffset =
                  destinationRoot.offset + relativeOffset;
              if (destinationOffset < destinationRoot.offset)
                continue;
              const ObjectID destination =
                  destinationOffset == 0
                      ? destinationRoot.base
                      : graph_->getOffsetObject(destinationRoot.base,
                                                destinationOffset);
              if (destination != 0)
                merge(copiedByDestination[mapObject(destination)], value);
            }
          }
        }
        for (const auto &[destination, value] : copiedByDestination)
          if (hasExactLength)
            assign(outgoingState()[mapObject(destination)], value);
          else
            merge(outgoingState()[mapObject(destination)], value);
      }
    }
  } else if (const auto *gep = dyn_cast<GepSVFGNode>(&node)) {
    ++stats_.processedGep;
    const auto *instruction =
        dyn_cast_or_null<GetElementPtrInst>(gep->getValue());
    StoredSet bases = directInput(node);
    top = {};
    if (instruction) {
      for (ObjectID base : materialize(bases)) {
        if (graph_->isUnknownObject(base)) {
          merge(top, singleton(base));
          continue;
        }
        // SVF: `if (isFieldInsensitive(o)) { set(o); continue; }`
        if (graph_->isFieldInsensitiveObject(base)) {
          merge(top, singleton(fieldInsensitiveRepresentative(base)));
          continue;
        }
        const SVFG::GepAccessInfo access = graph_->getGepAccess(instruction);
        if (!access.valid) {
          // SVF: `if (gepStmt->isVariantFieldGep()) {
          // setObjFieldInsensitive(o); set(getFIObjVar(o)); }`. The builder
          // normally decides this while constructing the SVFG; collapse here
          // as well so hand-built graphs and future callers cannot silently
          // keep per-field objects for a variable-index access.
          const ObjectID fi = fieldInsensitiveRepresentative(base);
          merge(top, singleton(fi != 0 ? fi : base));
          continue;
        }
        // Annotated constant GEP: use the canonical (base, offset) object.
        {
          ObjectID canonicalBase = base;
          uint64_t baseOffset = 0;
          if (const SVFG::ObjectInfo *info = graph_->getObjectInfo(base)) {
            if (info->baseObjId != 0)
              canonicalBase = info->baseObjId;
            if (info->hasFieldOffset)
              baseOffset = info->fieldOffset;
          }
          const uint64_t totalOffset = baseOffset + access.relativeOffset;
          if (totalOffset == 0) {
            merge(top, singleton(canonicalBase));
            continue;
          }
          if (const ObjectID offsetObject =
                  graph_->getOffsetObject(canonicalBase, totalOffset)) {
            merge(top, singleton(offsetObject));
            continue;
          }
        }
        const ObjectID mapped = graph_->getGepObject(instruction, base);
        if (mapped != 0) {
          merge(top, singleton(mapped));
          continue;
        }
        const PointsToSet &candidates = graph_->getObjectIds(gep->getValue());
        bool matchedBase = false;
        for (ObjectID candidate : candidates) {
          const SVFG::ObjectInfo *info = graph_->getObjectInfo(candidate);
          const ObjectID candidateBase =
              info && info->baseObjId != 0 ? info->baseObjId : candidate;
          if (candidate != base && candidateBase != base)
            continue;
          merge(top, singleton(candidate));
          matchedBase = true;
        }
        if (!matchedBase)
          for (ObjectID candidate : candidates)
            merge(top, singleton(candidate));
      }
    }
    if (materialize(top).empty() && !materialize(bases).empty()) {
      const PointsToSet &fields = gep->getValue()
                                      ? graph_->getObjectIds(gep->getValue())
                                      : PointsToSet{};
      for (ObjectID field : fields)
        merge(top, singleton(field));
    }
  } else if (isa<MSSASVFGNode>(&node)) {
    ++stats_.processedMSSA;
  } else {
    if (isa<MSSAPhiSVFGNode>(&node))
      ++stats_.processedPhi;
    else if (isa<ActualParmSVFGNode>(&node))
      ++stats_.processedActualParam;
    else if (isa<FormalRetSVFGNode>(&node))
      ++stats_.processedFormalRet;
    else if (isa<CopySVFGNode>(&node))
      ++stats_.processedCopy;
    top = directInput(node);
  }

  mapStoredInPlace(top);
  StoredSet &currentTop = topLevelPointsTo_[node.getId()];
  // Parallel evaluators read their previous result through the snapshot
  // accessor; their local output map starts empty. Seed it before unioning
  // the new facts so sequential and speculative transfers both accumulate.
  merge(currentTop, topSet(&node));
  const bool topChanged = merge(currentTop, top);
  changed |= topChanged;
  if (topChanged && resolveIndirectCalls(node, topLevelPointsTo_[node.getId()]))
    topologyChanged_ = true;
  changed |=
      assignState(dfOut_[node.getId()], outgoingWritten ? outgoing : incoming);
  return changed;
}

void FlowSensitivePTA::collectStatistics() {
  // SVF-named population statistics (FlowSensitiveStat).
  stats_.totalPointers = graph_->countPointerValueNodes();
  const auto [fiObjects, fsObjects] = graph_->countBaseObjects();
  stats_.fieldInsensitiveObjects = fiObjects;
  stats_.fieldSensitiveObjects = fsObjects;
  stats_.totalObjects = fiObjects + fsObjects;

  std::size_t inOutFacts = 0;
  std::size_t inOutFactCount = 0;
  auto accumulate = [&](const std::unordered_map<NodeID, MemoryState> &states) {
    for (const auto &[node, state] : states)
      for (const auto &[object, pointsTo] : state) {
        const std::size_t size = materialize(pointsTo).size();
        stats_.maxPtsSize = std::max(stats_.maxPtsSize, size);
        inOutFacts += size;
        ++inOutFactCount;
      }
  };
  accumulate(dfIn_);
  stats_.memoryInFacts = inOutFacts;
  accumulate(dfOut_);
  stats_.memoryOutFacts = inOutFacts - stats_.memoryInFacts;
  if (inOutFactCount != 0)
    stats_.averagePtsSize =
        static_cast<double>(inOutFacts) / static_cast<double>(inOutFactCount);

  std::size_t addrTakenTotal = 0;
  for (const auto &[nodeID, node] : *graph_) {
    if (!inScope(node))
      continue;
    const auto *store = dyn_cast<StoreSVFGNode>(node);
    if (!store)
      continue;
    const Value *pointer = accessPointer(store->getInstruction());
    if (!pointer || !pointer->getType()->isPointerTy())
      continue;
    // Pointer-only mode ignores scalar stores, so they do not contribute an
    // address-taken variable either.
    const Value *stored = storedValue(store->getInstruction());
    if (config_.pointerOnlySVFG && stored && !stored->getType()->isPointerTy())
      continue;
    const std::size_t size = materialize(pointerTargets(pointer)).size();
    if (size == 0)
      continue;
    ++stats_.addrTakenVars;
    addrTakenTotal += size;
    stats_.maxAddrTakenPtsSize = std::max(stats_.maxAddrTakenPtsSize, size);
  }
  if (stats_.addrTakenVars != 0)
    stats_.averageAddrTakenPtsSize = static_cast<double>(addrTakenTotal) /
                                     static_cast<double>(stats_.addrTakenVars);
}

FlowSensitivePTA::SCCInfo FlowSensitivePTA::computeSCCs() const {
  flow_sensitive::SVFGSCC scc = flow_sensitive::computeSVFGSCCs(
      *graph_, [this](const SVFGNode *node) { return inScope(node); });
  SCCInfo result;
  result.components = std::move(scc.components);
  result.nodeToComponent = std::move(scc.componentOf);
  result.successors = std::move(scc.successors);
  return result;
}

const FlowSensitivePTA::Statistics &FlowSensitivePTA::solve() {
  if (config_.parallel)
    return solveParallel();
  const auto started = std::chrono::steady_clock::now();
  topLevelPointsTo_.clear();
  dfIn_.clear();
  dfOut_.clear();
  initialMemory_.clear();
  strongUpdateSites_.clear();
  weakUpdateSites_.clear();
  stats_ = {};
  if (config_.setBackend == PointsToSetBackend::HashConsed)
    arena_.reset();
  hasDeadline_ = config_.timeLimitSeconds != 0;
  if (hasDeadline_)
    deadline_ = started +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(config_.timeLimitSeconds));
  buildObjectClustering();
  {
    const auto initializeStarted = std::chrono::steady_clock::now();
    initializeGlobalMemory();
    stats_.initializeSeconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      initializeStarted)
            .count();
  }
  do {
    if (timeLimitExceeded()) {
      stats_.timedOut = true;
      break;
    }
    ++stats_.topologyEpochs;
    topologyChanged_ = false;
    initializeRecursiveFunctions();
    SCCInfo scc = computeSCCs();
    stats_.sccs = scc.components.size();
    stats_.nodes = 0;
    std::queue<std::size_t> worklist;
    std::vector<bool> queued(scc.components.size(), true);
    for (std::size_t i = 0; i < scc.components.size(); ++i) {
      worklist.push(i);
      stats_.nodes += scc.components[i].size();
      stats_.maxSccSize = std::max(stats_.maxSccSize, scc.components[i].size());
    }
    while (!worklist.empty() && !stats_.timedOut) {
      if (timeLimitExceeded()) {
        stats_.timedOut = true;
        break;
      }
      const std::size_t component = worklist.front();
      worklist.pop();
      queued[component] = false;
      ++stats_.solveIterations;
      bool anyChanged = false;
      std::queue<const SVFGNode *> localWorklist;
      std::unordered_set<NodeID> locallyQueued;
      for (const SVFGNode *node : scc.components[component]) {
        localWorklist.push(node);
        locallyQueued.insert(node->getId());
      }
      while (!localWorklist.empty() && !topologyChanged_ && !stats_.timedOut) {
        if (timeLimitExceeded()) {
          stats_.timedOut = true;
          break;
        }
        const SVFGNode *node = localWorklist.front();
        localWorklist.pop();
        locallyQueued.erase(node->getId());
        if (!transfer(*node))
          continue;
        anyChanged = true;
        for (const SVFGEdge *edge : node->getOutEdges()) {
          const SVFGNode *successor = edge ? edge->getDstNode() : nullptr;
          if (!inScope(successor) ||
              scc.nodeToComponent[successor->getId()] != component ||
              locallyQueued.count(successor->getId()) != 0)
            continue;
          localWorklist.push(successor);
          locallyQueued.insert(successor->getId());
        }
      }
      if (topologyChanged_)
        break;
      if (anyChanged)
        for (std::size_t successor : scc.successors[component])
          if (!queued[successor]) {
            queued[successor] = true;
            worklist.push(successor);
          }
    }
  } while (topologyChanged_ && !stats_.timedOut);
  for (const auto &[node, pointsTo] : topLevelPointsTo_) {
    const PointsToSet &set = materialize(pointsTo);
    stats_.topLevelFacts += set.size();
    stats_.maxTopLevelPtsSize =
        std::max<std::size_t>(stats_.maxTopLevelPtsSize, set.size());
  }
  if (!topLevelPointsTo_.empty())
    stats_.averageTopLevelPtsSize =
        static_cast<double>(stats_.topLevelFacts) /
        static_cast<double>(topLevelPointsTo_.size());
  collectStatistics();
  stats_.strongUpdates = strongUpdateSites_.size();
  stats_.weakUpdates = weakUpdateSites_.size();
  stats_.svfgHasSU = !strongUpdateSites_.empty();
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    const auto hashStats = arena_.statistics();
    stats_.hashConsedUniqueSets = hashStats.uniqueSets;
    stats_.hashConsedUnionCacheHits = hashStats.unionCacheHits;
  }
  stats_.solveSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  return stats_;
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::pointsTo(const SVFGNode *node) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto it = topLevelPointsTo_.find(node->getId());
  return it == topLevelPointsTo_.end() ? empty : materialize(it->second);
}

std::optional<FlowSensitivePTA::PointsToSet>
FlowSensitivePTA::pointsTo(const Value *value) const {
  if (!value || !value->getType()->isPointerTy())
    return std::nullopt;
  const SVFGNode *node = graph_->getValueNode(value);
  if (!inScope(node))
    return std::nullopt;
  return pointsTo(node);
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::memoryIn(const SVFGNode *node, ObjectID object) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto state = dfIn_.find(node->getId());
  if (state == dfIn_.end())
    return empty;
  auto value = state->second.find(mapObject(object));
  return value == state->second.end() ? empty : materialize(value->second);
}

const FlowSensitivePTA::PointsToSet &
FlowSensitivePTA::memoryOut(const SVFGNode *node, ObjectID object) const {
  static const PointsToSet empty;
  if (!node)
    return empty;
  auto state = dfOut_.find(node->getId());
  if (state == dfOut_.end())
    return empty;
  auto value = state->second.find(mapObject(object));
  return value == state->second.end() ? empty : materialize(value->second);
}

std::optional<bool> FlowSensitivePTA::mayAlias(const Value *lhs,
                                               const Value *rhs) const {
  auto left = pointsTo(lhs);
  auto right = pointsTo(rhs);
  if (!left || !right)
    return std::nullopt;
  for (const PointsToSet *set : {&*left, &*right})
    for (ObjectID object : *set)
      if (graph_->isUnknownObject(object))
        return true;
  // A field-insensitive pointee overlaps each field of its base object.
  *left = expandIndirectObjects(*left);
  *right = expandIndirectObjects(*right);
  const PointsToSet *small = &*left, *large = &*right;
  if (large->size() < small->size())
    std::swap(small, large);
  for (ObjectID object : *small)
    if (large->count(object) != 0)
      return true;
  return false;
}

} // namespace lotus::alias
