/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/ConditionVariableChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/Core/ConcurrencyContext.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

ConditionVariableChecker::ConditionVariableChecker(
    Module &module, ThreadAPI *threadAPI, LockSetAnalysis *locksetAnalysis)
    : m_module(module), m_threadAPI(threadAPI),
      m_locksetAnalysis(locksetAnalysis) {}

bool ConditionVariableChecker::sameSyncObject(const Value *lhs,
                                              const Value *rhs) const {
  if (!lhs || !rhs)
    return false;
  const Value *s1 = lhs->stripPointerCasts();
  const Value *s2 = rhs->stripPointerCasts();
  if (s1 == s2)
    return true;
  if (m_locksetAnalysis && m_locksetAnalysis->locksMustMatch(s1, s2))
    return true;
  if (m_aliasAnalysis && s1->getType()->isPointerTy() &&
      s2->getType()->isPointerTy()) {
    return m_aliasAnalysis->mayAlias(s1, s2);
  }
  const Value *u1 = getUnderlyingObject(s1);
  const Value *u2 = getUnderlyingObject(s2);
  return u1 && u2 && u1 == u2;
}

bool ConditionVariableChecker::isWaitInsideLoop(
    const Instruction *waitInst) const {
  if (!waitInst || !waitInst->getParent() || !waitInst->getFunction())
    return false;
  auto instInLoop = [&](const Instruction *inst) -> bool {
    const Function *fn = inst->getFunction();
    if (!fn)
      return false;
    if (m_context) {
      const LoopInfo &LI = m_context->getLoopInfo(*fn);
      return LI.getLoopFor(inst->getParent()) != nullptr;
    }
    DominatorTree DT(const_cast<Function &>(*fn));
    LoopInfo LI(DT);
    return LI.getLoopFor(inst->getParent()) != nullptr;
  };
  if (instInLoop(waitInst))
    return true;

  // If `func` is a thin cond_wait helper wrapper (taking the condvar as a
  // parameter), check whether all direct call sites invoke it inside a loop.
  const Function *func = waitInst->getFunction();
  const Value *cond = m_threadAPI ? m_threadAPI->getCondVal(waitInst) : nullptr;
  const Value *condBase = cond ? getUnderlyingObject(cond) : nullptr;
  if (!condBase || !isa<Argument>(condBase))
    return false;
  bool foundCaller = false;
  for (const User *u : func->users()) {
    const auto *cb = dyn_cast<CallBase>(u);
    if (!cb || cb->getCalledFunction() != func)
      continue;
    foundCaller = true;
    if (!instInLoop(cb))
      return false;
  }
  return foundCaller;
}

std::vector<ConcurrencyBugReport>
ConditionVariableChecker::checkConditionVariables() {
  std::vector<ConcurrencyBugReport> reports;

  struct WaitRecord {
    const Instruction *inst = nullptr;
    const Value *cond = nullptr;
    const Value *mutex = nullptr;
  };
  SmallVector<WaitRecord, 8> waitRecords;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (auto &bb : func) {
      for (auto &inst : bb) {
        if (m_threadAPI->isTDCondWait(&inst)) {
          const Value *cond = m_threadAPI->getCondVal(&inst);
          const Value *mutex = m_threadAPI->getCondMutex(&inst);
          waitRecords.push_back(
              {&inst, cond ? cond->stripPointerCasts() : nullptr,
               mutex ? mutex->stripPointerCasts() : nullptr});
        }
      }
    }
  }

  // Check for the same condition variable bound to multiple provably distinct
  // mutexes (undefined behavior in POSIX and C++). Require must-same identity
  // on the condvar (so distinct heap-allocated condvars from one allocator do
  // not false-positive) and provably distinct mutexes.
  auto mustBeSameCondVar = [&](const Value *lhs, const Value *rhs) -> bool {
    if (!lhs || !rhs)
      return false;
    const Value *s1 = lhs->stripPointerCasts();
    const Value *s2 = rhs->stripPointerCasts();
    if (s1 == s2)
      return true;
    if (m_locksetAnalysis && m_locksetAnalysis->locksMustMatch(s1, s2))
      return true;
    const DataLayout &DL = m_module.getDataLayout();
    int64_t off1 = 0, off2 = 0;
    const Value *b1 = GetPointerBaseWithConstantOffset(s1, off1, DL);
    const Value *b2 = GetPointerBaseWithConstantOffset(s2, off2, DL);
    if (b1 && b2 && b1->stripPointerCasts() == b2->stripPointerCasts() &&
        off1 == off2 && isa<GlobalValue>(b1->stripPointerCasts())) {
      return true;
    }
    return false;
  };

  for (size_t i = 0; i < waitRecords.size(); ++i) {
    if (!waitRecords[i].cond || !waitRecords[i].mutex)
      continue;
    for (size_t j = i + 1; j < waitRecords.size(); ++j) {
      if (!waitRecords[j].cond || !waitRecords[j].mutex)
        continue;
      if (!mustBeSameCondVar(waitRecords[i].cond, waitRecords[j].cond))
        continue;
      if (!sameSyncObject(waitRecords[i].mutex, waitRecords[j].mutex)) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::COND_VAR_MISUSE,
            "Condition variable associated with multiple distinct mutexes",
            BugDescription::BI_HIGH, BugDescription::BC_ERROR);
        report.addStep(waitRecords[i].inst,
                       "First condition variable wait with one mutex");
        report.addStep(waitRecords[j].inst,
                       "Second condition variable wait with a different mutex");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (auto &bb : func) {
      for (auto &inst : bb) {
        // Advisory check for signal/broadcast without holding the associated
        // mutex: POSIX and C++ allow unlock-before-notify, so report as LOW
        // WARNING rather than HIGH ERROR.
        if (m_threadAPI->isTDCondSignal(&inst) ||
            m_threadAPI->isTDCondBroadcast(&inst)) {
          const Value *cond = m_threadAPI->getCondVal(&inst);
          if (!cond)
            continue;
          cond = cond->stripPointerCasts();
          bool mutexHeld = false;
          bool anyMatchingCond = false;
          if (m_locksetAnalysis) {
            for (const auto &w : waitRecords) {
              if (w.cond && w.mutex && sameSyncObject(w.cond, cond)) {
                anyMatchingCond = true;
                if (m_locksetAnalysis->mayHoldLock(&inst, w.mutex)) {
                  mutexHeld = true;
                  break;
                }
              }
            }
          }
          bool noLockHeld = m_locksetAnalysis &&
                            m_locksetAnalysis->getMayLockSetAt(&inst).empty();
          if (!mutexHeld &&
              (anyMatchingCond || (waitRecords.empty() && noLockHeld))) {
            const char *op =
                m_threadAPI->isTDCondSignal(&inst) ? "signal" : "broadcast";
            ConcurrencyBugReport report(
                ConcurrencyBugType::COND_VAR_MISUSE,
                std::string("Condition variable ") + op +
                    " called without holding the associated mutex (advisory: "
                    "may risk lost wakeup if predicate update was not guarded)",
                BugDescription::BI_LOW, BugDescription::BC_WARNING);
            report.addStep(&inst, op);
            reports.push_back(std::move(report));
          }
          continue;
        }

        if (m_threadAPI->isTDCondWait(&inst)) {
          const Value *mutex = getMutexForCV(&inst);
          if (mutex)
            mutex = mutex->stripPointerCasts();

          // 1. Check if wait is called with a mutex.
          if (!mutex) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::COND_VAR_MISUSE,
                "Condition variable wait called without a mutex",
                BugDescription::BI_HIGH, BugDescription::BC_ERROR);
            report.addStep(&inst, "Wait called here");
            reports.push_back(std::move(report));
            continue;
          }

          // 2. Check if the associated mutex is held at the wait site.
          if (m_locksetAnalysis) {
            if (!m_locksetAnalysis->mayHoldLock(&inst, mutex)) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::COND_VAR_MISUSE,
                  "Mutex not held when calling condition variable wait",
                  BugDescription::BI_HIGH, BugDescription::BC_ERROR);
              report.addStep(&inst, "Wait called without holding lock");
              reports.push_back(std::move(report));
            }

            // 3. Check if an additional unrelated mutex is definitely held
            // while sleeping in cond_wait (classic deadlock hazard).
            LockSet mustHeld = m_locksetAnalysis->getMustLockSetAt(&inst);
            for (LockID held : mustHeld) {
              if (!held || sameSyncObject(held, mutex))
                continue;
              ConcurrencyBugReport report(
                  ConcurrencyBugType::COND_VAR_MISUSE,
                  "Condition variable wait called while holding another "
                  "unrelated mutex (deadlock risk)",
                  BugDescription::BI_HIGH, BugDescription::BC_ERROR);
              report.addStep(
                  &inst,
                  "Wait only releases its own mutex while another lock stays "
                  "held");
              reports.push_back(std::move(report));
              break;
            }
          }

          // 4. Spurious wakeup risk: cond_wait not enclosed in a loop.
          if (!isWaitInsideLoop(&inst)) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::COND_VAR_MISUSE,
                "Condition variable wait is not enclosed in a loop (vulnerable "
                "to spurious wakeups)",
                BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
            report.addStep(&inst, "Wait outside predicate-checking loop");
            reports.push_back(std::move(report));
          }
        }
      }
    }
  }

  return reports;
}

const Value *
ConditionVariableChecker::getMutexForCV(const Instruction *waitInst) const {
  return m_threadAPI->getCondMutex(waitInst);
}

std::string ConditionVariableChecker::getInstructionLocation(
    const Instruction *inst) const {
  std::string location;
  raw_string_ostream os(location);
  if (const Function *func = inst->getFunction())
    os << func->getName();
  if (const BasicBlock *bb = inst->getParent())
    os << ":" << bb->getName();
  return os.str();
}

} // namespace concurrency
