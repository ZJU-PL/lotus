/*
 * Copyright 2019 - 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopForest.h"

#include "Analysis/CFG/Dominator.h"

#include <cassert>

namespace lotus {
namespace analysis {
namespace loop {

LoopTree::LoopTree(LoopForest *forest, LoopStructure *loop)
    : LoopTree(forest, loop, nullptr) {}

LoopTree::LoopTree(LoopForest *forest, LoopStructure *loop, LoopTree *parent)
    : forest{forest}, loop{loop}, parent{parent} {}

LoopTree::~LoopTree() = default;

LoopStructure *LoopTree::getLoop(void) const { return this->loop; }

bool LoopTree::isIncludedInItsSubLoops(llvm::Instruction *inst) const {
  if (!this->loop->isIncluded(inst)) {
    return false;
  }

  for (auto *subLoopNode : this->children) {
    auto *subLoop = subLoopNode->getLoop();
    assert(subLoop != nullptr);
    if (subLoop->isIncluded(inst)) {
      return true;
    }
  }

  return false;
}

uint32_t LoopTree::getNumberOfSubLoops(void) const {
  uint32_t subloops = 0;
  for (auto *subLoop : this->children) {
    ++subloops;
    subloops += subLoop->getNumberOfSubLoops();
  }
  return subloops;
}

LoopStructure *LoopTree::getInnermostLoopThatContains(llvm::Instruction *inst) {
  return this->getInnermostLoopThatContains(inst->getParent());
}

LoopStructure *LoopTree::getInnermostLoopThatContains(llvm::BasicBlock *bb) {
  if (!this->loop->isIncluded(bb)) {
    return nullptr;
  }

  LoopStructure *innerLoop = nullptr;
  uint32_t innerLoopLevel = 0;
  this->visitPreOrder([bb, &innerLoop, &innerLoopLevel](LoopTree *node,
                                                        uint32_t treeLevel) {
    auto *nl = node->getLoop();
    if (!nl->isIncluded(bb)) {
      return false;
    }
    if (innerLoop == nullptr || treeLevel > innerLoopLevel) {
      innerLoop = nl;
      innerLoopLevel = treeLevel;
    }
    return false;
  });

  return innerLoop;
}

LoopStructure *LoopTree::getOutermostLoopThatContains(llvm::Instruction *inst) {
  return this->getOutermostLoopThatContains(inst->getParent());
}

LoopStructure *LoopTree::getOutermostLoopThatContains(llvm::BasicBlock *bb) {
  if (!this->loop->isIncluded(bb)) {
    return nullptr;
  }

  LoopStructure *outerLoop = nullptr;
  uint32_t outerLoopLevel = 0;
  this->visitPreOrder([bb, &outerLoop, &outerLoopLevel](LoopTree *node,
                                                        uint32_t treeLevel) {
    auto *nl = node->getLoop();
    if (!nl->isIncluded(bb)) {
      return false;
    }
    if (outerLoop == nullptr || treeLevel < outerLoopLevel) {
      outerLoop = nl;
      outerLoopLevel = treeLevel;
    }
    return false;
  });

  return outerLoop;
}

LoopTree *LoopTree::getParent(void) const { return this->parent; }

std::unordered_set<LoopTree *> LoopTree::getDescendants(void) {
  std::unordered_set<LoopTree *> descendants;
  this->visitPreOrder([this, &descendants](LoopTree *node, uint32_t) {
    if (node != this) {
      descendants.insert(node);
    }
    return false;
  });
  return descendants;
}

std::unordered_set<LoopTree *> LoopTree::getChildren(void) const {
  return this->children;
}

std::set<LoopTree *> LoopTree::getNodes(void) {
  std::set<LoopTree *> nodes;
  this->visitPreOrder([&nodes](LoopTree *node, uint32_t) {
    nodes.insert(node);
    return false;
  });
  return nodes;
}

std::set<LoopStructure *> LoopTree::getLoops(void) {
  std::set<LoopStructure *> loops;
  this->visitPreOrder([&loops](LoopTree *node, uint32_t) {
    loops.insert(node->getLoop());
    return false;
  });
  return loops;
}

bool LoopTree::visitPreOrder(
    std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke) {
  return this->visitPreOrder(std::move(funcToInvoke), 1);
}

bool LoopTree::visitPostOrder(
    std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke) {
  return this->visitPostOrder(std::move(funcToInvoke), 1);
}

bool LoopTree::visitPreOrder(
    std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke,
    uint32_t treeLevel) {
  if (funcToInvoke(this, treeLevel)) {
    return true;
  }

  for (auto *child : this->children) {
    if (child->visitPreOrder(funcToInvoke, treeLevel + 1)) {
      return true;
    }
  }

  return false;
}

bool LoopTree::visitPostOrder(
    std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke,
    uint32_t treeLevel) {
  for (auto *child : this->children) {
    if (child->visitPostOrder(funcToInvoke, treeLevel + 1)) {
      return true;
    }
  }

  return funcToInvoke(this, treeLevel);
}

LoopForest::LoopForest(
    std::vector<LoopStructure *> const &loops,
    std::unordered_map<llvm::Function *, noelle::DominatorSummary *> const
        &doms) {
  std::unordered_set<LoopTree *> potentialTrees;
  for (auto *loop : loops) {
    auto *func = loop->getFunction();
    auto *header = loop->getHeader();
    auto ownedNode = std::make_unique<LoopTree>(this, loop);
    auto *node = ownedNode.get();
    this->nodes[loop] = std::move(ownedNode);
    this->functionLoops[func].insert(loop);
    this->headerLoops[header] = node;

    if (loop->getNestingLevel() == 1) {
      this->trees.insert(node);
    } else {
      potentialTrees.insert(node);
    }
  }

  for (auto *tree : this->trees) {
    this->addChildrenToTree(tree, doms, potentialTrees);
  }

  for (auto *node : potentialTrees) {
    this->trees.insert(node);
  }
}

LoopForest::~LoopForest() = default;

uint64_t LoopForest::getNumberOfLoops(void) const {
  uint64_t total = 0;
  for (auto *tree : this->getTrees()) {
    ++total;
    total += tree->getNumberOfSubLoops();
  }
  return total;
}

void LoopForest::addChildrenToTree(
    LoopTree *root,
    std::unordered_map<llvm::Function *, noelle::DominatorSummary *> const
        &doms,
    std::unordered_set<LoopTree *> &potentialTrees) {
  auto *loop = root->getLoop();
  auto *header = loop->getHeader();
  auto *func = loop->getFunction();

  auto *ds = doms.at(func);
  auto *loopHeaderDominatorNode = ds->DT.getNode(header);

  for (auto *functionLoop : this->functionLoops[func]) {
    auto *functionLoopHeader = functionLoop->getHeader();
    if (functionLoopHeader == header || !loop->isIncluded(functionLoopHeader)) {
      continue;
    }

    auto *subLoopDominatorNode = ds->DT.getNode(functionLoopHeader);
    assert(loopHeaderDominatorNode != subLoopDominatorNode);
    subLoopDominatorNode = subLoopDominatorNode->getParent();

    bool foundLoopInBetween = false;
    while (loopHeaderDominatorNode != subLoopDominatorNode) {
      auto *bb = subLoopDominatorNode->getBlock();
      auto it = this->headerLoops.find(bb);
      if (it != this->headerLoops.end() &&
          it->second->getLoop()->isIncluded(functionLoopHeader)) {
        foundLoopInBetween = true;
        break;
      }
      subLoopDominatorNode = subLoopDominatorNode->getParent();
    }
    if (foundLoopInBetween) {
      continue;
    }

    auto *child = this->nodes[functionLoop].get();
    assert(child != nullptr);
    root->children.insert(child);
    child->parent = root;
    potentialTrees.erase(child);
    this->addChildrenToTree(child, doms, potentialTrees);
  }
}

std::unordered_set<LoopTree *> LoopForest::getTrees(void) const {
  return this->trees;
}

void LoopForest::removeTree(LoopTree *tree) { this->trees.erase(tree); }

void LoopForest::addTree(LoopTree *tree) { this->trees.insert(tree); }

LoopTree *LoopForest::getNode(LoopStructure *loop) const {
  auto it = this->headerLoops.find(loop->getHeader());
  return it == this->headerLoops.end() ? nullptr : it->second;
}

LoopTree *LoopForest::getInnermostLoopThatContains(
    llvm::Instruction *inst) const {
  for (auto *tree : this->getTrees()) {
    auto *ls = tree->getLoop();
    if (ls->getFunction() != inst->getFunction() || !ls->isIncluded(inst)) {
      continue;
    }

    LoopTree *innermostLoop = nullptr;
    tree->visitPostOrder([inst, &innermostLoop](LoopTree *node, uint32_t) {
      if (node->getLoop()->isIncluded(inst)) {
        innermostLoop = node;
        return true;
      }
      return false;
    });
    return innermostLoop;
  }

  return nullptr;
}

LoopTree *LoopForest::getInnermostLoopThatContains(llvm::BasicBlock *bb) const {
  return bb->empty() ? nullptr
                     : this->getInnermostLoopThatContains(&*bb->begin());
}

} // namespace loop
} // namespace analysis
} // namespace lotus
