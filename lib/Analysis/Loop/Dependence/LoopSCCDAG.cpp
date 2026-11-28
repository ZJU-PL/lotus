/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Dependence/LoopSCCDAG.h"

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <algorithm>
#include <cassert>
#include <queue>
#include <set>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

struct TarjanState {
  uint64_t nextIndex{0};
  std::unordered_map<LoopDependenceNode *, uint64_t> index;
  std::unordered_map<LoopDependenceNode *, uint64_t> lowlink;
  std::unordered_set<LoopDependenceNode *> onStack;
  std::vector<LoopDependenceNode *> stack;
};

void strongConnect(LoopDependenceNode *node,
                   TarjanState &state,
                   std::vector<std::vector<LoopDependenceNode *>> &components) {
  state.index[node] = state.nextIndex;
  state.lowlink[node] = state.nextIndex;
  state.nextIndex++;
  state.stack.push_back(node);
  state.onStack.insert(node);

  for (auto *edge : node->getOutgoingEdges()) {
    auto *succ = edge->getDst();
    if (succ == nullptr) {
      continue;
    }

    if (state.index.find(succ) == state.index.end()) {
      strongConnect(succ, state, components);
      state.lowlink[node] = std::min(state.lowlink[node], state.lowlink[succ]);
    } else if (state.onStack.count(succ) != 0) {
      state.lowlink[node] = std::min(state.lowlink[node], state.index[succ]);
    }
  }

  if (state.lowlink[node] != state.index[node]) {
    return;
  }

  std::vector<LoopDependenceNode *> component;
  while (!state.stack.empty()) {
    auto *current = state.stack.back();
    state.stack.pop_back();
    state.onStack.erase(current);
    component.push_back(current);
    if (current == node) {
      break;
    }
  }

  std::sort(component.begin(), component.end(),
            [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
              return lhs->getID() < rhs->getID();
            });
  components.push_back(std::move(component));
}

bool componentHasCycle(std::vector<LoopDependenceNode *> const &members) {
  if (members.size() > 1) {
    return true;
  }

  auto *single = members.front();
  for (auto *edge : single->getOutgoingEdges()) {
    if (edge->getDst() == single) {
      return true;
    }
  }
  return false;
}

} // namespace

LoopSCCDAGEdge::LoopSCCDAGEdge(LoopSCC *src, LoopSCC *dst)
    : src{src}, dst{dst} {}

LoopSCC *LoopSCCDAGEdge::getSrc(void) const { return this->src; }

LoopSCC *LoopSCCDAGEdge::getDst(void) const { return this->dst; }

std::vector<LoopDependenceEdge *> LoopSCCDAGEdge::getSubEdges(void) const {
  auto edges = this->subEdges;
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
              if (lhs->getMemoryKind() != rhs->getMemoryKind()) {
                return static_cast<int>(lhs->getMemoryKind()) <
                       static_cast<int>(rhs->getMemoryKind());
              }
              return static_cast<int>(lhs->getOrigin()) <
                     static_cast<int>(rhs->getOrigin());
            });
  return edges;
}

uint64_t LoopSCCDAGEdge::getNumberOfSubEdges(void) const {
  return this->subEdges.size();
}

bool LoopSCCDAGEdge::isLoopCarried(void) const {
  for (auto *edge : this->subEdges) {
    if (edge != nullptr && edge->isLoopCarried()) {
      return true;
    }
  }
  return false;
}

void LoopSCCDAGEdge::addSubEdge(LoopDependenceEdge *edge) {
  if (edge != nullptr) {
    this->subEdges.push_back(edge);
  }
}

LoopSCC::LoopSCC(uint64_t id,
                 std::vector<LoopDependenceNode *> members,
                 bool hasCycle,
                 bool includedInLoop)
    : id{id}, internalNodes{std::move(members)}, cycle{hasCycle},
      includedInLoop{includedInLoop} {}

uint64_t LoopSCC::getID(void) const { return this->id; }

bool LoopSCC::hasCycle(void) const { return this->cycle; }

bool LoopSCC::isIncludedInLoop(void) const { return this->includedInLoop; }

std::vector<LoopDependenceNode *> LoopSCC::getNodes(void) const {
  std::vector<LoopDependenceNode *> nodes;
  nodes.reserve(this->internalNodes.size() + this->externalNodes.size());
  nodes.insert(nodes.end(), this->internalNodes.begin(),
               this->internalNodes.end());
  nodes.insert(nodes.end(), this->externalNodes.begin(),
               this->externalNodes.end());
  return nodes;
}

std::vector<LoopSCC *> LoopSCC::getPredecessors(void) const {
  return this->predecessors;
}

std::vector<LoopSCC *> LoopSCC::getSuccessors(void) const {
  return this->successors;
}

std::vector<LoopSCCDAGEdge *> LoopSCC::getIncomingEdges(void) const {
  return this->incomingEdges;
}

std::vector<LoopSCCDAGEdge *> LoopSCC::getOutgoingEdges(void) const {
  return this->outgoingEdges;
}

std::vector<LoopDependenceEdge *> LoopSCC::getEdges(void) const {
  std::vector<LoopDependenceEdge *> edges;
  std::set<LoopDependenceEdge *> seen;
  for (auto *node : this->internalNodes) {
    for (auto *edge : node->getOutgoingEdges()) {
      if (edge != nullptr && seen.insert(edge).second) {
        edges.push_back(edge);
      }
    }
    for (auto *edge : node->getIncomingEdges()) {
      if (edge != nullptr && seen.insert(edge).second) {
        edges.push_back(edge);
      }
    }
  }
  return edges;
}

std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
LoopSCC::internalNodePairs(void) const {
  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>> pairs;
  pairs.reserve(this->internalNodes.size());
  for (auto *node : this->internalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
LoopSCC::externalNodePairs(void) const {
  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>> pairs;
  pairs.reserve(this->externalNodes.size());
  for (auto *node : this->externalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

bool LoopSCC::isInternal(llvm::Value *value) const {
  for (auto *node : this->internalNodes) {
    if (node->getValue() == value) {
      return true;
    }
  }
  return false;
}

bool LoopSCC::isExternal(llvm::Value *value) const {
  for (auto *node : this->externalNodes) {
    if (node->getValue() == value) {
      return true;
    }
  }
  return false;
}

LoopDependenceNode *LoopSCC::fetchNode(llvm::Value *value) const {
  for (auto *node : this->internalNodes) {
    if (node->getValue() == value) {
      return node;
    }
  }
  for (auto *node : this->externalNodes) {
    if (node->getValue() == value) {
      return node;
    }
  }
  return nullptr;
}

int64_t LoopSCC::numberOfInstructions(void) const {
  int64_t count = 0;
  for (auto *node : this->internalNodes) {
    if (llvm::isa<llvm::Instruction>(node->getValue())) {
      ++count;
    }
  }
  return count;
}

bool LoopSCC::iterateOverInstructions(
    const std::function<bool(llvm::Instruction *)> &funcToInvoke) const {
  for (auto *node : this->internalNodes) {
    if (auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(node->getValue())) {
      if (funcToInvoke(inst)) {
        return true;
      }
    }
  }
  return false;
}

bool LoopSCC::iterateOverAllInstructions(
    const std::function<bool(llvm::Instruction *)> &funcToInvoke) const {
  for (auto *node : this->getNodes()) {
    if (auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(node->getValue())) {
      if (funcToInvoke(inst)) {
        return true;
      }
    }
  }
  return false;
}

bool LoopSCC::iterateOverValues(
    const std::function<bool(llvm::Value *)> &funcToInvoke) const {
  for (auto *node : this->internalNodes) {
    if (funcToInvoke(node->getValue())) {
      return true;
    }
  }
  return false;
}

bool LoopSCC::iterateOverAllValues(
    const std::function<bool(llvm::Value *)> &funcToInvoke) const {
  for (auto *node : this->getNodes()) {
    if (funcToInvoke(node->getValue())) {
      return true;
    }
  }
  return false;
}

LoopSCCDAG::LoopSCCDAG(LoopDependenceGraph &graph) : graph{&graph} {
  TarjanState state;
  std::vector<std::vector<LoopDependenceNode *>> components;

  this->internalNodes = graph.getInternalNodes();
  this->allNodes = graph.getNodes();
  std::sort(this->allNodes.begin(), this->allNodes.end(),
            [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
              return lhs->getID() < rhs->getID();
            });

  for (auto *node : this->allNodes) {
    if (state.index.find(node) == state.index.end()) {
      strongConnect(node, state, components);
    }
  }

  std::sort(components.begin(), components.end(),
            [](std::vector<LoopDependenceNode *> const &lhs,
               std::vector<LoopDependenceNode *> const &rhs) {
              return lhs.front()->getID() < rhs.front()->getID();
            });

  for (auto &component : components) {
    bool includedInLoop = false;
    for (auto *node : component) {
      includedInLoop |= node->isInternal();
    }
    auto owned = std::unique_ptr<LoopSCC>(new LoopSCC(
        this->ownedSCCs.size(), component, componentHasCycle(component),
        includedInLoop));
    auto *raw = owned.get();
    this->ownedSCCs.push_back(std::move(owned));
    this->allSCCs.push_back(raw);
    if (includedInLoop) {
      this->includedSCCs.push_back(raw);
    }
    for (auto *node : component) {
      this->sccByNode[node] = raw;
      auto *value = node->getValue();
      if (value != nullptr) {
        this->sccByValue[value] = raw;
      }
    }
  }

  auto nodeSorter = [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
    return lhs->getID() < rhs->getID();
  };
  for (auto &ownedSCC : this->ownedSCCs) {
    auto *scc = ownedSCC.get();
    std::set<LoopDependenceNode *> members(scc->internalNodes.begin(),
                                           scc->internalNodes.end());
    std::set<LoopDependenceNode *> externalNodes;
    for (auto *node : scc->internalNodes) {
      for (auto *edge : node->getOutgoingEdges()) {
        auto *dst = edge ? edge->getDst() : nullptr;
        if (dst != nullptr && members.count(dst) == 0) {
          externalNodes.insert(dst);
        }
      }
      for (auto *edge : node->getIncomingEdges()) {
        auto *src = edge ? edge->getSrc() : nullptr;
        if (src != nullptr && members.count(src) == 0) {
          externalNodes.insert(src);
        }
      }
    }
    scc->externalNodes.assign(externalNodes.begin(), externalNodes.end());
    std::sort(scc->internalNodes.begin(), scc->internalNodes.end(), nodeSorter);
    std::sort(scc->externalNodes.begin(), scc->externalNodes.end(), nodeSorter);
  }

  for (auto *node : this->allNodes) {
    auto *srcSCC = this->sccByNode.at(node);
    for (auto *edge : node->getOutgoingEdges()) {
      auto *dst = edge->getDst();
      if (dst == nullptr) {
        continue;
      }
      auto dstIt = this->sccByNode.find(dst);
      if (dstIt == this->sccByNode.end()) {
        continue;
      }
      auto *dstSCC = dstIt->second;
      if (srcSCC == dstSCC) {
        continue;
      }
      auto key = std::make_pair(srcSCC, dstSCC);
      auto edgeIt = this->edgeBySCCPair.find(key);
      LoopSCCDAGEdge *sccEdge = nullptr;
      if (edgeIt == this->edgeBySCCPair.end()) {
        auto ownedEdge =
            std::unique_ptr<LoopSCCDAGEdge>(new LoopSCCDAGEdge(srcSCC, dstSCC));
        sccEdge = ownedEdge.get();
        this->ownedEdges.push_back(std::move(ownedEdge));
        this->edgeBySCCPair[key] = sccEdge;
        srcSCC->successors.push_back(dstSCC);
        dstSCC->predecessors.push_back(srcSCC);
        srcSCC->outgoingEdges.push_back(sccEdge);
        dstSCC->incomingEdges.push_back(sccEdge);
      } else {
        sccEdge = edgeIt->second;
      }
      sccEdge->addSubEdge(edge);
    }
  }

  for (auto &ownedSCC : this->ownedSCCs) {
    std::sort(ownedSCC->predecessors.begin(), ownedSCC->predecessors.end(),
              [](LoopSCC *lhs, LoopSCC *rhs) {
                return lhs->getID() < rhs->getID();
              });
    std::sort(ownedSCC->successors.begin(), ownedSCC->successors.end(),
              [](LoopSCC *lhs, LoopSCC *rhs) {
                return lhs->getID() < rhs->getID();
              });
    std::sort(ownedSCC->incomingEdges.begin(), ownedSCC->incomingEdges.end(),
              [](LoopSCCDAGEdge *lhs, LoopSCCDAGEdge *rhs) {
                return lhs->getSrc()->getID() < rhs->getSrc()->getID();
              });
    std::sort(ownedSCC->outgoingEdges.begin(), ownedSCC->outgoingEdges.end(),
              [](LoopSCCDAGEdge *lhs, LoopSCCDAGEdge *rhs) {
                return lhs->getDst()->getID() < rhs->getDst()->getID();
              });
  }
  this->computeReachabilityAmongSCCs();
}

LoopDependenceGraph *LoopSCCDAG::getLoopDependenceGraph(void) const {
  return this->graph;
}

std::vector<LoopSCC *> LoopSCCDAG::getSCCs(void) const {
  return this->getInternalSCCs();
}

std::vector<LoopSCC *> LoopSCCDAG::getInternalSCCs(void) const {
  auto sccs = this->includedSCCs;
  std::sort(sccs.begin(), sccs.end(),
            [](LoopSCC *lhs, LoopSCC *rhs) {
              return lhs->getID() < rhs->getID();
            });
  return sccs;
}

std::vector<LoopSCC *> LoopSCCDAG::getExternalSCCs(void) const {
  std::vector<LoopSCC *> sccs;
  for (auto *scc : this->allSCCs) {
    if (scc != nullptr && !scc->isIncludedInLoop()) {
      sccs.push_back(scc);
    }
  }
  std::sort(sccs.begin(), sccs.end(),
            [](LoopSCC *lhs, LoopSCC *rhs) {
              return lhs->getID() < rhs->getID();
            });
  return sccs;
}

std::vector<LoopSCC *> LoopSCCDAG::getAllSCCs(void) const {
  auto sccs = this->allSCCs;
  std::sort(sccs.begin(), sccs.end(),
            [](LoopSCC *lhs, LoopSCC *rhs) {
              return lhs->getID() < rhs->getID();
            });
  return sccs;
}

std::vector<LoopSCCDAGEdge *> LoopSCCDAG::getEdges(void) const {
  std::vector<LoopSCCDAGEdge *> edges;
  edges.reserve(this->ownedEdges.size());
  for (const auto &edge : this->ownedEdges) {
    edges.push_back(edge.get());
  }
  std::sort(edges.begin(), edges.end(),
            [](LoopSCCDAGEdge *lhs, LoopSCCDAGEdge *rhs) {
              if (lhs->getSrc()->getID() != rhs->getSrc()->getID()) {
                return lhs->getSrc()->getID() < rhs->getSrc()->getID();
              }
              return lhs->getDst()->getID() < rhs->getDst()->getID();
            });
  return edges;
}

LoopSCC *LoopSCCDAG::getSCC(llvm::Value *value) const {
  auto it = this->sccByValue.find(value);
  return it == this->sccByValue.end() ? nullptr : it->second;
}

bool LoopSCCDAG::doesItContain(llvm::Instruction *inst) const {
  return this->getSCC(inst) != nullptr;
}

bool LoopSCCDAG::iterateOverLiveInAndLiveOut(
    const std::function<bool(llvm::Value *)> &funcToInvoke) const {
  for (auto *scc : this->getExternalSCCs()) {
    if (scc != nullptr && scc->iterateOverValues(funcToInvoke)) {
      return true;
    }
  }
  return false;
}

bool LoopSCCDAG::orderedBefore(const LoopSCC *early,
                               const LoopSCC *late) const {
  auto it = this->reachableSCCs.find(early);
  return it != this->reachableSCCs.end() && it->second.count(late) != 0;
}

bool LoopSCCDAG::orderedBefore(const LoopSCC *early,
                               const SCCSet &lates) const {
  for (auto *late : lates) {
    if (this->orderedBefore(early, late)) {
      return true;
    }
  }
  return false;
}

bool LoopSCCDAG::orderedBefore(const SCCSet &earlies,
                               const LoopSCC *late) const {
  for (auto *early : earlies) {
    if (this->orderedBefore(early, late)) {
      return true;
    }
  }
  return false;
}

uint32_t LoopSCCDAG::getSCCIndex(const LoopSCC *scc) const {
  auto it = this->sccIndexes.find(scc);
  assert(it != this->sccIndexes.end());
  return it->second;
}

void LoopSCCDAG::computeReachabilityAmongSCCs(void) {
  this->reachableSCCs.clear();
  this->sccIndexes.clear();
  uint32_t index = 0;
  for (const auto &owned : this->ownedSCCs) {
    this->sccIndexes.emplace(owned.get(), index++);
  }

  for (const auto &owned : this->ownedSCCs) {
    auto *root = owned.get();
    std::queue<const LoopSCC *> queue;
    std::unordered_set<const LoopSCC *> visited;
    for (auto *succ : root->getSuccessors()) {
      queue.push(succ);
    }
    while (!queue.empty()) {
      auto *current = queue.front();
      queue.pop();
      if (!visited.insert(current).second) {
        continue;
      }
      this->reachableSCCs[root].insert(current);
      for (auto *succ : current->getSuccessors()) {
        queue.push(succ);
      }
    }
  }
}

} // namespace loop
} // namespace analysis
} // namespace lotus
