/** @file AnalysisState.h @brief Abstract symbolic execution state for one function. */
#pragma once

#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Domain/SymbolicMemory.h"
#include "SymbolicExecution/Engine/AnalysisExtension.h"
#include "SymbolicExecution/Engine/CStringState.h"
#include "SymbolicExecution/Engine/GuardedValue.h"
#include "SymbolicExecution/Engine/TaintState.h"
#include "SymbolicExecution/Models/TaintModel.h"
#include "SymbolicExecution/Solver/ConstraintRepr.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"
#include "SymbolicExecution/Support/AnalysisContext.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SymbolicExecution {

using lotus::gvfg::GuardedValueFlowCallOutputNode;
using lotus::gvfg::GuardedValueFlowCallSite;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowPhiNode;
using lotus::gvfg::GuardedValueFlowRegionNode;
using lotus::gvfg::GuardedValueFlowReturnNode;

class AnalysisDriver;
class AnalysisSummary;

/// Symbolic state maintained while analyzing one function.
///
/// Tracks symbolic register values, points-to information over symbolic access
/// paths, path conditions, taint propagation, C-string lengths, freed memory
/// facts, and interprocedural call-site renaming state. Bug checking is
/// decoupled via StateExtension hooks.
class AnalysisState {
  friend class AnalysisSummary;
  friend class CStringState;

public:
  AnalysisState(const AnalysisContext &Ctx, const TaintModel &TaintSpec,
                GuardedValueFlowGraph *Graph, llvm::Function *Func,
                StateExtension *Ext = nullptr, bool NeedsSummary = true);

  void transfer(llvm::Instruction *Inst, AnalysisDriver &Driver);
  void finalizeSummary();

  // Read-only state inspection API (used by checkers and extensions)
  const AnalysisContext &getContext() const { return Ctx; }
  llvm::Function *getFunc() const { return F; }
  GuardedValueFlowGraph *getGraph() const { return Graph; }
  bool hasSolver() const { return Solver != nullptr; }
  PathCondSolver *getSolver() const {
    if (!Solver) {
      Solver = std::make_unique<PathCondSolver>(*Ctx.getDataLayout());
    }
    return Solver.get();
  }
  bool needsSummary() const { return NeedsSummary; }

  GuardedValueFlowNode *getNode(llvm::Value *V) const;

  bool hasSymbolicVals(const ProgramValuePtr &V) const {
    return !V.isVacuous() && Regs.count(V) != 0;
  }
  const GuardedSymbolicValSet &getSymbolicVals(const ProgramValuePtr &V) const {
    static const GuardedSymbolicValSet Empty;
    if (V.isVacuous())
      return Empty;
    auto It = Regs.find(V);
    return It != Regs.end() ? It->second : Empty;
  }

  bool hasPts(const ProgramValuePtr &Ptr) const {
    return !Ptr.isVacuous() && PointsTo.count(Ptr) != 0;
  }
  const PtsSet &getPts(const ProgramValuePtr &Ptr) const {
    static const PtsSet Empty;
    if (Ptr.isVacuous())
      return Empty;
    auto It = PointsTo.find(Ptr);
    return It != PointsTo.end() ? It->second : Empty;
  }

  const TaintValSet &getTaintedVals() const { return TaintedVals; }
  const std::unordered_map<ProgramValuePtr, std::vector<TaintStep>> &
  getTaintedSteps() const {
    return TaintedSteps;
  }
  const GuardedProgramValSet &getFreedPtrs() const { return FreedPtrSet; }

  GuardedSymbolicValSet getStrlen(const ProgramValuePtr &Ptr,
                                  llvm::Instruction *Loc) const {
    return StrState.getCStrlen(Ptr, Loc, false, *this);
  }

  Condition getLocalCond(llvm::BasicBlock *BB) const;
  Condition getDataDepsCond(GuardedValueFlowNode *N) const;
  bool mustBeConstantInt(const ProgramValuePtr &V) const;
  bool isInstUnmodelled(llvm::Instruction *Inst) const;

  // Interprocedural call-site mapping & inlining API (used by summary consumers)
  bool hasFormal(llvm::Instruction *CS, const ProgramValuePtr &Formal) const;
  ProgramValuePtr getRealForFormal(llvm::Instruction *CS,
                                   const ProgramValuePtr &Formal) const;
  GuardedSymbolicValSet inlineExpr(const PropertyValuePtr &E,
                                   llvm::Instruction *CS) const;
  GuardedSymbolicValSet inlineVals(const GuardedSymbolicValSet &Vals,
                                   llvm::Instruction *CS,
                                   llvm::Function *Callee,
                                   const Condition &CSCond) const;
  std::pair<PtsSet, bool> inlineVals(const PtsSet &Vals, llvm::Instruction *CS,
                                     llvm::Function *Callee,
                                     const Condition &CSCond) const;
  GuardedProgramValSet inlineVals(const GuardedProgramValSet &Vals,
                                  llvm::Instruction *CS,
                                  llvm::Function *Callee) const;
  Condition transCond(llvm::Instruction *CS, llvm::Function *Callee,
                      const Condition &CalleeCond) const;

private:
  class RenameCtx {
  public:
    explicit RenameCtx(const SMTExpr &FormalExpr) : FormalExpr(FormalExpr) {}

    void add(llvm::Instruction *CS, const SMTExpr &RealExpr) {
      CSToRealExpr.insert(std::make_pair(CS, RealExpr));
    }

    SMTExpr getFormalExpr() const { return FormalExpr; }
    bool hasRealExpr(llvm::Instruction *CS) const {
      return CSToRealExpr.count(CS) != 0;
    }
    SMTExpr getRealExpr(llvm::Instruction *CS) const {
      return CSToRealExpr.at(CS);
    }

  private:
    SMTExpr FormalExpr;
    std::unordered_map<llvm::Instruction *, SMTExpr> CSToRealExpr;
  };

  const AnalysisContext &Ctx;
  const TaintModel &TaintSpec;
  GuardedValueFlowGraph *Graph = nullptr;
  llvm::Function *F = nullptr;
  StateExtension *Ext = nullptr;
  bool NeedsSummary = true;
  mutable std::unique_ptr<PathCondSolver> Solver;
  llvm::ReturnInst *RetI = nullptr;
  mutable size_t FreeVarID = 0;

  // Abstract symbolic state
  std::unordered_map<ProgramValuePtr, PtsSet> PointsTo;
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> Regs;
  std::unordered_set<ProgramValuePtr> UnknownSyms;
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> EscapeAllocToSizes;

  mutable std::unordered_map<llvm::BasicBlock *, Condition> LocalCondMap;
  mutable std::unordered_map<GuardedValueFlowRegionNode *, Condition>
      RegionCondMap;
  mutable std::unordered_map<const GuardedValueFlowCallOutputNode *, Condition>
      CSOutputCondMap;
  mutable std::unordered_map<GuardedValueFlowNode *, Condition> DataDepsCondMap;

  // Summary export and call-site mapping state
  TaintSummary TaintSmry;
  std::unordered_map<ProgramValuePtr, PtsSet> OutputPts;
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> OutSymbolicValMap;
  std::unique_ptr<SummaryExtension> FinalizedExt;

  std::unordered_map<llvm::Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealMap;
  std::unordered_map<llvm::Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealArgMap;
  std::unordered_map<llvm::Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealEscapeMap;
  std::unordered_map<llvm::Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealLenMap;
  std::unordered_map<
      llvm::Instruction *,
      std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet>>
      FormalToRealSymValsMap;
  std::unordered_map<llvm::Function *,
                     std::unordered_map<ProgramValuePtr, RenameCtx>>
      SMTRenameCtxMap;

  // Sub-states
  TaintValSet TaintedVals;
  std::unordered_map<ProgramValuePtr, std::vector<TaintStep>> TaintedSteps;
  CStringState StrState;
  GuardedProgramValSet FreedPtrSet;

  // Inlining caches
  mutable std::unordered_map<
      PropertyValuePtr,
      std::unordered_map<llvm::Instruction *, GuardedSymbolicValSet>>
      InlineExprCache;
  mutable std::unordered_map<llvm::Instruction *,
                             std::unordered_map<int64_t, Condition>>
      InlineCondCache;
  mutable std::unordered_map<llvm::Instruction *, Condition> MappingCondCache;

  // Statement transfer functions
  void processBinaryInst(llvm::Instruction *Inst);
  void processICmpInst(llvm::Instruction *Inst);
  void processAlloca(llvm::Instruction *Inst);
  void processPhiInst(llvm::Instruction *Inst);
  void processGEP(llvm::Instruction *Inst);
  void processLoad(llvm::Instruction *Inst);
  void collectEscapeObjs();
  void processReturn();
  void processLoadPtr(const ProgramValuePtr &Ptr,
                      const GuardedValueFlowNode *LdMemNode,
                      const ProgramValuePtr &Dst, llvm::Instruction *Pos);
  void processStore(llvm::Instruction *Inst);
  void processCall(llvm::CallInst *Inst, llvm::Function *Callee,
                   const AnalysisSummary &Smry);
  void processLibraryCall(llvm::CallInst *Inst);
  void processAsUnknownLib(llvm::CallInst *Inst);
  void processFreeCall(llvm::CallInst *Inst);

  // Taint analysis transfer & summary
  void taintInit(llvm::Function *Func);
  void taintTransfer(llvm::Instruction *Inst);
  void taintProcessCall(llvm::CallInst *Inst, llvm::Function *Callee,
                        const TaintSummary &Smry);
  void buildTaintSummary();
  void processCallTaintSources(llvm::Instruction *Inst);
  void processTaintPropagation(llvm::Instruction *Inst);
  void propagateTaintPointer(const ProgramValuePtr &LdPtr,
                             const ProgramValuePtr &LdVal,
                             llvm::Instruction *Inst, const Condition &Cond);
  void propagateTaint(const ProgramValuePtr &Src, const ProgramValuePtr &Dst,
                      llvm::Instruction *Inst, const Condition &Cond,
                      bool Peel = true);
  Condition getTaintedCond(const ProgramValuePtr &V) const;
  std::unordered_map<ProgramValuePtr, std::unordered_set<ProgramValuePtr>>
  getTaintTransferTargets(llvm::Instruction *Inst) const;
  void taintVal(const ProgramValuePtr &V, const std::vector<TaintStep> &Steps,
                const Condition &PreCond, bool Peel = true);
  void markTaint(const ProgramValuePtr &V, const std::vector<TaintStep> &Steps,
                 const Condition &Cond);

  // Call-site formal/actual mapping helpers
  void initializeFormalToRealMap(llvm::CallInst *Inst,
                                 const AnalysisSummary &Smry);
  void addFormalToRealArgMap(GuardedValueFlowCallSite *GraphCS,
                             llvm::Function *Callee,
                             const AnalysisSummary &Smry);
  void addFormalToRealLenMap(GuardedValueFlowCallSite *GraphCS,
                             llvm::Function *Callee,
                             const AnalysisSummary &Smry);
  void addFormalToRealEscapeMap(GuardedValueFlowCallSite *GraphCS,
                                llvm::Function *Callee,
                                const AnalysisSummary &Smry);
  void addFormalToRealValues(GuardedValueFlowCallSite *GraphCS,
                             llvm::Function *Callee,
                             const ProgramValuePtr &Formal,
                             const ProgramValuePtr &Real);

  // Internal state helpers
  void setPts(const ProgramValuePtr &Dst, const PtsSet &Pts);
  void assignVal(const ProgramValuePtr &Dst, const ProgramValuePtr &Src,
                 const Condition &Cond = Condition());
  void assignVals(const ProgramValuePtr &Dst,
                  const GuardedProgramValSet &SrcVals);
  void assignPtr(const ProgramValuePtr &Dst, const ProgramValuePtr &Src,
                 const GuardedSymbolicValSet &Offset, bool StrongUpdate,
                 const Condition &Cond = Condition());
  std::string getCallsiteSuffix(llvm::Instruction *CS) const;
  Condition getMappingCond(llvm::Instruction *CS, llvm::Function *Callee) const;
  SMTExpr buildVarEqValues(const ProgramValuePtr &V, SMTExpr VExpr,
                           const GuardedSymbolicValSet &SymVals) const;
  ProgramValuePtr getFreeVar(llvm::Type *Ty) const;
  GuardedSymbolicValSet
  evalExpr(const PropertyValuePtr &E,
           const std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> &M,
           llvm::Instruction *CS = nullptr) const;
  void createMemoryObject(const ProgramValuePtr &Ptr, PTItem::MemObjKind Kind,
                          const PropertyValuePtr &Sz = PropertyValuePtr(),
                          const Condition &Cond = Condition());
  PropertyValuePtr computeOffsets(llvm::GEPOperator *GEP);
  void initPointsToTarget(const ProgramValuePtr &Ptr, llvm::Instruction *Pos);
  void processMemAlloc(llvm::CallInst *Inst,
                       const std::vector<unsigned> &AllocSizeArgs);
  void handleMalloc(llvm::CallInst *Inst, unsigned SzIdx);
  void handleCalloc(llvm::CallInst *Inst, unsigned NumIdx, unsigned SzIdx);

  void setSymbolicVals(const ProgramValuePtr &V,
                       const GuardedSymbolicValSet &Vals) {
    Regs[V] = Vals;
  }
  void addSymbolicVals(const ProgramValuePtr &V,
                       const GuardedSymbolicValSet &Vals) {
    Regs[V].addValues(Vals);
  }
  void addUnknownSym(const ProgramValuePtr &V) { UnknownSyms.insert(V); }
  void initSymbol(const ProgramValuePtr &V);
  const GuardedSymbolicValSet &ensureSymbolicVals(const ProgramValuePtr &V);

  Condition getCallSiteOutDeps() const;
  Condition getRegionCond(GuardedValueFlowRegionNode *R) const;
  Condition getPhiCond(const GuardedValueFlowPhiNode *PhiNode,
                       const GuardedValueFlowPhiNode::Incoming InNode) const;

  std::unique_ptr<PathCondSolver> releaseSolver() { return std::move(Solver); }
};

bool isPseudoArgVal(const ProgramValuePtr &V);

} // namespace SymbolicExecution
