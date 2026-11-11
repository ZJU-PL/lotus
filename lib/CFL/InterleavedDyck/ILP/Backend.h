#pragma once

#include "CFL/InterleavedDyck/ILP/Solver.h"
#include "LinearProgram.h"

#include <memory>

namespace lotus::cfl::interleaved_dyck::ilp {

/// Internal interface implemented by each compiled-in ILP backend.
class SolverBackend {
public:
  virtual ~SolverBackend() = default;
  virtual Feasibility check(const LinearProgram &program) = 0;
};

std::unique_ptr<SolverBackend> makeZ3Backend();

// Defined only when LOTUS_ENABLE_GUROBI adds GurobiBackend.cpp to the target.
std::unique_ptr<SolverBackend> makeGurobiBackend();

std::unique_ptr<SolverBackend> makeBackend(Backend backend);

} // namespace lotus::cfl::interleaved_dyck::ilp
