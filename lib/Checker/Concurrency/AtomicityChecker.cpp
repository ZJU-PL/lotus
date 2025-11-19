//===----------------------------------------------------------------------===//
// AtomicityChecker.cpp – detect atomicity violations
// Implements a two–phase algorithm:
//
//  1. Discover critical sections (acquire … release pairs) once per function.
//  2. Compare memory accesses of critical-section pairs that may run in
//     parallel according to MHPAnalysis.
//
// This version uses modern LLVM ranges, dominance / post-dominance matching,
// SmallVector / DenseMap for performance, and emits user-friendly diagnostics.
//===----------------------------------------------------------------------===//

#include "Checker/Concurrency/AtomicityChecker.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/DominanceFrontier.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>

using namespace llvm;
using namespace mhp;

namespace concurrency {

//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
// Helpers
//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

static std::string formatLoc(const Instruction &I) {
  if (const DebugLoc &DL = I.getDebugLoc()) {
    return (Twine(DL->getFilename()) + ":" + Twine(DL->getLine())).str();
  }
  // Fallback: print function and basic-block name.
  std::string S;
  raw_string_ostream OS(S);
  OS << I.getFunction()->getName() << ':' << I.getParent()->getName();
  return OS.str();
}

//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
// Construction
//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

AtomicityChecker::AtomicityChecker(Module &M, MHPAnalysis *MHP,
                                   LockSetAnalysis *LSA, ThreadAPI *TAPI)
    : m_module(M), m_mhpAnalysis(MHP), m_locksetAnalysis(LSA),
      m_threadAPI(TAPI) {}

//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
// Phase 0 – collect critical sections
//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

void AtomicityChecker::collectCriticalSections() {
  m_csPerFunc.clear();

  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;

    DominatorTree DT(F);
    PostDominatorTree PDT(F);

    SmallVector<const Instruction *, 4> LockStack;

    for (Instruction &I : instructions(F)) {
      if (m_threadAPI->isTDAcquire(&I)) {
        LockStack.push_back(&I);
        continue;
      }

      if (m_threadAPI->isTDRelease(&I) && !LockStack.empty()) {
        const Instruction *Acq = LockStack.pop_back_val();
        if (!Acq)
          continue;

        const Instruction *Rel = &I;

        // Validate the pair with dominance / post-dominance.
        if (!(DT.dominates(Acq, Rel) &&
              PDT.dominates(Rel, F.getEntryBlock().getTerminator())))
          continue;

        // Build the critical section body.
        CriticalSection CS{Acq, Rel};
        bool InBody = false;
        for (Instruction &J : instructions(F)) {
          if (&J == Acq)
            InBody = true;
          else if (&J == Rel)
            InBody = false;
          else if (InBody)
            CS.Body.push_back(&J);
        }
        m_csPerFunc[&F].push_back(std::move(CS));
      }
    }
  }
}

//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
// Phase 1 – bug detection
//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

bool AtomicityChecker::isMemoryAccess(const Instruction *inst) const {
  return isa<LoadInst>(inst) || isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) ||
         isa<AtomicCmpXchgInst>(inst);
}

static bool isWrite(const Instruction &I) {
  if (auto *S = dyn_cast<StoreInst>(&I))
    return !S->isVolatile();
  if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
    return true;
  if (auto *CAS = dyn_cast<AtomicCmpXchgInst>(&I))
    return true;
  return false;
}

std::vector<ConcurrencyBugReport> AtomicityChecker::checkAtomicityViolations() {
  collectCriticalSections(); // build cache once

  std::vector<ConcurrencyBugReport> Reports;

  // Compare every pair of CS that may run in parallel.
  for (auto &FuncPair : m_csPerFunc) {
    auto &Sections = FuncPair.second;
    for (size_t i = 0; i < Sections.size(); ++i) {
      for (size_t j = i + 1; j < Sections.size(); ++j) {
        const CriticalSection &CS1 = Sections[i];
        const CriticalSection &CS2 = Sections[j];

        // Cheap filter: if acquires are on different locks, skip.
        if (m_threadAPI->getLockVal(CS1.Acquire) !=
            m_threadAPI->getLockVal(CS2.Acquire))
          continue;

        // May these CS execute concurrently?
        if (!m_mhpAnalysis->mayHappenInParallel(CS1.Acquire, CS2.Acquire))
          continue;

        // Compare memory accesses.
        for (const Instruction *I1 : CS1.Body) {
          if (!isMemoryAccess(I1))
            continue;

          for (const Instruction *I2 : CS2.Body) {
            if (!isMemoryAccess(I2))
              continue;

            // At least one write?
            if (!(isWrite(*I1) || isWrite(*I2)))
              continue;

            // Found a potential violation.
            std::string Desc =
                "Potential atomicity violation between accesses at " +
                formatLoc(*I1) + " and " + formatLoc(*I2);

            Reports.emplace_back(ConcurrencyBugType::ATOMICITY_VIOLATION, I1,
                                 I2, std::move(Desc),
                                 BugDescription::BI_MEDIUM,
                                 BugDescription::BC_WARNING);
          }
        }
      }
    }
  }
  return Reports; // NRVO — no extra copy
}

//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
// Thin wrappers delegating to ThreadAPI
//―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

bool AtomicityChecker::isAcquire(const Instruction *I) const {
  return m_threadAPI->isTDAcquire(I);
}
bool AtomicityChecker::isRelease(const Instruction *I) const {
  return m_threadAPI->isTDRelease(I);
}

} // namespace concurrency