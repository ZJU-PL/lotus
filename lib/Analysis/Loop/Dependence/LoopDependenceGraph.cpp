/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"
#include "Analysis/Loop/Dependence/LoopDependenceBuilder.h"

#include "IR/PDG/Core/Graph.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

bool isInternalValueToLoop(LoopStructure *loop, llvm::Value *value) {
  if (loop == nullptr || value == nullptr) {
    return false;
  }
  if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
    return loop->isIncluded(instruction);
  }
  return false;
}

const char *describeEdgeKind(LoopDependenceEdgeKind kind) {
  switch (kind) {
  case LoopDependenceEdgeKind::Variable:
    return "var";
  case LoopDependenceEdgeKind::Control:
    return "ctrl";
  case LoopDependenceEdgeKind::Memory:
    return "mem";
  }
  return "unknown";
}

const char *describeMemoryKind(LoopDependenceMemoryKind kind) {
  switch (kind) {
  case LoopDependenceMemoryKind::None:
    return "none";
  case LoopDependenceMemoryKind::Raw:
    return "raw";
  case LoopDependenceMemoryKind::ReadOnly:
    return "read";
  case LoopDependenceMemoryKind::AliasUnknown:
    return "alias";
  case LoopDependenceMemoryKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *describeOrigin(LoopDependenceEdgeOrigin origin) {
  switch (origin) {
  case LoopDependenceEdgeOrigin::ImportedPDG:
    return "pdg";
  case LoopDependenceEdgeOrigin::SynthesizedBoundaryValue:
    return "boundary";
  case LoopDependenceEdgeOrigin::SynthesizedRefinement:
    return "refinement";
  }
  return "unknown";
}

LoopDependenceEdgeKind classifyEdgeKind(pdg::EdgeType type) {
  switch (type) {
  case pdg::EdgeType::CONTROLDEP_CALLINV:
  case pdg::EdgeType::CONTROLDEP_CALLRET:
  case pdg::EdgeType::CONTROLDEP_ENTRY:
  case pdg::EdgeType::CONTROLDEP_BR:
  case pdg::EdgeType::CONTROLDEP_IND_BR:
    return LoopDependenceEdgeKind::Control;
  case pdg::EdgeType::DATA_DEF_USE:
  case pdg::EdgeType::DATA_RET:
  case pdg::EdgeType::PARAMETER_IN:
  case pdg::EdgeType::PARAMETER_OUT:
  case pdg::EdgeType::PARAMETER_FIELD:
  case pdg::EdgeType::GLOBAL_DEP:
  case pdg::EdgeType::VAL_DEP:
    return LoopDependenceEdgeKind::Variable;
  default:
    return LoopDependenceEdgeKind::Memory;
  }
}

LoopDependenceMemoryKind classifyMemoryKind(pdg::EdgeType type) {
  switch (type) {
  case pdg::EdgeType::DATA_RAW:
    return LoopDependenceMemoryKind::Raw;
  case pdg::EdgeType::DATA_READ:
    return LoopDependenceMemoryKind::ReadOnly;
  case pdg::EdgeType::DATA_ALIAS:
    return LoopDependenceMemoryKind::AliasUnknown;
  case pdg::EdgeType::DATA_DEF_USE:
  case pdg::EdgeType::DATA_RET:
  case pdg::EdgeType::PARAMETER_IN:
  case pdg::EdgeType::PARAMETER_OUT:
  case pdg::EdgeType::PARAMETER_FIELD:
  case pdg::EdgeType::GLOBAL_DEP:
  case pdg::EdgeType::VAL_DEP:
  case pdg::EdgeType::CONTROLDEP_CALLINV:
  case pdg::EdgeType::CONTROLDEP_CALLRET:
  case pdg::EdgeType::CONTROLDEP_ENTRY:
  case pdg::EdgeType::CONTROLDEP_BR:
  case pdg::EdgeType::CONTROLDEP_IND_BR:
    return LoopDependenceMemoryKind::None;
  default:
    return LoopDependenceMemoryKind::Unknown;
  }
}

template <typename T> void eraseFirst(std::vector<T *> &values, T *needle) {
  auto it = std::find(values.begin(), values.end(), needle);
  if (it != values.end()) {
    values.erase(it);
  }
}

} // namespace

std::string LoopDependenceGraph::describeValue(const llvm::Value *value) {
  if (value == nullptr) {
    return "null";
  }
  if (const auto *function = llvm::dyn_cast<llvm::Function>(value)) {
    return std::string("func:") + function->getName().str();
  }
  if (value->hasName()) {
    return value->getName().str();
  }
  std::string text;
  llvm::raw_string_ostream stream(text);
  value->printAsOperand(stream, false);
  return stream.str();
}

bool LoopDependenceGraph::shouldIgnoreInstruction(
    const llvm::Instruction *instruction) {
  if (instruction == nullptr) {
    return true;
  }
  if (llvm::isa<llvm::DbgInfoIntrinsic>(instruction)) {
    return true;
  }
  if (const auto *call = llvm::dyn_cast<llvm::CallInst>(instruction)) {
    if (call->isLifetimeStartOrEnd()) {
      return true;
    }
  }
  return false;
}

bool LoopDependenceGraph::shouldIgnoreValue(const llvm::Value *value) {
  if (value == nullptr) {
    return true;
  }
  if (llvm::isa<llvm::BasicBlock>(value) ||
      llvm::isa<llvm::MetadataAsValue>(value)) {
    return true;
  }
  if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(value)) {
    return shouldIgnoreInstruction(inst);
  }
  return false;
}

bool LoopDependenceGraph::isAllowedBoundaryContextValue(
    const llvm::Value *value) {
  return value != nullptr && !shouldIgnoreValue(value);
}

LoopDependenceNode::LoopDependenceNode(uint64_t id,
                                       llvm::Value *value,
                                       pdg::Node *pdgNode,
                                       bool internal)
    : id{id}, value{value}, pdgNode{pdgNode}, internal{internal} {}

uint64_t LoopDependenceNode::getID(void) const { return this->id; }

llvm::Value *LoopDependenceNode::getValue(void) const { return this->value; }

pdg::Node *LoopDependenceNode::getPDGNode(void) const { return this->pdgNode; }

bool LoopDependenceNode::isInternal(void) const { return this->internal; }

bool LoopDependenceNode::isExternal(void) const { return !this->internal; }

const std::vector<LoopDependenceEdge *> &
LoopDependenceNode::getIncomingEdges(void) const {
  return this->incomingEdges;
}

const std::vector<LoopDependenceEdge *> &
LoopDependenceNode::getOutgoingEdges(void) const {
  return this->outgoingEdges;
}

bool LoopDependenceNode::hasIncomingEdges(void) const {
  return !this->incomingEdges.empty();
}

bool LoopDependenceNode::hasOutgoingEdges(void) const {
  return !this->outgoingEdges.empty();
}

LoopDependenceEdge::LoopDependenceEdge(LoopDependenceNode *src,
                                       LoopDependenceNode *dst,
                                       LoopDependenceEdgeKind kind,
                                       LoopDependenceMemoryKind memoryKind,
                                       LoopDependenceEdgeOrigin origin,
                                       pdg::EdgeType originalEdgeType,
                                       bool loopCarried)
    : src{src}, dst{dst}, kind{kind}, memoryKind{memoryKind},
      origin{origin}, originalEdgeType{originalEdgeType},
      loopCarried{loopCarried} {}

LoopDependenceNode *LoopDependenceEdge::getSrc(void) const { return this->src; }

LoopDependenceNode *LoopDependenceEdge::getDst(void) const { return this->dst; }

LoopDependenceEdgeKind LoopDependenceEdge::getKind(void) const {
  return this->kind;
}

LoopDependenceMemoryKind LoopDependenceEdge::getMemoryKind(void) const {
  return this->memoryKind;
}

LoopDependenceEdgeOrigin LoopDependenceEdge::getOrigin(void) const {
  return this->origin;
}

pdg::EdgeType LoopDependenceEdge::getOriginalEdgeType(void) const {
  return this->originalEdgeType;
}

bool LoopDependenceEdge::isLoopCarried(void) const { return this->loopCarried; }

void LoopDependenceEdge::setLoopCarried(bool isLoopCarried) {
  this->loopCarried = isLoopCarried;
}

LoopDependenceGraph::LoopDependenceGraph(LoopTree *loopNode,
                                         pdg::ProgramGraph &pdg) {
  auto bundle =
      LoopDependenceBuilder::buildBaseLoopDependenceGraph(loopNode, pdg);
  *this = std::move(*bundle.graph);
}

LoopTree *LoopDependenceGraph::getLoopHierarchyStructures(void) const {
  return this->loop;
}

LoopStructure *LoopDependenceGraph::getLoopStructure(void) const {
  return this->loop ? this->loop->getLoop() : nullptr;
}

std::vector<LoopDependenceNode *>
LoopDependenceGraph::getInternalNodes(void) const {
  return this->internalNodes;
}

std::vector<LoopDependenceNode *>
LoopDependenceGraph::getExternalNodes(void) const {
  return this->externalNodes;
}

std::vector<LoopDependenceNode *> LoopDependenceGraph::getNodes(void) const {
  std::vector<LoopDependenceNode *> nodes;
  nodes.reserve(this->ownedNodes.size());
  for (const auto &owned : this->ownedNodes) {
    nodes.push_back(owned.get());
  }
  return nodes;
}

std::vector<LoopDependenceEdge *> LoopDependenceGraph::getEdges(void) const {
  std::vector<LoopDependenceEdge *> edges;
  edges.reserve(this->ownedEdges.size());
  for (const auto &edge : this->ownedEdges) {
    edges.push_back(edge.get());
  }
  return edges;
}

std::vector<LoopDependenceEdge *> LoopDependenceGraph::getDependences(
    llvm::Value *src,
    llvm::Value *dst) const {
  std::vector<LoopDependenceEdge *> result;
  auto *srcNode = this->getNode(src);
  auto *dstNode = this->getNode(dst);
  if (srcNode == nullptr || dstNode == nullptr) {
    return result;
  }

  for (auto *edge : srcNode->getOutgoingEdges()) {
    if (edge != nullptr && edge->getDst() == dstNode) {
      result.push_back(edge);
    }
  }
  return result;
}

std::vector<LoopDependenceEdge *>
LoopDependenceGraph::getSortedDependences(void) const {
  auto edges = this->getEdges();
  std::sort(edges.begin(), edges.end(),
            [](LoopDependenceEdge *lhs, LoopDependenceEdge *rhs) {
              if (lhs->getSrc()->getID() != rhs->getSrc()->getID()) {
                return lhs->getSrc()->getID() < rhs->getSrc()->getID();
              }
              if (lhs->getDst()->getID() != rhs->getDst()->getID()) {
                return lhs->getDst()->getID() < rhs->getDst()->getID();
              }
              if (lhs->getKind() != rhs->getKind()) {
                return static_cast<int>(lhs->getKind()) <
                       static_cast<int>(rhs->getKind());
              }
              if (lhs->getOrigin() != rhs->getOrigin()) {
                return static_cast<int>(lhs->getOrigin()) <
                       static_cast<int>(rhs->getOrigin());
              }
              if (lhs->getMemoryKind() != rhs->getMemoryKind()) {
                return static_cast<int>(lhs->getMemoryKind()) <
                       static_cast<int>(rhs->getMemoryKind());
              }
              return static_cast<int>(lhs->getOriginalEdgeType()) <
                     static_cast<int>(rhs->getOriginalEdgeType());
            });
  return edges;
}

std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
LoopDependenceGraph::internalNodePairs(void) const {
  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>> pairs;
  pairs.reserve(this->internalNodes.size());
  for (auto *node : this->internalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
LoopDependenceGraph::externalNodePairs(void) const {
  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>> pairs;
  pairs.reserve(this->externalNodes.size());
  for (auto *node : this->externalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

LoopDependenceNode *LoopDependenceGraph::getNode(llvm::Value *value) const {
  auto it = this->nodesByValue.find(value);
  return it == this->nodesByValue.end() ? nullptr : it->second;
}

bool LoopDependenceGraph::doesItContain(llvm::Value *value) const {
  return this->getNode(value) != nullptr;
}

bool LoopDependenceGraph::isInternal(llvm::Value *value) const {
  auto *node = this->getNode(value);
  return node != nullptr && node->isInternal();
}

bool LoopDependenceGraph::isExternal(llvm::Value *value) const {
  auto *node = this->getNode(value);
  return node != nullptr && node->isExternal();
}

bool LoopDependenceGraph::iterateOverDependencesTo(
    llvm::Value *target,
    bool includeControl,
    bool includeVariable,
    bool includeMemory,
    const std::function<bool(llvm::Value *, LoopDependenceEdge *)>
        &funcToInvoke) const {
  auto *targetNode = this->getNode(target);
  if (targetNode == nullptr) {
    return false;
  }

  for (auto *edge : targetNode->getIncomingEdges()) {
    bool include = false;
    switch (edge->getKind()) {
    case LoopDependenceEdgeKind::Control:
      include = includeControl;
      break;
    case LoopDependenceEdgeKind::Variable:
      include = includeVariable;
      break;
    case LoopDependenceEdgeKind::Memory:
      include = includeMemory;
      break;
    }
    if (!include) {
      continue;
    }

    auto *src = edge->getSrc();
    if (src != nullptr && funcToInvoke(src->getValue(), edge)) {
      return true;
    }
  }

  return false;
}

bool LoopDependenceGraph::iterateOverDependencesFrom(
    llvm::Value *source,
    bool includeControl,
    bool includeVariable,
    bool includeMemory,
    const std::function<bool(llvm::Value *, LoopDependenceEdge *)>
        &funcToInvoke) const {
  auto *sourceNode = this->getNode(source);
  if (sourceNode == nullptr) {
    return false;
  }

  for (auto *edge : sourceNode->getOutgoingEdges()) {
    bool include = false;
    switch (edge->getKind()) {
    case LoopDependenceEdgeKind::Control:
      include = includeControl;
      break;
    case LoopDependenceEdgeKind::Variable:
      include = includeVariable;
      break;
    case LoopDependenceEdgeKind::Memory:
      include = includeMemory;
      break;
    }
    if (!include) {
      continue;
    }

    auto *dst = edge->getDst();
    if (dst != nullptr && funcToInvoke(dst->getValue(), edge)) {
      return true;
    }
  }

  return false;
}

std::unique_ptr<LoopDependenceGraph> LoopDependenceGraph::createSubgraph(
    bool includeControl,
    bool includeVariable,
    bool includeMemory) const {
  auto subgraph =
      std::unique_ptr<LoopDependenceGraph>(new LoopDependenceGraph());
  subgraph->loop = this->loop;
  subgraph->pdg = this->pdg;

  std::unordered_map<LoopDependenceNode *, LoopDependenceNode *> clonedNodes;
  for (const auto &owned : this->ownedNodes) {
    auto *node = owned.get();
    auto cloned = std::unique_ptr<LoopDependenceNode>(new LoopDependenceNode(
        node->getID(), node->getValue(), node->getPDGNode(), node->isInternal()));
    auto *rawCloned = cloned.get();
    subgraph->ownedNodes.push_back(std::move(cloned));
    if (rawCloned->isInternal()) {
      subgraph->internalNodes.push_back(rawCloned);
    } else {
      subgraph->externalNodes.push_back(rawCloned);
    }
    if (rawCloned->getValue() != nullptr) {
      subgraph->nodesByValue[rawCloned->getValue()] = rawCloned;
    }
    if (rawCloned->getPDGNode() != nullptr) {
      subgraph->nodesByPDGNode[rawCloned->getPDGNode()] = rawCloned;
    }
    clonedNodes[node] = rawCloned;
  }

  for (const auto &owned : this->ownedEdges) {
    auto *edge = owned.get();
    bool keep = false;
    switch (edge->getKind()) {
    case LoopDependenceEdgeKind::Control:
      keep = includeControl;
      break;
    case LoopDependenceEdgeKind::Variable:
      keep = includeVariable;
      break;
    case LoopDependenceEdgeKind::Memory:
      keep = includeMemory;
      break;
    }
    if (!keep) {
      continue;
    }

    auto *src = clonedNodes.at(edge->getSrc());
    auto *dst = clonedNodes.at(edge->getDst());
    subgraph->addEdge(src, dst, edge->getKind(), edge->getMemoryKind(),
                      edge->getOrigin(), edge->getOriginalEdgeType(),
                      edge->isLoopCarried());
  }

  return subgraph;
}

std::unique_ptr<LoopDependenceGraph>
LoopDependenceGraph::createSubgraphFromValues(
    const std::vector<llvm::Value *> &values,
    bool linkToExternal,
    bool includeControl,
    bool includeVariable,
    bool includeMemory,
    const std::unordered_set<LoopDependenceEdge *> &edgesToIgnore) const {
  if (values.empty()) {
    return nullptr;
  }

  auto subgraph =
      std::unique_ptr<LoopDependenceGraph>(new LoopDependenceGraph());
  subgraph->loop = this->loop;
  subgraph->pdg = this->pdg;

  std::unordered_set<llvm::Value *> internalValues;
  for (auto *value : values) {
    if (value == nullptr || shouldIgnoreValue(value)) {
      continue;
    }
    internalValues.insert(value);
    auto *sourceNode = this->getNode(value);
    auto *sourcePDGNode =
        sourceNode ? sourceNode->getPDGNode()
                   : (this->pdg && llvm::isa<llvm::Instruction>(value)
                          ? this->pdg->getNode(*value)
                          : nullptr);
    subgraph->fetchOrCreateNode(value, sourcePDGNode, true);
  }

  for (const auto &owned : this->ownedEdges) {
    auto *edge = owned.get();
    if (edgesToIgnore.count(edge) != 0) {
      continue;
    }

    bool keep = false;
    switch (edge->getKind()) {
    case LoopDependenceEdgeKind::Control:
      keep = includeControl;
      break;
    case LoopDependenceEdgeKind::Variable:
      keep = includeVariable;
      break;
    case LoopDependenceEdgeKind::Memory:
      keep = includeMemory;
      break;
    }
    if (!keep) {
      continue;
    }

    auto *srcValue = edge->getSrc() ? edge->getSrc()->getValue() : nullptr;
    auto *dstValue = edge->getDst() ? edge->getDst()->getValue() : nullptr;
    bool srcInternal = srcValue != nullptr && internalValues.count(srcValue) != 0;
    bool dstInternal = dstValue != nullptr && internalValues.count(dstValue) != 0;
    if (!srcInternal && !dstInternal) {
      continue;
    }
    if (!linkToExternal && (!srcInternal || !dstInternal)) {
      continue;
    }

    auto *srcNode = subgraph->fetchOrCreateNode(
        srcValue, edge->getSrc() ? edge->getSrc()->getPDGNode() : nullptr,
        srcInternal);
    auto *dstNode = subgraph->fetchOrCreateNode(
        dstValue, edge->getDst() ? edge->getDst()->getPDGNode() : nullptr,
        dstInternal);

    subgraph->addEdge(srcNode, dstNode, edge->getKind(), edge->getMemoryKind(),
                      edge->getOrigin(), edge->getOriginalEdgeType(),
                      edge->isLoopCarried());
  }

  return subgraph;
}

void LoopDependenceGraph::removeEdge(LoopDependenceEdge *edge) {
  if (edge == nullptr) {
    return;
  }

  auto *src = edge->getSrc();
  auto *dst = edge->getDst();
  if (src != nullptr) {
    eraseFirst(src->outgoingEdges, edge);
  }
  if (dst != nullptr) {
    eraseFirst(dst->incomingEdges, edge);
  }

  auto it = std::find_if(
      this->ownedEdges.begin(), this->ownedEdges.end(),
      [edge](const std::unique_ptr<LoopDependenceEdge> &owned) {
        return owned.get() == edge;
      });
  if (it != this->ownedEdges.end()) {
    this->ownedEdges.erase(it);
  }
}

void LoopDependenceGraph::addVariableDependence(llvm::Value *srcValue,
                                                llvm::Value *dstValue,
                                                bool loopCarried) {
  auto *loopStructure = this->getLoopStructure();
  assert(loopStructure != nullptr);
  if (!isAllowedBoundaryContextValue(srcValue) ||
      !isAllowedBoundaryContextValue(dstValue)) {
    return;
  }

  auto *srcPDGNode = llvm::isa<llvm::Instruction>(srcValue)
                         ? this->pdg->getNode(*srcValue)
                         : nullptr;
  auto *dstPDGNode = llvm::isa<llvm::Instruction>(dstValue)
                         ? this->pdg->getNode(*dstValue)
                         : nullptr;
  auto *srcNode = this->fetchOrCreateNode(
      srcValue, srcPDGNode, isInternalValueToLoop(loopStructure, srcValue));
  auto *dstNode = this->fetchOrCreateNode(
      dstValue, dstPDGNode, isInternalValueToLoop(loopStructure, dstValue));

  for (auto *edge : srcNode->getOutgoingEdges()) {
    if (edge->getDst() == dstNode &&
        edge->getKind() == LoopDependenceEdgeKind::Variable) {
      return;
    }
  }

  this->addEdge(srcNode, dstNode, LoopDependenceEdgeKind::Variable,
                LoopDependenceMemoryKind::None,
                LoopDependenceEdgeOrigin::SynthesizedBoundaryValue,
                pdg::EdgeType::DATA_DEF_USE, loopCarried);
}

LoopDependenceNode *LoopDependenceGraph::fetchOrCreateNode(llvm::Value *value,
                                                           pdg::Node *pdgNode,
                                                           bool internal) {
  if (pdgNode != nullptr) {
    auto existingByPDG = this->nodesByPDGNode.find(pdgNode);
    if (existingByPDG != this->nodesByPDGNode.end()) {
      if (internal && !existingByPDG->second->internal) {
        existingByPDG->second->internal = true;
        eraseFirst(this->externalNodes, existingByPDG->second);
        this->internalNodes.push_back(existingByPDG->second);
      }
      return existingByPDG->second;
    }
  }

  if (value != nullptr) {
    auto existingByValue = this->nodesByValue.find(value);
    if (existingByValue != this->nodesByValue.end()) {
      if (internal && !existingByValue->second->internal) {
        existingByValue->second->internal = true;
        eraseFirst(this->externalNodes, existingByValue->second);
        this->internalNodes.push_back(existingByValue->second);
      }
      return existingByValue->second;
    }
  }

  auto node = std::unique_ptr<LoopDependenceNode>(new LoopDependenceNode(
      this->ownedNodes.size(), value, pdgNode, internal));
  auto *rawNode = node.get();
  this->ownedNodes.push_back(std::move(node));
  if (internal) {
    this->internalNodes.push_back(rawNode);
  } else {
    this->externalNodes.push_back(rawNode);
  }
  if (value != nullptr) {
    this->nodesByValue[value] = rawNode;
  }
  if (pdgNode != nullptr) {
    this->nodesByPDGNode[pdgNode] = rawNode;
  }
  return rawNode;
}

void LoopDependenceGraph::importEdgeIfIncluded(
    pdg::Edge *edge,
    bool linkToExternal,
    const std::unordered_set<llvm::Value *> &internalValues,
    std::unordered_set<pdg::Edge *> &seenEdges) {
  if (edge == nullptr) {
    return;
  }

  auto *srcPDGNode = edge->getSrcNode();
  auto *dstPDGNode = edge->getDstNode();
  auto *srcValue = srcPDGNode ? srcPDGNode->getValue() : nullptr;
  auto *dstValue = dstPDGNode ? dstPDGNode->getValue() : nullptr;
  if (!isAllowedBoundaryContextValue(srcValue) ||
      !isAllowedBoundaryContextValue(dstValue)) {
    return;
  }

  bool srcInternal = srcValue != nullptr && internalValues.count(srcValue) != 0;
  bool dstInternal = dstValue != nullptr && internalValues.count(dstValue) != 0;
  if (!srcInternal && !dstInternal) {
    return;
  }
  if (!linkToExternal && (!srcInternal || !dstInternal)) {
    return;
  }

  if (!seenEdges.insert(edge).second) {
    return;
  }

  auto *srcNode = this->fetchOrCreateNode(srcValue, srcPDGNode, srcInternal);
  auto *dstNode = this->fetchOrCreateNode(dstValue, dstPDGNode, dstInternal);
  this->addEdge(srcNode, dstNode, classifyEdgeKind(edge->getEdgeType()),
                classifyMemoryKind(edge->getEdgeType()),
                LoopDependenceEdgeOrigin::ImportedPDG, edge->getEdgeType(),
                false);
}

void LoopDependenceGraph::initialize(LoopTree *loopNode,
                                     pdg::ProgramGraph &programGraph) {
  this->loop = loopNode;
  this->pdg = &programGraph;
}

void LoopDependenceGraph::addEdge(LoopDependenceNode *src,
                                  LoopDependenceNode *dst,
                                  LoopDependenceEdgeKind kind,
                                  LoopDependenceMemoryKind memoryKind,
                                  LoopDependenceEdgeOrigin origin,
                                  pdg::EdgeType originalEdgeType,
                                  bool loopCarried) {
  auto ownedEdge = std::unique_ptr<LoopDependenceEdge>(new LoopDependenceEdge(
      src, dst, kind, memoryKind, origin, originalEdgeType, loopCarried));
  auto *rawEdge = ownedEdge.get();
  src->outgoingEdges.push_back(rawEdge);
  dst->incomingEdges.push_back(rawEdge);
  this->ownedEdges.push_back(std::move(ownedEdge));
}

std::string LoopDependenceGraph::renderStable(void) const {
  auto orderedNodes = this->getNodes();
  std::sort(orderedNodes.begin(), orderedNodes.end(),
            [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
              return lhs->getID() < rhs->getID();
            });
  auto orderedEdges = this->getSortedDependences();

  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << "nodes\n";
  for (auto *node : orderedNodes) {
    stream << "  [" << node->getID() << "] "
           << (node->isInternal() ? "internal " : "external ")
           << describeValue(node->getValue()) << "\n";
  }
  stream << "edges\n";
  for (auto *edge : orderedEdges) {
    stream << "  [" << edge->getSrc()->getID() << " -> "
           << edge->getDst()->getID() << "] kind="
           << describeEdgeKind(edge->getKind()) << " mem="
           << describeMemoryKind(edge->getMemoryKind()) << " origin="
           << describeOrigin(edge->getOrigin()) << " carried="
           << (edge->isLoopCarried() ? "true" : "false") << "\n";
  }
  return stream.str();
}

} // namespace loop
} // namespace analysis
} // namespace lotus
