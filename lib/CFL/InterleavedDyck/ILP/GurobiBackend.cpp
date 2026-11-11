// Optional Gurobi backend. This file is added to CanaryInterleavedDyckILP only
// when LOTUS_ENABLE_GUROBI=ON; the default build neither requires nor links
// Gurobi. Users must provide their own Gurobi installation and license.
//
// TBD: validate this backend and benchmark it against Z3 before making it a
// default or exposing it as a separate CLI engine. The CLI accepts
// `--backend gurobi` only in builds that define LOTUS_HAVE_GUROBI.
#include "Backend.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <gurobi_c++.h>

namespace lotus::cfl::interleaved_dyck::ilp {
namespace {

class GurobiBackend final : public SolverBackend {
public:
  GurobiBackend() : environment_(false) {
    environment_.set(GRB_IntParam_OutputFlag, 0);
  }

  Feasibility check(const LinearProgram &program) override {
    try {
      GRBModel model(environment_);
      model.set(GRB_IntParam_OutputFlag, 0);

      std::vector<GRBVar> variables;
      variables.reserve(program.variables.size());
      for (const auto &variable : program.variables) {
        const double lower = variable.nonnegative ? 0.0 : -GRB_INFINITY;
        variables.push_back(
            model.addVar(lower, GRB_INFINITY, 0.0, GRB_INTEGER));
      }
      model.update();

      for (const auto &constraint : program.constraints) {
        GRBLinExpr lhs = 0.0;
        for (const auto &term : constraint.coefficients) {
          const auto index = static_cast<std::size_t>(term.first);
          lhs += term.second * variables.at(index);
        }
        switch (constraint.sense) {
        case LinearProgram::Sense::Equal:
          model.addConstr(lhs == constraint.rhs);
          break;
        case LinearProgram::Sense::GreaterEqual:
          model.addConstr(lhs >= constraint.rhs);
          break;
        case LinearProgram::Sense::LessEqual:
          model.addConstr(lhs <= constraint.rhs);
          break;
        }
      }

      model.optimize();
      switch (model.get(GRB_IntAttr_Status)) {
      case GRB_OPTIMAL:
        return Feasibility::Sat;
      case GRB_INFEASIBLE:
        return Feasibility::Unsat;
      case GRB_TIME_LIMIT:
      case GRB_ITERATION_LIMIT:
      case GRB_NODE_LIMIT:
      case GRB_INTERRUPTED:
      case GRB_INF_OR_UNBOUNDED:
      case GRB_UNBOUNDED:
        return Feasibility::Unknown;
      default:
        return Feasibility::Unknown;
      }
    } catch (const GRBException &error) {
      throw std::runtime_error("ILP/Gurobi: " + error.getMessage());
    }
  }

private:
  GRBEnv environment_;
};

} // namespace

std::unique_ptr<SolverBackend> makeGurobiBackend() {
  return std::make_unique<GurobiBackend>();
}

} // namespace lotus::cfl::interleaved_dyck::ilp
