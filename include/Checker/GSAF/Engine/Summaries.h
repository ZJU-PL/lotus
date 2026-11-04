#pragma once

#include "Checker/GSAF/API/Trace.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GVFG/GuardedValueFlowNodes.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"
#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Support/Casting.h>

namespace lotus::gsaf {

class SummaryCacheItem {
public:
  SMTExpr *constraints;
  std::string suffix;
  int depth;

public:
  SummaryCacheItem(SMTExpr *constraints, std::string suffix, int depth);

  std::pair<SMTExpr, bool>
  getSMTExprFromCache(SMTFactory *Fctry,
                      SMTRenamingAdvisor *Advisor = nullptr) const;
};

class SummaryBase {

public:
  static void SMTReadLock();
  static void SMTWriteLock();
  // Split SMTUnlock() to SMTReadUnlock() and SMTWriteUnlock().
  // Compatible with windows.
  static void SMTReadUnlock();
  static void SMTWriteUnlock();

public:
  enum SummaryType {
    ST_TraceSummaryBegin,
    ST_InputSummary = ST_TraceSummaryBegin,
    ST_OutputSummary,
    ST_TransferSummary,
    ST_SourceSummary,
    ST_TraceSummaryEnd = ST_SourceSummary,

    ST_SymbolicSummary,
  };

private:
  SummaryType Type;

  std::unordered_set<const gvfg::GuardedValueFlowNode *> Inputs;

  // Cache for Ctrl/Data dependence, and dependence caused by inlining symbolic
  // summary for the current summary. Shall be inlined only for post
  // verification when the full bug trace is constructed
  //
  // NonSymSummay/SymbSummay are caches for better caching non-symbolic/symbolic
  // deps
  std::vector<SummaryCacheItem> NonSymDepsCache;
  std::vector<SummaryCacheItem> SymbDepsCache;
  std::unordered_map<std::string, SMTExpr *> NonSymSummary;
  std::unordered_map<std::string, SMTExpr *> SymbSummary;

  llvm::Function *F;

  unsigned InlineDepth;

  /// This mask is to show what vulnerabilities the summary belongs to
  /// If VulnerabilityManager::VulnerabilityTy & VulnerabilityMask != 0,
  /// the summary belongs to the vulnerability.
  int VulnerabilityMask;

protected:
  SummaryBase(llvm::Function *Func, SummaryType Ty, unsigned Depth = 0);

public:
  virtual ~SummaryBase();
  SummaryBase(const SummaryBase &) = delete;
  SummaryBase &operator=(const SummaryBase &) = delete;

  SummaryType getType() const;

  void
  setInputs(const std::unordered_set<const gvfg::GuardedValueFlowNode *> &Ins);

  const std::unordered_set<const gvfg::GuardedValueFlowNode *> &getInputs();

  // Add the control/data/symbolic dependence from the cache (not inlined) in
  // the bug detection procedure
  void addNonSymDeps(SummaryCacheItem Item);
  void addSymbDeps(SummaryCacheItem Item);

  // Get the control/data/symbolic cache vector
  const std::vector<SummaryCacheItem> &getNonSymDepsCache();
  const std::vector<SummaryCacheItem> &getSymbDepsCache();

  llvm::Function *getFunction();

  unsigned getInlineDepth();

  void setVulnerabilityMask(int M);

  int getVulnerabilityMask();

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &,
                                       const SummaryBase &);
};

using gvfg::GuardedValueFlowNode;
using gvfg::GuardedValueFlowObject;
using gvfg::GuardedValueFlowReturnNode;
using gvfg::GuardedValueFlowReturnSite;
using gvfg::GuardedValueFlowSite;
using gvfg::GuardedValueFlowTrace;
using llvm::dyn_cast;
using llvm::isa;
using llvm::raw_ostream;

/// A trace summary describes relevant information
/// of a trace, including the trace itself, the
/// constraints, etc.
class TraceSummary : public SummaryBase {
protected:
  std::shared_ptr<VulnerabilityTrace> Trace;

public:
  TraceSummary(llvm::Function *Func, SummaryBase::SummaryType Type,
               std::shared_ptr<VulnerabilityTrace> T, unsigned Depth = 0)
      : SummaryBase(Func, Type, Depth), Trace(T) {
    assert(Trace->get_length() > 0 && "Empty trace!");
  }

  virtual ~TraceSummary() = 0;

  const std::shared_ptr<VulnerabilityTrace> &getTrace() { return Trace; }

  const std::shared_ptr<VulnerabilityTrace> &getTrace() const { return Trace; }

  const GuardedValueFlowNode *getSourceNode() {
    assert(Trace->get_length() > 1 &&
           "A Trace at least contains a source node and a source site!");
    auto *HeadObj = Trace->head();
    assert(isa<GuardedValueFlowNode>(HeadObj));
    return (const GuardedValueFlowNode *)HeadObj;
  }

  const GuardedValueFlowSite *getSourceSite() {
    assert(Trace->get_length() > 1 &&
           "A Trace at least contains a source node and a source site!");
    auto *HeadSite = Trace->at(1);
    if (HeadSite) {
      assert(isa<GuardedValueFlowSite>(HeadSite));
      return (const GuardedValueFlowSite *)HeadSite;
    } else {
      return nullptr;
    }
  }

  const GuardedValueFlowNode *getTailNode() {
    assert(Trace->get_length() > 1 &&
           "A trace at least contains a value (node) and one of its use site.");

    size_t trace_size = Trace->get_length();
    auto *TailNode = dyn_cast<GuardedValueFlowNode>(Trace->at(trace_size - 2));
    assert(TailNode);

    return TailNode;
  }

  const GuardedValueFlowSite *getTailSite() {
    assert(Trace->get_length() > 1 &&
           "A trace at least contains a value (node) and one of its use site.");
    auto *TailSite = dyn_cast<GuardedValueFlowSite>(Trace->tail());
    assert(TailSite);
    return TailSite;
  }

public:
  static bool classof(const SummaryBase *N) {
    return N->getType() >= ST_TraceSummaryBegin &&
           N->getType() <= ST_TraceSummaryEnd;
  }
};

struct trace_summary_cmp {
  bool operator()(const TraceSummary *Left, const TraceSummary *Right) const {
    const auto &LeftTrace = Left->getTrace();
    const auto &RightTrace = Right->getTrace();

    if (LeftTrace->get_length() < RightTrace->get_length()) {
      return true;
    } else if (LeftTrace->get_length() > RightTrace->get_length()) {
      return false;
    }

    struct ObjectLess comparator;
    for (int Idx = 0; Idx < LeftTrace->get_length(); ++Idx) {
      const auto *O1 = LeftTrace->at(Idx);
      const auto *O2 = RightTrace->at(Idx);

      if (!O1 || !O2) {
        if (O1 == O2) { // both nullptr
          continue;
        }
        return O1 < O2; // only one of them is nullptr
      }

      if (comparator(O1, O2)) {
        return true;
      } else if (comparator(O2, O1)) {
        return false;
      }
    }

    return false;
  }
};

/// Input summary is a trace summary whose
/// trace ends at a sink point of a vulnerability
class InputSummary : public TraceSummary {
public:
  InputSummary(llvm::Function *Func, std::shared_ptr<VulnerabilityTrace> Trace,
               unsigned Depth = 0)
      : TraceSummary(Func, ST_InputSummary, Trace, Depth) {}
  virtual ~InputSummary() {}

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                                       const InputSummary &Smry) {
    Out << (const SummaryBase &)Smry;
    return Out;
  }

public:
  static bool classof(const SummaryBase *N) {
    return N->getType() == ST_InputSummary;
  }
};

/// Output summary is a trace summary whose
/// trace ends at a return point
class OutputSummary : public TraceSummary {
public:
  OutputSummary(llvm::Function *Func, std::shared_ptr<VulnerabilityTrace> Trace,
                unsigned Depth = 0)
      : TraceSummary(Func, ST_OutputSummary, Trace, Depth) {}
  virtual ~OutputSummary() {}

  const GuardedValueFlowReturnNode *getReturnNode() {
    assert(Trace->get_length() > 1 &&
           "A trace at least contains a value (node) and one of its use site.");

    size_t trace_size = Trace->get_length();
    auto *TailNode = dyn_cast<GuardedValueFlowNode>(Trace->at(trace_size - 2));
    assert(TailNode);

    assert(isa<GuardedValueFlowReturnNode>(TailNode));
    return (const GuardedValueFlowReturnNode *)TailNode;
  }

  const GuardedValueFlowReturnSite *getReturnSite() {
    assert(Trace->get_length() > 1 &&
           "A trace at least contains a value (node) and one of its use site.");
    auto *TailSite = dyn_cast<GuardedValueFlowSite>(Trace->tail());
    assert(TailSite);
    assert(isa<GuardedValueFlowReturnSite>(TailSite));
    return (const GuardedValueFlowReturnSite *)TailSite;
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                                       const OutputSummary &Smry) {
    Out << (const SummaryBase &)Smry;
    return Out;
  }

public:
  static bool classof(const SummaryBase *N) {
    return N->getType() == ST_OutputSummary;
  }
};

/// A transfer summary records a trace starting from a function parameter
/// to a return site.
class TransferSummary : public TraceSummary {
public:
  /// Deprecated constructor. Suggest not to use it.
  TransferSummary(llvm::Function *Func,
                  std::shared_ptr<VulnerabilityTrace> Trace, unsigned Depth = 0)
      : TraceSummary(Func, ST_TransferSummary, Trace, Depth) {}

  virtual ~TransferSummary() {}

  const GuardedValueFlowReturnNode *getReturnNode() {
    assert(Trace->get_length() > 1 &&
           "A trace at least contains a value (node) and one of its use site.");

    size_t trace_size = Trace->get_length();
    auto *TailNode = dyn_cast<GuardedValueFlowNode>(Trace->at(trace_size - 2));
    assert(TailNode);

    assert(isa<GuardedValueFlowReturnNode>(TailNode));
    return (const GuardedValueFlowReturnNode *)TailNode;
  }

  const GuardedValueFlowReturnSite *getReturnSite() {
    assert(Trace->get_length() > 1 &&
           "A trace at least contains a value (node) and one of its use site.");
    auto *TailSite = dyn_cast<GuardedValueFlowSite>(Trace->tail());
    assert(TailSite);
    assert(isa<GuardedValueFlowReturnSite>(TailSite));
    return (const GuardedValueFlowReturnSite *)TailSite;
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                                       const TransferSummary &Smry) {
    Out << (const SummaryBase &)Smry;
    return Out;
  }

public:
  static bool classof(const SummaryBase *N) {
    return N->getType() == ST_TransferSummary;
  }
};

/// Source summary is a trace summary whose
/// trace starts from a parameter and ends with a source
class SourceSummary : public TraceSummary {
public:
  /// Deprecated constructor. Suggest not to use it.
  SourceSummary(llvm::Function *Func, std::shared_ptr<VulnerabilityTrace> Trace,
                unsigned Depth = 0)
      : TraceSummary(Func, ST_SourceSummary, Trace, Depth) {}

  virtual ~SourceSummary() {}

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                                       const SourceSummary &Smry) {
    Out << (const SummaryBase &)Smry;
    return Out;
  }

public:
  static bool classof(const SummaryBase *N) {
    return N->getType() == ST_SourceSummary;
  }
};

class SymbolicSummary : public SummaryBase {
private:
  const gvfg::GuardedValueFlowNode *Node = nullptr;

public:
  SymbolicSummary(llvm::Function *Func, unsigned Depth = 0)
      : SummaryBase(Func, ST_SymbolicSummary, Depth) {}

  SymbolicSummary(llvm::Function *Func, const gvfg::GuardedValueFlowNode *N,
                  unsigned Depth = 0)
      : SummaryBase(Func, ST_SymbolicSummary, Depth), Node(N) {}

  ~SymbolicSummary() {}

  const gvfg::GuardedValueFlowNode *getSummarizedNode() { return Node; }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                                       const SymbolicSummary &Smry) {
    Out << (const SummaryBase &)Smry;
    return Out;
  }

public:
  static bool classof(const SummaryBase *N) {
    return N->getType() == ST_SymbolicSummary;
  }
};

} // namespace lotus::gsaf
