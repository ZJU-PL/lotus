#pragma once

#include "Checker/TPA/Support/TaintMap.h"
#include "Checker/TPA/Support/TaintValue.h"

#include <llvm/IR/Constant.h>

namespace tpa::taint {

class TaintEnv {
private:
  TaintMap<TaintValue> env;

public:
  using const_iterator = typename decltype(env)::const_iterator;

  TaintEnv() = default;

  TaintLattice lookup(const TaintValue &pLoc) const {
    if (llvm::isa<llvm::Constant>(pLoc.getValue()))
      return TaintLattice::Untainted;
    else
      return env.lookup(pLoc);
  }

  bool weakUpdate(const TaintValue &pLoc, TaintLattice l) {
    assert(!llvm::isa<llvm::Constant>(pLoc.getValue()));
    return env.weakUpdate(pLoc, l);
  }

  bool strongUpdate(const TaintValue &pLoc, TaintLattice l) {
    assert(!llvm::isa<llvm::Constant>(pLoc.getValue()));
    return env.strongUpdate(pLoc, l);
  }

  bool mergeWith(const TaintEnv &rhs) { return env.mergeWith(rhs.env); }

  const_iterator begin() const { return env.begin(); }
  const_iterator end() const { return env.end(); }
  size_t size() const { return env.size(); }
};

} // namespace tpa::taint