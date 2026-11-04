#pragma once

#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"
#include "Utils/ADT/PushPopCache.h"

#include <mutex>

namespace lotus::gsaf {
using namespace llvm;
using namespace lotus::gvfg;

/*
 * Specific trace representation for vulnerabilities
 *
 * The trace is a vector of GuardedValueFlowObjects (GuardedValueFlowSite or
 * GuardedValueFlowNode) that stands for the value flow.
 *
 * e.g.
 * 		Source Value -> Source Site -> Source Value -> Val_11 -> Val_12
 * ->
 * ...
 * 		-> Val_1n (Actual Argument) -> Call Site -> Val_21 (Formal
 * Argument)
 * 		-> Val_22 -> ... -> Val_2n (Return Value) -> Return Site ->
 * 		Val_31 (Call Site Output) -> Val_32 -> ... -> Val_3n -> Sink
 * Site.
 *
 * 	In the above example, each Val_ij is modeled as an GuardedValueFlowNode,
 * 	and each Site is modeled as an GuardedValueFlowSite. Source Site
 * 	can be nullptr.
 *
 */
class VulnerabilityTrace : public GuardedValueFlowTrace {
private:
  /// A trace has been reported as a vulnerability
  bool Reported = false;

  /// It is used when trace-level synchronization is necessary
  std::mutex TraceLock;

public:
  VulnerabilityTrace();

  VulnerabilityTrace(const VulnerabilityTrace &T);

  /// Create a trace that only contains \p Obj
  VulnerabilityTrace(const GuardedValueFlowObject *Obj);

  VulnerabilityTrace(const GuardedValueFlowObject *Obj1,
                     const GuardedValueFlowObject *Obj2);

  VulnerabilityTrace(const std::vector<const GuardedValueFlowObject *> &Trace);

  virtual ~VulnerabilityTrace();

  VulnerabilityTrace &operator=(const VulnerabilityTrace &T);

  bool reported() const { return Reported; }

  void setReported(bool B) { Reported = B; }

  void lock() { TraceLock.lock(); }

  void unlock() { TraceLock.unlock(); }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                                       const VulnerabilityTrace &T);
};

using namespace llvm;
using namespace lotus::gvfg;

class VulnerabilityTraceBuilder
    : public PushPopVector<const GuardedValueFlowObject *> {
public:
  VulnerabilityTraceBuilder();
  VulnerabilityTraceBuilder(const VulnerabilityTraceBuilder &);
  ~VulnerabilityTraceBuilder();

  void add(std::shared_ptr<VulnerabilityTrace>);
  virtual void add(const GuardedValueFlowObject *O);

  /// This function makes a copy of the current trace
  /// and returns the copy as a VulnerabilityTrace.
  std::shared_ptr<VulnerabilityTrace> snapshot() const;

  const GuardedValueFlowObject *operator[](size_t Index) const {
    assert(Index < this->size());
    return this->getCacheVector()[Index];
  }

  const GuardedValueFlowNode *sourceNode() const {
    if (!this->empty()) {
      auto *Obj = this->operator[](0);
      return Obj ? dyn_cast<GuardedValueFlowNode>(Obj) : nullptr;
    } else {
      return nullptr;
    }
  }

  const GuardedValueFlowSite *sourceSite() const {
    if (this->size() >= 2) {
      auto *Obj = this->operator[](1);
      return Obj ? dyn_cast<GuardedValueFlowSite>(Obj) : nullptr;
    } else {
      return nullptr;
    }
  }

  const GuardedValueFlowObject *recentObj() const {
    if (size_t Sz = this->size()) {
      return this->operator[](Sz - 1);
    } else {
      return nullptr;
    }
  }

  template <typename DstTy> const DstTy *recentObjAs() const {
    auto *Ret = recentObj();
    return Ret ? dyn_cast<DstTy>(Ret) : nullptr;
  }

  /// Searching the trace from the \p StartIndex until
  /// the predicate \p P is satisfied. Return the found
  /// GuardedValueFlowObject and its index.
  template <typename Predicate>
  std::pair<const GuardedValueFlowObject *, size_t> find(size_t StartIndex,
                                                         Predicate P) const {
    size_t Size = size();
    for (size_t I = StartIndex; I < Size; ++I) {
      auto *O = this->operator[](I);
      if (P(O)) {
        return std::make_pair(O, I);
      }
    }

    return std::make_pair(nullptr, Size);
  }

  /// Same as VulnerabilityTraceBuilder::find, except that it
  /// searches the trace in a reversed order.
  template <typename Predicate>
  std::pair<const GuardedValueFlowObject *, size_t> rfind(size_t StartIndex,
                                                          Predicate P) const {
    size_t Size = size();
    if (StartIndex >= Size)
      return std::make_pair(nullptr, Size);

    for (size_t I = StartIndex;; --I) {
      auto *O = this->operator[](I);
      if (P(O)) {
        return std::make_pair(O, I);
      }

      if (I == 0)
        break;
    }

    return std::make_pair(nullptr, Size);
  }
};

} // namespace lotus::gsaf
