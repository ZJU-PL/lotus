#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/ProgramPoint.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

using namespace tpa::taint;

namespace util {
namespace io {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, TaintLattice l) {
  switch (l) {
  case TaintLattice::Unknown:
    os << "Unknown";
    break;
  case TaintLattice::Untainted:
    os << "Untainted";
    break;
  case TaintLattice::Tainted:
    os << "Tainted";
    break;
  case TaintLattice::Either:
    os << "Either";
    break;
  }
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const tpa::taint::DefUseInstruction &duInst) {
  if (duInst.isEntryInstruction())
    os << "[entry duInst for " << duInst.getFunction()->getName() << "]";
  else
    os << "[" << duInst.getFunction()->getName() << *duInst.getInstruction()
       << "]";
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const tpa::taint::ProgramPoint &pp) {
  os << "(" << *pp.getContext() << ", " << *pp.getDefUseInstruction() << ")";
  return os;
}

} // namespace io
} // namespace util