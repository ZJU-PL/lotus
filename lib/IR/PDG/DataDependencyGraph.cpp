/**
 * @file DataDependencyGraph.cpp
 * @brief Implementation of the data dependency analysis for the PDG
 *
 * This file implements the DataDependencyGraph pass, which analyzes data dependencies
 * between program elements. Data dependencies occur when one instruction defines a value
 * that is used by another instruction (def-use chains).
 *
 * Key features:
 * - Analysis of def-use chains in LLVM IR
 * - Support for different types of data dependencies (direct, memory, etc.)
 * - Function-level data dependency analysis
 * - Integration with the overall PDG framework
 * - Support for memory-based dependencies through load/store analysis
 *
 * The data dependency analysis is a fundamental component of the PDG system,
 * complementing control dependency analysis to provide a complete view of
 * program dependencies.
 */

#include "IR/PDG/DataDependencyGraph.h"

char pdg::DataDependencyGraph::ID = 0;

using namespace llvm;

bool pdg::DataDependencyGraph::runOnModule(Module &M)
{
  ProgramGraph &g = ProgramGraph::getInstance();
  if (!g.isBuild())
  {
    g.build(M);
    // TODO: add comment
    g.bindDITypeToNodes(M);
  }
  
  // Initialize two alias analysis wrappers:
  // 1. Over-approximation using Andersen's analysis (precise, flow-insensitive)
  // 2. Under-approximation using syntactic pattern matching (fast, conservative)
  _alias_wrapper_over = PDGAliasFactory::create(M, AAType::Andersen);
  _alias_wrapper_under = PDGAliasFactory::create(M, AAType::UnderApprox);
  
  for (auto &F : M)
  {
    if (F.isDeclaration() || F.empty())
      continue;
    _mem_dep_res = &getAnalysis<MemoryDependenceWrapperPass>(F).getMemDep();
    // setup alias query interface for each function
    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F); inst_iter++)
    {
      addDefUseEdges(*inst_iter);
      addRAWEdges(*inst_iter);
      addAliasEdges(*inst_iter);
    }
  }
  return false;
}

void pdg::DataDependencyGraph::addAliasEdges(Instruction &inst)
{
  ProgramGraph &g = ProgramGraph::getInstance();
  Function* func = inst.getFunction();
  for (auto inst_iter = inst_begin(func); inst_iter != inst_end(func); inst_iter++)
  {
    if (&inst == &*inst_iter)
      continue;
    auto alias_result = queryAliasUnderApproximate(inst, *inst_iter);
    if (alias_result != llvm::AliasResult::NoAlias)
    {
      Node* src = g.getNode(inst);
      Node* dst = g.getNode(*inst_iter);
      if (src == nullptr || dst == nullptr)
        continue;
      src->addNeighbor(*dst, EdgeType::DATA_ALIAS);
    }
  }
}

void pdg::DataDependencyGraph::addDefUseEdges(Instruction &inst)
{
  ProgramGraph &g = ProgramGraph::getInstance();
  for (auto user : inst.users())
  {
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*user);
    if (src == nullptr || dst == nullptr)
      continue;
    EdgeType edge_type = EdgeType::DATA_DEF_USE;
    if (dst->getNodeType() == GraphNodeType::ANNO_VAR)
      edge_type = EdgeType::ANNO_VAR;
    if (dst->getNodeType() == GraphNodeType::ANNO_GLOBAL)
      edge_type = EdgeType::ANNO_GLOBAL;
    src->addNeighbor(*dst, edge_type);
  }
}

void pdg::DataDependencyGraph::addRAWEdges(Instruction &inst)
{
  if (!isa<LoadInst>(&inst))
    return;

  ProgramGraph &g = ProgramGraph::getInstance();
  auto dep_res = _mem_dep_res->getDependency(&inst);
  auto dep_inst = dep_res.getInst();

  if (!dep_inst)
    return;
  if (!isa<StoreInst>(dep_inst))
    return;

  Node *src = g.getNode(inst);
  Node *dst = g.getNode(*dep_inst);
  if (src == nullptr || dst == nullptr)
    return;
  dst->addNeighbor(*src, EdgeType::DATA_RAW);
}

llvm::AliasResult pdg::DataDependencyGraph::queryAliasUnderApproximate(llvm::Value &v1, llvm::Value &v2)
{
  // Use the under-approximation wrapper (syntactic pattern matching)
  // This only returns MustAlias for clear syntactic patterns, otherwise NoAlias
  if (_alias_wrapper_under && _alias_wrapper_under->isInitialized())
  {
    return _alias_wrapper_under->query(&v1, &v2);
  }
  
  // Fall back to simple check if wrapper is not available
  if (!v1.getType()->isPointerTy() || !v2.getType()->isPointerTy())
    return llvm::AliasResult::NoAlias;
  
  return llvm::AliasResult::NoAlias;
}

llvm::AliasResult pdg::DataDependencyGraph::queryAliasOverApproximate(llvm::Value &v1, llvm::Value &v2)
{
  // Use the over-approximation wrapper (Andersen's analysis)
  // This integrates precise pointer analysis from lib/Alias/Andersen
  if (_alias_wrapper_over && _alias_wrapper_over->isInitialized())
  {
    return _alias_wrapper_over->query(&v1, &v2);
  }
  
  // Fall back to conservative answer if wrapper is not available
  return llvm::AliasResult::MayAlias;
}

  void pdg::DataDependencyGraph::getAnalysisUsage(AnalysisUsage & AU) const
  {
    AU.addRequired<MemoryDependenceWrapperPass>();
    AU.setPreservesAll();
  }

  static RegisterPass<pdg::DataDependencyGraph>
      DDG("ddg", "Data Dependency Graph Construction", false, true);