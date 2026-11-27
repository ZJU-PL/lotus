/** @file PathCondSolver.h @brief SMT-backed path condition solver for symbolic execution. */
#pragma once

#include "IR/GVFG/GuardedValueFlowSolver.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"
#include "SymbolicExecution/Domain/BigInteger.h"
#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyValue.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SymbolicExecution {

using llvm::BasicBlock;
using llvm::Function;
using lotus::gvfg::GuardedValueFlowCallOutputNode;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowPhiNode;
using lotus::gvfg::GuardedValueFlowRegionNode;
using lotus::gvfg::GuardedValueFlowSolver;

class PropertySymExpr;

/// Solver-owned context for constructing and checking symbolic path predicates.
class PathCondSolver {
public:
  explicit PathCondSolver(const llvm::DataLayout &DL);
  ~PathCondSolver() = default;
  PathCondSolver(const PathCondSolver &) = delete;
  PathCondSolver &operator=(const PathCondSolver &) = delete;

  SMTExpr buildRegionCondition(GuardedValueFlowRegionNode *Cond);
  SMTExprVec createEmptySMTExprVec();
  SMTExpr buildExprForVal(const PropertyValue *V);
  SMTExpr buildBitVecVal(uint64_t V, uint64_t Sz);
  SMTExpr getExpr(Var V);
  SMTExpr renameExpr(const SMTExpr &Constr, const std::string &Suffix);
  SMTExpr buildBoolVal(bool V);
  SMTExpr buildBoolUnknown(const std::string &Name);
  SMTExpr buildPredicate(const PropertyValue *Va1, const PropertyValue *Va2,
                         unsigned Pred);
  SMTExpr buildEqualCond(Var V, const PropertyValue *Va,
                         const std::string &VSuffix = "");
  SMTExpr getPhiGated(const GuardedValueFlowPhiNode *PhiNode,
                      const GuardedValueFlowPhiNode::Incoming InNode);

  SMTExpr getCtrlDeps(BasicBlock *B, const GuardedValueFlowGraph *G);
  SMTExpr getDataDeps(const GuardedValueFlowNode *N);

  std::unordered_set<const GuardedValueFlowCallOutputNode *>
  getUsedCallSiteOutput() const {
    return UsedCSOuts;
  }

  bool isConstraintSat(const SMTExprVec &Cons);

  std::vector<bool>
  batchCheckPredicates(const SMTExprVec &Context,
                       const std::vector<SMTExprVec> &Predicates);

  void batchCheckPredicatesWithAnalysis(
      const SMTExprVec &Context, const std::vector<SMTExprVec> &Predicates,
      std::vector<bool> &Results, unsigned &PrunedCount);

  SMTExpr translate(const SMTExpr &Cons) { return Fctry.translate(Cons); }
  std::mutex &getSolverLock() { return Mtx; }
  unsigned getExprSize(const SMTExpr &E, unsigned LimitVal);
  uint32_t getID() const { return SolverID; }

private:
  static std::atomic<uint32_t> ID;

  const llvm::DataLayout &DL;
  uint32_t SolverID;
  std::mutex Mtx;
  SMTFactory Fctry;
  std::unordered_set<const GuardedValueFlowCallOutputNode *> UsedCSOuts;
  std::unique_ptr<GuardedValueFlowSolver> GraphSolver;
  std::unique_ptr<SMTSolver> QuerySolver;

  std::unordered_map<unsigned, unsigned> ConstrToSize;
  std::unordered_map<unsigned, std::vector<SMTExpr>> ExprFreeConstsCache;

  GuardedValueFlowSolver &getGraphSolver();
  SMTSolver &getQuerySolver();
  const std::vector<SMTExpr> &getFreeConsts(const SMTExpr &E);
  void resetSolverState();
  void updateUsedCSOuts();
  void normalizeExprWidth(SMTExpr &L, SMTExpr &R, bool SignExtend = true);
  void normalizeExprWidth(SMTExpr &E1, SMTExpr &E2, SMTExpr &E3,
                          bool SignExtend = true);
  SMTExpr buildSymExpr(const PropertySymExpr *V);
  SMTExpr buildIntExpr(const BigInteger &I);
  SMTExpr makeConstMul(SMTExpr Val, uint64_t C);
};

} // namespace SymbolicExecution
