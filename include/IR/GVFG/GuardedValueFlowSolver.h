/// @file GuardedValueFlowSolver.h
/// @brief SMT-based constraint solver over the GuardedValueFlowGraph
///
/// Translates value-flow dependencies and region-based path conditions into
/// SMT formulas.  Supports:
///   - control-dependence queries (per-node, per-block, paired)
///   - data-dependence queries (recursive encoding of operand/opcode chains)
///   - phi gating (incoming-edge-local guards with AND/OR semantics)
///   - combined dependency queries (Ctrl+Data for a single assignment edge)
///   - opcode encoding (binary, compare, cast, GEP, select, concat,
///     extract/insert element)
///
/// `DTGuardedValueFlowSolver` is a dominator-aware variant that suppresses
/// control-dependence constraints already implied by the previously visited
/// basic block.

#pragma once

#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"
#include "Solvers/SMT/LIBSMT/SMTSolver.h"
#include "Utils/ADT/PushPopCache.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>

namespace lotus {
namespace gvfg {

using llvm::BasicBlock;
using llvm::DataLayout;
using llvm::DominatorTree;

// GuardedValueFlowSolver turns value-flow and path-condition structure into SMT
// constraints. It mirrors the old query style:
// - control dependencies for a node or block
// - data dependencies for a node
// - gated predicates for PHI incoming values
// - combined dependencies for one assignment edge
/// Core solver that encodes value-flow and path-condition structure into
/// SMT constraints using an `SMTFactory`.
class GuardedValueFlowSolver : public SMTSolver {
public:
  struct QueryContext {
    BasicBlock *previous_block{nullptr};
  };

protected:
  std::unordered_map<const GuardedValueFlowNode *, SMTExpr> NodeExprMap;
  std::unordered_map<std::string, const GuardedValueFlowNode *>
      NodeSymbolNameMap;

  const DataLayout &DL;

public:
  explicit GuardedValueFlowSolver(SMTFactory &factory, const DataLayout &dl);
  virtual ~GuardedValueFlowSolver();

  SMTExprVec getCtrlDeps(const GuardedValueFlowNode *node,
                         const QueryContext *context = nullptr);
  SMTExprVec getCtrlDeps(BasicBlock *block, const GuardedValueFlowGraph *graph,
                         const QueryContext *context = nullptr);
  std::pair<SMTExprVec, SMTExprVec>
  getCtrlDepsPair(BasicBlock *block, const GuardedValueFlowGraph *graph,
                  const QueryContext *context = nullptr);

  SMTExprVec getDataDeps(const GuardedValueFlowNode *node,
                         const QueryContext *context = nullptr);

  SMTExprVec getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                         GuardedValueFlowPhiNode::Incoming incoming,
                         const QueryContext *context = nullptr);
  SMTExprVec getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                         const GuardedValueFlowNode *value, BasicBlock *block,
                         const QueryContext *context = nullptr);
  SMTExprVec getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                         const GuardedValueFlowNode *value,
                         const QueryContext *context = nullptr);

  virtual SMTExprVec getDeps(const GuardedValueFlowNode *node,
                             const GuardedValueFlowNode *child);
  virtual std::pair<SMTExprVec, SMTExprVec>
  getDepsPair(const GuardedValueFlowNode *node,
              const GuardedValueFlowNode *child);

  virtual void push();
  virtual void pop(unsigned n = 1);
  virtual void reset();
  virtual SMTResultType check();

  const std::unordered_set<const GuardedValueFlowNode *> &
  getUsedFunctionArguments() {
    return FunctionArgumentCache.getCacheSet();
  }

  std::pair<std::vector<const GuardedValueFlowCallOutputNode *>::iterator,
            std::vector<const GuardedValueFlowCallOutputNode *>::iterator>
  getUsedCallSiteOutputs(bool restart = false) {
    return CallSiteOutputCache.getCacheVector(restart);
  }

  const GuardedValueFlowNode *getNodeFromSymbol(const std::string &symbol) {
    auto it = NodeSymbolNameMap.find(symbol);
    return it == NodeSymbolNameMap.end() ? nullptr : it->second;
  }

  SMTExpr getOrInsertExpr(const GuardedValueFlowNode *node);

  SMTExpr encodeOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr
  encodeBinaryOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr
  encodeCompareOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr encodeCastOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr encodeGEPOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr
  encodeExtractElementOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr
  encodeInsertElementOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr
  encodeSelectOpcodeNode(const GuardedValueFlowOpcodeNode *node);
  virtual SMTExpr
  encodeConcatOpcodeNode(const GuardedValueFlowOpcodeNode *node);

protected:
  PushPopCache<const GuardedValueFlowNode *> ConstraintCache;
  PushPopCache<BasicBlock *> BBCache;

  /// Encoding policy hooks for clients with a different scalar abstraction.
  /// Default implementations preserve GVFG's LLVM layout and bit patterns.
  virtual uint64_t getEncodingTypeSize(Type *type) const;
  virtual std::string getEncodingSymbol(const GuardedValueFlowNode *node) const;
  virtual SMTExpr encodeScalarConstant(const llvm::Constant *constant,
                                       uint64_t width);
  virtual std::pair<uint64_t, uint64_t>
  getEncodingCastWidths(const GuardedValueFlowOpcodeNode *node) const;
  virtual bool trackCallOutput(const GuardedValueFlowCallOutputNode *node) const;
  virtual bool isNonNullTerminal(const GuardedValueFlowNode *node) const;

  virtual std::pair<SMTExprVec, SMTExprVec>
  computeCtrlDepsPair(BasicBlock *block, const GuardedValueFlowGraph *graph,
                      const QueryContext *context);
  virtual std::pair<SMTExprVec, SMTExprVec>
  computePhiGatedPair(const GuardedValueFlowPhiNode *phi_node,
                      GuardedValueFlowPhiNode::Incoming incoming,
                      const QueryContext *context);
  virtual SMTExprVec computeDataDeps(const GuardedValueFlowNode *node,
                                     const QueryContext *context);

private:
  PushPopCache<const GuardedValueFlowNode *> FunctionArgumentCache;
  PushPopCache<const GuardedValueFlowCallOutputNode *> CallSiteOutputCache;

  std::unordered_map<BasicBlock *, SMTExprVec> CtrlCacheMap;
  std::unordered_set<BasicBlock *> ActiveCtrlBlocks;
  std::unordered_map<const GuardedValueFlowOpcodeNode *, SMTExpr>
      OpcodeConstraintsCacheMap;

  std::pair<SMTExprVec, SMTExprVec>
  _getCtrlDeps(BasicBlock *block, const GuardedValueFlowGraph *graph,
               size_t depth = 0);
  std::pair<SMTExprVec, SMTExprVec>
  _getPhiGated(const GuardedValueFlowPhiNode *phi_node,
               GuardedValueFlowPhiNode::Incoming incoming);
  SMTExprVec _getDataDeps(const GuardedValueFlowNode *node, size_t depth = 0);
};

// Dominator-aware variant that suppresses control constraints already implied
// by the previously visited block.
class DTGuardedValueFlowSolver : public GuardedValueFlowSolver {
private:
  const DominatorTree *DT;

  std::unordered_map<BasicBlock *, std::pair<SMTExprVec, BasicBlock *>>
      CtrlCacheMap;

public:
  DTGuardedValueFlowSolver(SMTFactory &factory, const DataLayout &dl,
                           const DominatorTree *dt)
      : GuardedValueFlowSolver(factory, dl), DT(dt) {}
  virtual SMTExprVec getDeps(const GuardedValueFlowNode *node,
                             const GuardedValueFlowNode *child) override;

  virtual void reset() override;

protected:
  std::pair<SMTExprVec, SMTExprVec>
  computeCtrlDepsPair(BasicBlock *block, const GuardedValueFlowGraph *graph,
                      const QueryContext *context) override;
  std::pair<SMTExprVec, SMTExprVec>
  computePhiGatedPair(const GuardedValueFlowPhiNode *phi_node,
                      GuardedValueFlowPhiNode::Incoming incoming,
                      const QueryContext *context) override;
  SMTExprVec computeDataDeps(const GuardedValueFlowNode *node,
                             const QueryContext *context) override;

private:
  std::pair<SMTExprVec, SMTExprVec>
  _getCtrlDepsWrapper(BasicBlock *block, const GuardedValueFlowGraph *graph,
                      BasicBlock *prev_block);
  std::pair<SMTExprVec, SMTExprVec>
  _getCtrlDeps(BasicBlock *block, const GuardedValueFlowGraph *graph,
               BasicBlock *prev_block);
  std::pair<SMTExprVec, SMTExprVec>
  _getPhiGated(const GuardedValueFlowPhiNode *phi_node,
               GuardedValueFlowPhiNode::Incoming incoming,
               BasicBlock *prev_block);
  SMTExprVec _getDataDeps(const GuardedValueFlowNode *node,
                          BasicBlock *prev_block);
};

} // namespace gvfg
} // namespace lotus
