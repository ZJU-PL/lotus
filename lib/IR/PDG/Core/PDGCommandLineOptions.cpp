#include "llvm/Support/CommandLine.h"

#include "IR/PDG/Core/PDGCommandLineOptions.h"

using namespace llvm;

bool pdg::DEBUG;

cl::opt<bool, true> DEBUG("pdg-debug", cl::desc("print debug messages"),
                          cl::value_desc("print debug messages"),
                          cl::location(pdg::DEBUG), cl::init(false));
