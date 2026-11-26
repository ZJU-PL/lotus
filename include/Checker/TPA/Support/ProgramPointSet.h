#pragma once

#include "Checker/TPA/Support/ProgramPoint.h"

#include <cstddef>
#include <unordered_set>

namespace tpa::taint {

class ProgramPointSet {
private:
  using SetType = std::unordered_set<ProgramPoint>;
  SetType ppSet;

public:
  using const_iterator = SetType::const_iterator;

  ProgramPointSet() = default;

  ProgramPointSet(const ProgramPointSet &) = default;
  ProgramPointSet(ProgramPointSet &&) noexcept = default;
  ProgramPointSet &operator=(const ProgramPointSet &) = default;
  ProgramPointSet &operator=(ProgramPointSet &&) = default;

  bool insert(const ProgramPoint &pp) { return ppSet.insert(pp).second; }

  const_iterator begin() const { return ppSet.begin(); }
  const_iterator end() const { return ppSet.end(); }
  size_t size() const { return ppSet.size(); }
  bool empty() const { return ppSet.empty(); }
};

} // namespace tpa::taint