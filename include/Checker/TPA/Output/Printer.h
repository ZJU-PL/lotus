#pragma once

#include "Alias/InclusionBased/TPA/Util/IO/PointerAnalysis/Printer.h"
#include "Checker/TPA/Lattice/TaintLattice.h"

namespace tpa::taint {
class DefUseInstruction;
class ProgramPoint;
} // namespace tpa::taint

namespace util {
namespace io {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              tpa::taint::TaintLattice l);
llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const tpa::taint::DefUseInstruction &);
llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const tpa::taint::ProgramPoint &);

} // namespace io
} // namespace util
