#pragma once

#include "Alias/InclusionBased/TPA/Util/Hashing.h"
#include "Checker/TPA/Support/ProgramPoint.h"

#include <cstddef>

namespace llvm {
class Function;
} // namespace llvm

namespace tpa::taint {

class SinkSignature {
private:
  ProgramPoint callsite;
  const llvm::Function *callee;

public:
  SinkSignature(const ProgramPoint &p, const llvm::Function *f)
      : callsite(p), callee(f) {}

  const ProgramPoint &getCallSite() const { return callsite; }

  const llvm::Function *getCallee() const { return callee; }

  bool operator==(const SinkSignature &rhs) const {
    return callee == rhs.callee && callsite == rhs.callsite;
  }
  bool operator!=(const SinkSignature &rhs) const { return !(*this == rhs); }
};

} // namespace tpa::taint

namespace std {
template <> struct hash<tpa::taint::SinkSignature> {
  size_t operator()(const tpa::taint::SinkSignature &s) const {
    return util::hashPair(s.getCallSite(), s.getCallee());
  }
};
} // namespace std