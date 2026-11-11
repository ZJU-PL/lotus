#include <iostream>
#include <string_view>

int runInterleavedDyckAffineSPDS(int argc, char **argv);
int runInterleavedDyckSPDS(int argc, char **argv);
int runInterleavedDyckUnary(int argc, char **argv);
int runInterleavedDyckStagedBounds(int argc, char **argv);
int runInterleavedDyckMCFL(int argc, char **argv);
int runInterleavedDyckLCL(int argc, char **argv);
int runInterleavedDyckILP(int argc, char **argv);
int runInterleavedDyckGraphAux(int argc, char **argv);
int runInterleavedDyckDkMerge();

namespace {
void usage(std::ostream &out) {
  out << "usage: lotus-cfl-interleaved-dyck <engine> [engine options]\n"
         "engines: lcl, ilp, spds, affine-spds, unary, staged-bounds, mcfl\n"
         "graph reduction helpers: graphaux, dkmerge\n"
         "run a solver engine with --help for its options\n";
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  int first_argument = 1;
  if (argc > 1 && std::string_view(argv[1]) == "--engine")
    first_argument = 2;
  if (argc <= first_argument) {
    usage(std::cerr);
    return 2;
  }

  const std::string_view engine = argv[first_argument];
  const int engine_argc = argc - first_argument;
  char **engine_argv = argv + first_argument;
  if (engine == "lcl")
    return runInterleavedDyckLCL(engine_argc, engine_argv);
  if (engine == "ilp")
    return runInterleavedDyckILP(engine_argc, engine_argv);
  if (engine == "spds")
    return runInterleavedDyckSPDS(engine_argc, engine_argv);
  if (engine == "affine-spds")
    return runInterleavedDyckAffineSPDS(engine_argc, engine_argv);
  if (engine == "unary")
    return runInterleavedDyckUnary(engine_argc, engine_argv);
  if (engine == "staged-bounds")
    return runInterleavedDyckStagedBounds(engine_argc, engine_argv);
  if (engine == "mcfl")
    return runInterleavedDyckMCFL(engine_argc, engine_argv);
  if (engine == "graphaux")
    return runInterleavedDyckGraphAux(engine_argc, engine_argv);
  if (engine == "dkmerge") {
    if (engine_argc != 1) {
      std::cerr << "dkmerge takes no arguments\n";
      return 2;
    }
    return runInterleavedDyckDkMerge();
  }

  std::cerr << "unknown interleaved-Dyck engine: " << engine << '\n';
  usage(std::cerr);
  return 2;
}
