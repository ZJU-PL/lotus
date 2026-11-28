#pragma once

#include "Checker/TPA/Engine/EvalSuccessor.h"
#include "Checker/TPA/Support/TaintMap.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace tpa::taint {

class EvalResult {
public:
  struct DirectMemProp {
    ProgramPoint targetPP;
    const tpa::MemoryObject *obj;
  };

  struct MemBroadcast {
    ProgramPoint targetPP;
    bool full;
  };

private:
  // Non-owning view of the incoming pointer-stable TaintStore in TaintMemo.
  const TaintStore *baseStore = nullptr;
  // Sparse per-object writes applied by this transfer (empty for passthrough
  // nodes like Entry, Return, Resume, InternalCall, IgnoredCall).
  TaintMap<const tpa::MemoryObject *> overrides;

  std::vector<ProgramPoint> topSuccs;
  std::vector<DirectMemProp> directMemProps;
  std::vector<MemBroadcast> memBroadcasts;

public:
  EvalResult() = default;

  EvalResult(const EvalResult &) = delete;
  EvalResult(EvalResult &&) noexcept = default;
  EvalResult &operator=(const EvalResult &) = delete;
  EvalResult &operator=(EvalResult &&) = delete;

  void setBaseStore(const TaintStore *s) { baseStore = s; }
  const TaintStore *getBaseStore() const { return baseStore; }
  const TaintMap<const tpa::MemoryObject *> &getOverrides() const {
    return overrides;
  }

  bool hasOverride(const tpa::MemoryObject *obj) const {
    return overrides.findValue(obj) != nullptr;
  }

  TaintLattice lookup(const tpa::MemoryObject *obj) const {
    if (!overrides.empty()) {
      if (const TaintLattice *ov = overrides.findValue(obj))
        return *ov;
    }
    return baseStore ? baseStore->lookup(obj) : TaintLattice::Unknown;
  }

  void strongUpdateOverride(const tpa::MemoryObject *obj, TaintLattice val) {
    overrides.strongUpdate(obj, val);
  }

  void weakUpdateOverride(const tpa::MemoryObject *obj, TaintLattice val) {
    if (const TaintLattice *existing = overrides.findValue(obj)) {
      overrides.strongUpdate(obj, Lattice<TaintLattice>::merge(*existing, val));
    } else {
      const TaintLattice baseVal =
          baseStore ? baseStore->lookup(obj) : TaintLattice::Unknown;
      overrides.strongUpdate(obj, Lattice<TaintLattice>::merge(baseVal, val));
    }
  }

  void addTopLevelSuccessor(const ProgramPoint &pp) { topSuccs.push_back(pp); }

  void addDirectMemProp(const ProgramPoint &pp, const tpa::MemoryObject *obj) {
    directMemProps.push_back(DirectMemProp{pp, obj});
  }

  void addMemBroadcast(const ProgramPoint &pp, bool full) {
    memBroadcasts.push_back(MemBroadcast{pp, full});
  }

  const std::vector<ProgramPoint> &getTopSuccessors() const { return topSuccs; }
  const std::vector<DirectMemProp> &getDirectMemProps() const {
    return directMemProps;
  }
  const std::vector<MemBroadcast> &getMemBroadcasts() const {
    return memBroadcasts;
  }

  bool empty() const {
    return topSuccs.empty() && directMemProps.empty() && memBroadcasts.empty();
  }
};

} // namespace tpa::taint
