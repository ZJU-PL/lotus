#pragma once

#include "Checker/GSAF/Engine/Solver.h"
#include "IR/GVFG/GuardedValueFlowSerializer.h"

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace lotus::gsaf {

using gvfg::GuardedValueFlowArgumentNode;
using gvfg::GuardedValueFlowCallOutputNode;
using gvfg::GuardedValueFlowCallSite;
using gvfg::GuardedValueFlowDereferenceSite;
using gvfg::GuardedValueFlowDivSite;
using gvfg::GuardedValueFlowGEPReferenceSite;
using gvfg::GuardedValueFlowGraph;
using gvfg::GuardedValueFlowGraphBuilderPass;
using gvfg::GuardedValueFlowNode;
using gvfg::GuardedValueFlowObject;
using gvfg::GuardedValueFlowOpcodeNode;
using gvfg::GuardedValueFlowPhiNode;
using gvfg::GuardedValueFlowRegionNode;
using gvfg::GuardedValueFlowReturnNode;
using gvfg::GuardedValueFlowReturnSite;
using gvfg::GuardedValueFlowSite;
using gvfg::GuardedValueFlowSolver;

/// Orders graph-owned trace references without copying their IR objects.
struct ObjectLess {
  bool operator()(const gvfg::GuardedValueFlowObject *left,
                  const gvfg::GuardedValueFlowObject *right) const;
};

GuardedValueFlowCallSite *
callSite(const gvfg::GuardedValueFlowCallSummaryNode *node);

GuardedValueFlowCallSite *
callSite(const GuardedValueFlowCallOutputNode *node);

llvm::BasicBlock *immediateDominator(const llvm::DominatorTree &tree,
                                     llvm::BasicBlock *block);

bool hasContradictoryRegions(
    const std::vector<GuardedValueFlowRegionNode *> &regions,
    const GuardedValueFlowRegionNode *extra = nullptr);

bool isOperand(const GuardedValueFlowObject *object);

const GuardedValueFlowNode *operandNode(const GuardedValueFlowObject *object);

const GuardedValueFlowSite *operationalSite(const GuardedValueFlowSite *site);

GuardedValueFlowGraph *
graphFor(GuardedValueFlowGraphBuilderPass *builder, llvm::Function *function);

std::vector<GuardedValueFlowNode *>
valueFlowParents(const GuardedValueFlowNode *node, bool arithmetic);

std::vector<GuardedValueFlowCallSite *>
callSites(const GuardedValueFlowGraph *graph);

std::vector<const GuardedValueFlowCallOutputNode *>
callOutputs(const GuardedValueFlowCallSite *site, llvm::Function *callee);

bool isGEPOpcode(const GuardedValueFlowNode *node);

llvm::Instruction *gepMemoryUse(const GuardedValueFlowGEPReferenceSite *site);

std::vector<GuardedValueFlowNode *> children(const GuardedValueFlowNode *node);

std::string smtText(const SMTExprVec &values);

std::pair<SMTExprVec, SMTExprVec>
ctrlDepsPair(GuardedValueFlowSolver &solver, llvm::BasicBlock *block,
             const GuardedValueFlowGraph *graph,
             llvm::BasicBlock *previous_block);

template <GuardedValueFlowNode::Kind Kind,
          typename NodeType = GuardedValueFlowNode>
const NodeType *nodeOfKind(const GuardedValueFlowObject *object) {
  auto *node = llvm::dyn_cast_or_null<GuardedValueFlowNode>(object);
  return node && node->getKind() == Kind ? llvm::dyn_cast<NodeType>(node)
                                         : nullptr;
}

template <typename Tree>
std::vector<llvm::BasicBlock *> properDominators(const Tree *tree,
                                                 llvm::BasicBlock *block) {
  std::vector<llvm::BasicBlock *> result;
  auto *node = tree->getNode(block);
  if (!node)
    return result;
  for (node = node->getIDom(); node; node = node->getIDom())
    if (node->getBlock())
      result.push_back(node->getBlock());
  return result;
}

template <typename Tree>
std::vector<llvm::BasicBlock *>
properlyDominatedBlocks(const Tree *tree, llvm::BasicBlock *block) {
  std::vector<llvm::BasicBlock *> result;
  auto *node = tree->getNode(block);
  if (!node)
    return result;
  std::vector<llvm::DomTreeNode *> work;
  for (auto *child : *node)
    work.push_back(child);
  while (!work.empty()) {
    auto *current = work.back();
    work.pop_back();
    if (current->getBlock())
      result.push_back(current->getBlock());
    for (auto *child : *current)
      work.push_back(child);
  }
  return result;
}

template <typename T> std::string smtText(const T &value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

} // namespace lotus::gsaf
