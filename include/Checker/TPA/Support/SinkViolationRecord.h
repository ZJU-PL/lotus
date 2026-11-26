#pragma once

#include "Checker/TPA/Annotation/TaintDescriptor.h"
#include "Checker/TPA/Lattice/TaintLattice.h"
#include "Checker/TPA/Support/ProgramPoint.h"

#include <unordered_map>
#include <vector>

namespace tpa::taint {

struct SinkViolation {
  unsigned argPos;
  annotation::TClass what;
  TaintLattice expectVal;
  TaintLattice actualVal;
};

using SinkViolationList = std::vector<SinkViolation>;
using SinkViolationRecord = std::unordered_map<ProgramPoint, SinkViolationList>;

} // namespace tpa::taint
