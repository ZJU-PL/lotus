#pragma once

#include "Checker/TPA/Support/TaintStore.h"

#include <llvm/ADT/ArrayRef.h>

namespace tpa::taint {

class EvalResult;
class ProgramPoint;
class TaintMemo;
class WorkList;

class TaintPropagator {
private:
  TaintMemo &memo;
  WorkList &workList;

  void propagateObjFrom(const ProgramPoint &, const tpa::MemoryObject *,
                        TaintLattice);

public:
  TaintPropagator(TaintMemo &m, WorkList &w) : memo(m), workList(w) {}

  void propagate(const EvalResult &,
                 llvm::ArrayRef<const tpa::MemoryObject *> dirtyObjs = {});
};

} // namespace tpa::taint