/** @file LoopForest.h @brief Loop forest data structure representing loop nesting hierarchies. */
/*
 * Copyright 2026 Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "Analysis/Loop/LoopStructure.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noelle {
class DominatorSummary;
} // namespace noelle

namespace lotus {
namespace analysis {
namespace loop {

class LoopForest;

class LoopTree {
public:
  LoopTree(LoopForest *forest, LoopStructure *loop);
  LoopTree(LoopForest *forest, LoopStructure *loop, LoopTree *parent);
  ~LoopTree();

  LoopStructure *getLoop(void) const;

  LoopStructure *getInnermostLoopThatContains(llvm::Instruction *inst);
  LoopStructure *getInnermostLoopThatContains(llvm::BasicBlock *bb);

  LoopStructure *getOutermostLoopThatContains(llvm::Instruction *inst);
  LoopStructure *getOutermostLoopThatContains(llvm::BasicBlock *bb);

  std::set<LoopTree *> getNodes(void);
  std::set<LoopStructure *> getLoops(void);

  LoopTree *getParent(void) const;
  std::unordered_set<LoopTree *> getChildren(void) const;
  std::unordered_set<LoopTree *> getDescendants(void);

  bool isIncludedInItsSubLoops(llvm::Instruction *inst) const;
  uint32_t getNumberOfSubLoops(void) const;

  bool visitPreOrder(
      std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke);
  bool visitPostOrder(
      std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke);

private:
  friend class LoopForest;

  LoopForest *forest;
  LoopStructure *loop;
  LoopTree *parent;
  std::unordered_set<LoopTree *> children;

  bool visitPreOrder(
      std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke,
      uint32_t treeLevel);
  bool visitPostOrder(
      std::function<bool(LoopTree *node, uint32_t treeLevel)> funcToInvoke,
      uint32_t treeLevel);
};

class LoopForest {
public:
  LoopForest(
      std::vector<LoopStructure *> const &loops,
      std::unordered_map<llvm::Function *, noelle::DominatorSummary *> const
          &doms);
  ~LoopForest();

  uint64_t getNumberOfLoops(void) const;

  std::unordered_set<LoopTree *> getTrees(void) const;

  void removeTree(LoopTree *tree);
  void addTree(LoopTree *tree);

  LoopTree *getNode(LoopStructure *loop) const;
  LoopTree *getInnermostLoopThatContains(llvm::Instruction *inst) const;
  LoopTree *getInnermostLoopThatContains(llvm::BasicBlock *bb) const;

private:
  std::unordered_map<LoopStructure *, std::unique_ptr<LoopTree>> nodes;
  std::unordered_set<LoopTree *> trees;
  std::unordered_map<llvm::Function *, std::unordered_set<LoopStructure *>>
      functionLoops;
  std::unordered_map<llvm::BasicBlock *, LoopTree *> headerLoops;

  void addChildrenToTree(
      LoopTree *root,
      std::unordered_map<llvm::Function *, noelle::DominatorSummary *> const
          &doms,
      std::unordered_set<LoopTree *> &potentialTrees);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
