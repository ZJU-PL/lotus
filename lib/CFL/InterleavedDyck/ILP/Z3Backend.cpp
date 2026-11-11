#include "Backend.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <z3++.h>

namespace lotus::cfl::interleaved_dyck::ilp {
namespace {

/// Reuses one Z3 context across all disjuncts. A fresh solver is created per
/// feasibility check, which keeps assertions independent without paying the
/// context construction and SMT-LIB parsing cost again.
class Z3Backend final : public SolverBackend {
public:
  Feasibility check(const LinearProgram &program) override {
    try {
      z3::solver solver(context_, "QF_LIA");
      std::vector<z3::expr> variables;
      variables.reserve(program.variables.size());
      for (std::size_t i = 0; i < program.variables.size(); ++i) {
        const std::string name = "v" + std::to_string(i);
        variables.push_back(context_.int_const(name.c_str()));
        if (program.variables[i].nonnegative)
          solver.add(variables.back() >= 0);
      }
      for (const auto &constraint : program.constraints) {
        z3::expr lhs = context_.int_val(0);
        for (const auto &term : constraint.coefficients) {
          const auto index = static_cast<std::size_t>(term.first);
          lhs = lhs + variables.at(index) * context_.int_val(term.second);
        }
        const z3::expr rhs = context_.int_val(constraint.rhs);
        switch (constraint.sense) {
        case LinearProgram::Sense::Equal:
          solver.add(lhs == rhs);
          break;
        case LinearProgram::Sense::GreaterEqual:
          solver.add(lhs >= rhs);
          break;
        case LinearProgram::Sense::LessEqual:
          solver.add(lhs <= rhs);
          break;
        }
      }
      switch (solver.check()) {
      case z3::sat:
        return Feasibility::Sat;
      case z3::unsat:
        return Feasibility::Unsat;
      case z3::unknown:
        return Feasibility::Unknown;
      }
      return Feasibility::Unknown;
    } catch (const z3::exception &error) {
      throw std::runtime_error(std::string("ILP/Z3: ") + error.what());
    }
  }

private:
  z3::context context_;
};

} // namespace

std::unique_ptr<SolverBackend> makeZ3Backend() {
  return std::make_unique<Z3Backend>();
}

} // namespace lotus::cfl::interleaved_dyck::ilp
