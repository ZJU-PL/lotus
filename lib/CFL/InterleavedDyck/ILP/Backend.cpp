#include "Backend.h"

#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::ilp {

bool backendAvailable(Backend backend) {
  switch (backend) {
  case Backend::Z3:
    return true;
  case Backend::Gurobi:
#ifdef LOTUS_HAVE_GUROBI
    return true;
#else
    return false;
#endif
  }
  return false;
}

std::unique_ptr<SolverBackend> makeBackend(Backend backend) {
  switch (backend) {
  case Backend::Z3:
    return makeZ3Backend();
  case Backend::Gurobi:
#ifdef LOTUS_HAVE_GUROBI
    return makeGurobiBackend();
#else
    throw std::invalid_argument(
        "ILP: Gurobi backend is not compiled in; rebuild with "
        "-DLOTUS_ENABLE_GUROBI=ON");
#endif
  }
  throw std::invalid_argument("ILP: unknown solver backend");
}

} // namespace lotus::cfl::interleaved_dyck::ilp
