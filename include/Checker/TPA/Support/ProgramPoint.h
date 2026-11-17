#pragma once

#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cassert>

namespace context {
class Context;
} // namespace context

namespace tpa::taint {

class DefUseInstruction;

class ProgramPoint {
private:
  const context::Context *ctx;
  const DefUseInstruction *node;

public:
  ProgramPoint(const context::Context *c, const DefUseInstruction *n)
      : ctx(c), node(n) {
    assert(ctx != nullptr && node != nullptr);
  }

  const context::Context *getContext() const { return ctx; }
  const DefUseInstruction *getDefUseInstruction() const { return node; }

  bool operator==(const ProgramPoint &rhs) const {
    return ctx == rhs.ctx && node == rhs.node;
  }
  bool operator!=(const ProgramPoint &rhs) const { return !(*this == rhs); }
  bool operator<(const ProgramPoint &rhs) const {
    if (ctx < rhs.ctx)
      return true;
    else if (rhs.ctx < ctx)
      return false;
    else
      return node < rhs.node;
  }
  bool operator>(const ProgramPoint &rhs) const { return rhs < *this; }
  bool operator<=(const ProgramPoint &rhs) const { return !(rhs < *this); }
  bool operator>=(const ProgramPoint &rhs) const { return !(*this < rhs); }
};

} // namespace tpa::taint

namespace std {
template <> struct hash<tpa::taint::ProgramPoint> {
  size_t operator()(const tpa::taint::ProgramPoint &p) const {
    return util::hashPair(p.getContext(), p.getDefUseInstruction());
  }
};
} // namespace std
