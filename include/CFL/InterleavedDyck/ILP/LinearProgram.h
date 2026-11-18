#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::ilp {

/// A small integer linear feasibility problem. The ILP encoder only needs
/// integer variables, linear equality/inequality constraints, and a yes/no
/// answer, so the backend interface stays independent of any solver library.
struct LinearProgram {
  struct Variable {
    bool nonnegative = false;
  };
  enum class Sense { Equal, GreaterEqual, LessEqual };
  struct Constraint {
    std::vector<std::pair<int, std::int64_t>> coefficients;
    std::int64_t rhs = 0;
    Sense sense = Sense::Equal;
  };

  std::vector<Variable> variables;
  std::vector<Constraint> constraints;
};

enum class Feasibility { Sat, Unsat, Unknown };

} // namespace lotus::cfl::interleaved_dyck::ilp
