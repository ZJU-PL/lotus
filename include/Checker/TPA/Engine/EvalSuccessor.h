#pragma once

#include "Checker/TPA/Support/ProgramPoint.h"

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

class EvalSuccessor {
private:
  ProgramPoint pp;
  const tpa::MemoryObject *obj;

  EvalSuccessor(const ProgramPoint &p, const tpa::MemoryObject *o)
      : pp(p), obj(o) {}

public:
  bool isTopLevel() const { return obj == nullptr; }
  const ProgramPoint &getProgramPoint() const { return pp; }

  const tpa::MemoryObject *getMemoryObject() const {
    assert(obj != nullptr);
    return obj;
  }

  friend class EvalResult;
};

} // namespace tpa::taint
