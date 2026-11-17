#pragma once

#include "Checker/TPA/Annotation/TaintDescriptor.h"
#include "Checker/TPA/Support/SinkViolationRecord.h"
#include "Checker/TPA/Support/TaintStore.h"

namespace annotation {
class ExternalTaintTable;
class SinkTaintEntry;
class TaintSummary;
} // namespace annotation

namespace tpa {
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {

class SinkSignature;
class TaintEnv;
class TaintMemo;
class TaintValue;

class SinkViolationChecker {
private:
  const TaintEnv &env;
  const TaintMemo &memo;
  const annotation::ExternalTaintTable &table;
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;

  TaintLattice lookupTaint(const TaintValue &, annotation::TClass,
                           const TaintStore *);
  void checkValueWithTClass(const TaintValue &, annotation::TClass, unsigned,
                            const TaintStore *, SinkViolationList &);
  void checkCallSiteWithEntry(const ProgramPoint &,
                              const annotation::SinkTaintEntry &,
                              SinkViolationList &);
  SinkViolationList checkCallSiteWithSummary(const ProgramPoint &,
                                             const annotation::TaintSummary &);

  void checkSinkViolation(const SinkSignature &, SinkViolationRecord &);

public:
  SinkViolationChecker(const TaintEnv &e, const TaintMemo &m,
                       const annotation::ExternalTaintTable &t,
                       const tpa::SemiSparsePointerAnalysis &p)
      : env(e), memo(m), table(t), ptrAnalysis(p) {}

  // Return all prog location whose sink policy is violated
  template <typename SigContainer>
  SinkViolationRecord checkSinkViolation(const SigContainer &sinks) {
    SinkViolationRecord ret;
    for (auto const &sig : sinks)
      checkSinkViolation(sig, ret);
    return ret;
  }
};

} // namespace tpa::taint
