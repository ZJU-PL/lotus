#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/ProgramPoint.h"
#include "Utils/ADT/PriorityWorkList.h"
#include "Utils/ADT/TwoLevelWorkList.h"
#include "Utils/LLVM/FIFOWorkList.h"

#include <utility>

namespace tpa::taint {

class TrackerWorkList {
private:
  struct NodeComparator {
    bool operator()(const DefUseInstruction *lhs,
                    const DefUseInstruction *rhs) const {
      return lhs->getPriority() > rhs->getPriority();
    }
  };

  using GlobalWorkListType = util::FIFOWorkList<tpa::FunctionContext>;
  using LocalWorkListType =
      util::PriorityWorkList<const DefUseInstruction *, NodeComparator>;
  using WorkListType =
      util::TwoLevelWorkList<GlobalWorkListType, LocalWorkListType>;
  WorkListType workList;

public:
  using ElemType = ProgramPoint;

  TrackerWorkList() = default;

  void enqueue(const ProgramPoint &p) {
    workList.enqueue(std::make_pair(
        tpa::FunctionContext(p.getContext(),
                             p.getDefUseInstruction()->getFunction()),
        p.getDefUseInstruction()));
  }

  ProgramPoint dequeue() {
    auto pair = workList.dequeue();
    return ProgramPoint(pair.first.getContext(), pair.second);
  }

  ProgramPoint front() {
    auto pair = workList.front();
    return ProgramPoint(pair.first.getContext(), pair.second);
  }

  bool empty() const { return workList.empty(); }
};

} // namespace tpa::taint
