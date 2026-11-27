//===----------------------------------------------------------------------===//
//
// Bug query representation and SMT encoding.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Checks/Query.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace SymbolicExecution;

NumericalQueryPtr::NumericalQueryPtr(
    const std::shared_ptr<DirectNumericalQuery> &V)
    : Data(V) {}

NumericalQueryPtr::NumericalQueryPtr(
    const std::shared_ptr<IndirectNumericalQuery> &V)
    : Data(V) {}

bool NumericalQueryPtr::operator==(const NumericalQueryPtr &R) const {
  if (Data == R.Data) {
    return true;
  }

  auto *Op1 = get();
  auto *Op2 = R.get();
  if (!Op1 || !Op2 || Op1->getKind() != Op2->getKind() ||
      Op1->getBugTy() != Op2->getBugTy()) {
    return false;
  }

  if (isa<DirectNumericalQuery>(Op1)) {
    return *cast<DirectNumericalQuery>(Op1) == *cast<DirectNumericalQuery>(Op2);
  }
  return *cast<IndirectNumericalQuery>(Op1) ==
         *cast<IndirectNumericalQuery>(Op2);
}

size_t NumericalQueryPtr::hash() const { return Data ? Data->hash() : 0; }

void NumericalQueryPtr::translate(PathCondSolver *Solver) const {
  if (Data) {
    Data->translate(Solver);
  }
}

NumericalQueryPtr NumericalQuery::clone() const {
  if (QK == QK_DIRECT) {
    return std::make_shared<DirectNumericalQuery>(
        *cast<DirectNumericalQuery>(this));
  }
  return std::make_shared<IndirectNumericalQuery>(
      *cast<IndirectNumericalQuery>(this));
}

void NumericalQuery::dumpDeps() const {
  llvm::errs() << "Deps: ";
  for (const auto &P : Deps) {
    llvm::errs() << P.first.getID() << "\n";
  }
}

NumericalQueryPtr DirectNumericalQuery::createMustErr(SymexBugType BugTy) {
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(BugTy));
}

NumericalQueryPtr
DirectNumericalQuery::createTaintOnly(SymexBugType BugTy,
                                      const GuardedProgramValSet &Deps) {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(BugTy, Deps));
}

NumericalQueryPtr
DirectNumericalQuery::createSymbolic(SymexBugType BugTy,
                                     const PropertyValuePtr &Qr, bool Eq,
                                     const GuardedProgramValSet &Deps) {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(BugTy, Qr, Eq, Deps));
}

void DirectNumericalQuery::dump() const {
  llvm::errs() << "Direct Query:";
  if (mustSat()) {
    llvm::errs() << "must err!\n";
  } else if (!Q) {
    llvm::errs() << "null query expr!\n";
  } else {
    Q->dump();
  }
}

SMTExprVec DirectNumericalQuery::toSMT(PathCondSolver &Solver) const {
  SMTExprVec RetVec = Solver.createEmptySMTExprVec();
  if (!Q) {
    return RetVec;
  }

  SMTExpr ResExpr = Solver.buildExprForVal(Q.get());
  if (!ResExpr.isBitVector() || ResExpr.getBitVecSize() == 0) {
    return RetVec;
  }
  SMTExpr CstZero = Solver.buildBitVecVal(0, ResExpr.getBitVecSize());
  if (Eq) {
    RetVec.push_back(ResExpr == CstZero);
  } else {
    RetVec.push_back(ResExpr.basic_sgt(CstZero));
  }
  return RetVec;
}

NumericalQueryPtr IndirectNumericalQuery::createBof(
    const ProgramValuePtr &BasePtr, const PropertyValuePtr &Offset,
    const PropertyValuePtr &AccSize, const GuardedProgramValSet &Deps) {
  return std::shared_ptr<IndirectNumericalQuery>(new IndirectNumericalQuery(
      BUG_TY_BOF, BasePtr, Offset, AccSize, Deps));
}

void IndirectNumericalQuery::dump() const {
  llvm::errs() << "Indirect query:\n";
  llvm::errs() << "Base:" << BasePtr.getID() << ", Offset:";
  Offset->dump();
  llvm::errs() << "Acc Size:";
  AccSize->dump();
  dumpDeps();
}
