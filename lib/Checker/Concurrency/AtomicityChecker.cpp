//===----------------------------------------------------------------------===//
// AtomicityChecker.cpp – detect atomicity violations
// Implements two complementary passes:
//
//  1. CFG/dominance-validated critical section discovery and comparison of
//     conflicting parallel critical sections under disjoint locks.
//  2. Classic 3-access unserializable interleaving detection (R-W-R, R-W-W,
//     W-W-R, W-R-W, W-W-W) across:
//       - split critical sections (lock; A1; unlock; ...; lock; A2; unlock;),
//       - mixed-lock or atomic check-then-act sequences, and
//       - unprotected check-then-act / read-modify-write sequences with local
//         data or control dependence between A1 and A2.
//
//  Author: rainoftime
//===----------------------------------------------------------------------===//

#include "Checker/Concurrency/AtomicityChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/Core/ConcurrencyContext.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/Thread/Sharing/EscapeAnalysis.h"
#include "Concurrency/Thread/Sharing/StaticThreadSharingAnalysis.h"
#include "Concurrency/Thread/Sharing/ThreadLocalAnalysis.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

namespace {

std::string formatLoc(const Instruction &I) {
  if (const DebugLoc &DL = I.getDebugLoc()) {
    return (Twine(DL->getFilename()) + ":" + Twine(DL->getLine())).str();
  }
  std::string S;
  raw_string_ostream OS(S);
  OS << I.getFunction()->getName() << ':' << I.getParent()->getName();
  return OS.str();
}

bool isWrite(const Instruction &I) {
  if (auto *S = dyn_cast<StoreInst>(&I))
    return !S->isVolatile();
  if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I))
    return true;
  return false;
}

bool isAtomicInst(const Instruction &I) {
  if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I))
    return true;
  if (const auto *L = dyn_cast<LoadInst>(&I))
    return L->isAtomic();
  if (const auto *S = dyn_cast<StoreInst>(&I))
    return S->isAtomic();
  return false;
}

uint64_t getAccessByteSize(const Instruction *inst, const DataLayout &DL) {
  if (!inst)
    return 0;
  Type *ty = nullptr;
  if (const auto *L = dyn_cast<LoadInst>(inst))
    ty = L->getType();
  else if (const auto *S = dyn_cast<StoreInst>(inst))
    ty = S->getValueOperand()->getType();
  else if (const auto *RMW = dyn_cast<AtomicRMWInst>(inst))
    ty = RMW->getValOperand()->getType();
  else if (const auto *CAS = dyn_cast<AtomicCmpXchgInst>(inst))
    ty = CAS->getCompareOperand()->getType();
  if (!ty || !ty->isSized())
    return 0;
  return DL.getTypeStoreSize(ty);
}

bool isIgnorableStructType(const StructType *st) {
  if (!st || !st->hasName())
    return false;
  StringRef name = st->getName();
  if (name.startswith("struct."))
    name = name.drop_front(7);
  else if (name.startswith("class."))
    name = name.drop_front(6);
  else if (name.startswith("union."))
    name = name.drop_front(6);
  if (name.startswith("std::__1::"))
    name = name.drop_front(10);
  else if (name.startswith("std::"))
    name = name.drop_front(5);
  if (size_t dot = name.find('.'); dot != StringRef::npos) {
    StringRef suffix = name.substr(dot + 1);
    bool allDigits = !suffix.empty() &&
                     std::all_of(suffix.begin(), suffix.end(),
                                 [](char c) { return c >= '0' && c <= '9'; });
    if (allDigits)
      name = name.substr(0, dot);
  }
  static const char *kIgnorable[] = {
      "pthread_mutex_t", "pthread_cond_t", "pthread_barrier_t",
      "pthread_rwlock_t", "pthread_spinlock_t", "pthread_once_t",
      "__pthread_mutex_s", "__pthread_cond_s", "mutex", "recursive_mutex",
      "shared_mutex", "timed_mutex", "condition_variable", "once_flag",
      "atomic_flag", "atomic_t", "atomic", "spinlock_t", "FILE", "__FILE",
      "_IO_FILE", "__jmp_buf_tag"};
  for (const char *ig : kIgnorable) {
    if (name.equals(ig))
      return true;
  }
  return false;
}

bool isIgnorableAccess(const Instruction *inst, const Value *loc) {
  if (!loc)
    return true;
  auto checkType = [](const Type *ty) {
    const auto *st = dyn_cast_or_null<StructType>(ty);
    return st && isIgnorableStructType(st);
  };
  if (const auto *L = dyn_cast_or_null<LoadInst>(inst)) {
    if (checkType(L->getType()))
      return true;
  } else if (const auto *S = dyn_cast_or_null<StoreInst>(inst)) {
    if (checkType(S->getValueOperand()->getType()))
      return true;
  }
  const Value *stripped = loc->stripPointerCasts();
  if (const auto *gep = dyn_cast<GEPOperator>(stripped)) {
    if (checkType(gep->getResultElementType()) ||
        checkType(gep->getSourceElementType()))
      return true;
  }
  const Value *base = getUnderlyingObject(stripped);
  if (const auto *gv = dyn_cast_or_null<GlobalVariable>(base)) {
    if (gv->isThreadLocal() || gv->isConstant() ||
        checkType(gv->getValueType()))
      return true;
  } else if (const auto *ai = dyn_cast_or_null<AllocaInst>(base)) {
    if (checkType(ai->getAllocatedType()))
      return true;
  }
  return false;
}

bool areDisjointConstantOffsetLocations(const Instruction *i1, const Value *p1,
                                        const Instruction *i2, const Value *p2,
                                        const DataLayout &DL) {
  if (!p1 || !p2)
    return false;
  int64_t off1 = 0, off2 = 0;
  const Value *b1 = GetPointerBaseWithConstantOffset(p1, off1, DL);
  const Value *b2 = GetPointerBaseWithConstantOffset(p2, off2, DL);
  if (!b1 || !b2)
    return false;
  b1 = b1->stripPointerCasts();
  b2 = b2->stripPointerCasts();
  if (b1 != b2) {
    const bool b1Distinct = isa<GlobalVariable>(b1) || isa<AllocaInst>(b1);
    const bool b2Distinct = isa<GlobalVariable>(b2) || isa<AllocaInst>(b2);
    return b1Distinct && b2Distinct;
  }
  const uint64_t s1 = getAccessByteSize(i1, DL);
  const uint64_t s2 = getAccessByteSize(i2, DL);
  if (s1 == 0 || s2 == 0)
    return false;
  if (off1 < 0 || off2 < 0)
    return false;
  const uint64_t start1 = static_cast<uint64_t>(off1);
  const uint64_t start2 = static_cast<uint64_t>(off2);
  const uint64_t end1 = start1 + s1;
  const uint64_t end2 = start2 + s2;
  return end1 <= start2 || end2 <= start1;
}

// Per-function cached CFG reachability using compact bitvectors computed lazily
// per source basic block so repeated reachability queries run in O(1).
struct FunctionReachability {
  DenseMap<const BasicBlock *, unsigned> bbToIdx;
  std::vector<const BasicBlock *> idxToBB;
  DenseMap<const Instruction *, unsigned> instOrdinal;
  std::vector<BitVector> reachCache;
  std::vector<bool> reachComputed;

  explicit FunctionReachability(const Function &F) {
    idxToBB.reserve(F.size());
    for (const BasicBlock &BB : F) {
      unsigned id = static_cast<unsigned>(idxToBB.size());
      bbToIdx[&BB] = id;
      idxToBB.push_back(&BB);
      unsigned ord = 0;
      for (const Instruction &I : BB) {
        instOrdinal[&I] = ord++;
      }
    }
    reachCache.resize(idxToBB.size());
    reachComputed.assign(idxToBB.size(), false);
  }

  const BitVector &getReachableFrom(unsigned srcIdx) {
    if (reachComputed[srcIdx])
      return reachCache[srcIdx];
    const unsigned n = static_cast<unsigned>(idxToBB.size());
    BitVector &bits = reachCache[srcIdx];
    bits.resize(n, false);
    SmallVector<unsigned, 32> worklist;
    for (const BasicBlock *succ : successors(idxToBB[srcIdx])) {
      auto it = bbToIdx.find(succ);
      if (it != bbToIdx.end() && !bits.test(it->second)) {
        bits.set(it->second);
        worklist.push_back(it->second);
      }
    }
    while (!worklist.empty()) {
      unsigned cur = worklist.pop_back_val();
      for (const BasicBlock *succ : successors(idxToBB[cur])) {
        auto it = bbToIdx.find(succ);
        if (it != bbToIdx.end() && !bits.test(it->second)) {
          bits.set(it->second);
          worklist.push_back(it->second);
        }
      }
    }
    reachComputed[srcIdx] = true;
    return bits;
  }

  bool canReach(const Instruction *from, const Instruction *to) {
    if (!from || !to || from == to || from->getFunction() != to->getFunction())
      return false;
    const BasicBlock *fromBB = from->getParent();
    const BasicBlock *toBB = to->getParent();
    if (fromBB == toBB) {
      auto fIt = instOrdinal.find(from);
      auto tIt = instOrdinal.find(to);
      if (fIt != instOrdinal.end() && tIt != instOrdinal.end() &&
          fIt->second < tIt->second) {
        return true;
      }
    }
    auto fromIt = bbToIdx.find(fromBB);
    auto toIt = bbToIdx.find(toBB);
    if (fromIt == bbToIdx.end() || toIt == bbToIdx.end())
      return false;
    return getReachableFrom(fromIt->second).test(toIt->second);
  }
};

// Returns true if there exists a CFG path from `from` to `to` along which
// `commonLock` is not must-held at some intermediate instruction.
bool hasUnprotectedGapBetween(const Instruction *from, const Instruction *to,
                              LockID commonLock, const LockSetAnalysis *lsa,
                              FunctionReachability &reach) {
  if (!from || !to || !commonLock || !lsa)
    return true;
  const BasicBlock *fromBB = from->getParent();
  const BasicBlock *toBB = to->getParent();

  if (fromBB == toBB) {
    bool foundToAfterFrom = false;
    for (const Instruction *cur = from->getNextNode(); cur;
         cur = cur->getNextNode()) {
      if (cur == to) {
        foundToAfterFrom = true;
        break;
      }
      if (!lsa->mustHoldLock(cur, commonLock))
        return true;
    }
    if (foundToAfterFrom)
      return false;
  }

  for (const Instruction *cur = from->getNextNode(); cur;
       cur = cur->getNextNode()) {
    if (!lsa->mustHoldLock(cur, commonLock))
      return true;
  }

  auto toIt = reach.bbToIdx.find(toBB);
  if (toIt == reach.bbToIdx.end())
    return true;
  const unsigned toIdx = toIt->second;

  SmallVector<const BasicBlock *, 32> worklist;
  DenseSet<const BasicBlock *> visited;
  for (const BasicBlock *succ : successors(fromBB)) {
    if (visited.insert(succ).second)
      worklist.push_back(succ);
  }
  while (!worklist.empty()) {
    const BasicBlock *bb = worklist.pop_back_val();
    if (bb == toBB) {
      for (const Instruction &I : *bb) {
        if (&I == to)
          break;
        if (!lsa->mustHoldLock(&I, commonLock))
          return true;
      }
      continue;
    }
    auto bbIt = reach.bbToIdx.find(bb);
    if (bbIt == reach.bbToIdx.end())
      continue;
    // Skip blocks that cannot even reach `toBB`.
    if (!reach.getReachableFrom(bbIt->second).test(toIdx))
      continue;
    for (const Instruction &I : *bb) {
      if (!lsa->mustHoldLock(&I, commonLock))
        return true;
    }
    for (const BasicBlock *succ : successors(bb)) {
      if (visited.insert(succ).second)
        worklist.push_back(succ);
    }
  }
  return false;
}

// Local check-then-act / read-modify-write dependence test between A1 and A2:
// returns true if A2 is data-dependent on A1 (including through a local
// unpromoted stack slot) or control-dependent on a branch derived from A1.
bool hasLocalDataOrControlDependence(const Instruction *A1,
                                     const Instruction *A2,
                                     const PostDominatorTree &PDT,
                                     FunctionReachability &reach) {
  if (!A1 || !A2 || A1 == A2 || A1->getType()->isVoidTy())
    return false;

  SmallVector<const Value *, 16> worklist;
  DenseSet<const Value *> visited;
  worklist.push_back(A1);
  visited.insert(A1);
  unsigned budget = 64;

  while (!worklist.empty() && budget-- > 0) {
    const Value *cur = worklist.pop_back_val();
    for (const User *u : cur->users()) {
      const auto *uInst = dyn_cast<Instruction>(u);
      if (!uInst || uInst->getFunction() != A1->getFunction())
        continue;
      if (uInst == A2)
        return true;

      // Control dependence: A1 feeds a conditional branch/switch that guards A2
      // (A2 does not post-dominate the branch block, and A2 is reachable).
      if (const auto *br = dyn_cast<BranchInst>(uInst)) {
        if (br->isConditional() && br->getCondition() == cur) {
          const BasicBlock *condBB = br->getParent();
          if (condBB != A2->getParent() &&
              !PDT.dominates(A2->getParent(), condBB) &&
              reach.canReach(br, A2)) {
            return true;
          }
        }
        continue;
      }
      if (const auto *sw = dyn_cast<SwitchInst>(uInst)) {
        if (sw->getCondition() == cur) {
          const BasicBlock *condBB = sw->getParent();
          if (condBB != A2->getParent() &&
              !PDT.dominates(A2->getParent(), condBB) &&
              reach.canReach(sw, A2)) {
            return true;
          }
        }
        continue;
      }

      // Handle -O0 stack temporary forwarding: store A1 -> %tmp; load %tmp -> A2.
      if (const auto *st = dyn_cast<StoreInst>(uInst)) {
        if (st->getValueOperand() == cur) {
          if (const auto *slot = dyn_cast<AllocaInst>(
                  st->getPointerOperand()->stripPointerCasts())) {
            for (const User *slotUser : slot->users()) {
              if (const auto *ld = dyn_cast<LoadInst>(slotUser)) {
                if (reach.canReach(st, ld) && reach.canReach(ld, A2) &&
                    visited.insert(ld).second) {
                  worklist.push_back(ld);
                }
              }
            }
          }
        }
        continue;
      }

      if (isa<CastInst>(uInst) || isa<BinaryOperator>(uInst) ||
          isa<UnaryOperator>(uInst) || isa<ICmpInst>(uInst) ||
          isa<FCmpInst>(uInst) || isa<SelectInst>(uInst) ||
          isa<PHINode>(uInst) || isa<GetElementPtrInst>(uInst)) {
        if (visited.insert(uInst).second)
          worklist.push_back(uInst);
      }
    }
  }
  return false;
}

} // namespace

AtomicityChecker::AtomicityChecker(Module &M, IMHPAnalysis *MHP,
                                   LockSetAnalysis *LSA, ThreadAPI *TAPI,
                                   lotus::AliasAnalysisWrapper *AA)
    : m_module(M), m_mhpAnalysis(MHP), m_locksetAnalysis(LSA),
      m_threadAPI(TAPI), m_aliasAnalysis(AA) {}

void AtomicityChecker::collectCriticalSections() {
  m_csPerFunc.clear();

  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;

    SmallVector<std::pair<const Instruction *, LockID>, 4> acquires;
    SmallVector<std::pair<const Instruction *, LockID>, 4> releases;
    for (Instruction &I : instructions(F)) {
      if (m_threadAPI->isTDAcquire(&I)) {
        LockID l = m_locksetAnalysis ? m_locksetAnalysis->getLockValue(&I)
                                     : m_threadAPI->getAnalysisLockIdentity(&I);
        if (l)
          acquires.push_back({&I, l->stripPointerCasts()});
      } else if (m_threadAPI->isTDRelease(&I)) {
        LockID l = m_locksetAnalysis ? m_locksetAnalysis->getLockValue(&I)
                                     : m_threadAPI->getAnalysisLockIdentity(&I);
        if (l)
          releases.push_back({&I, l->stripPointerCasts()});
      }
    }
    if (acquires.empty() || releases.empty())
      continue;

    std::unique_ptr<DominatorTree> ownedDT;
    std::unique_ptr<PostDominatorTree> ownedPDT;
    const DominatorTree *DT = nullptr;
    const PostDominatorTree *PDT = nullptr;
    if (m_context) {
      DT = &m_context->getDominatorTree(F);
      PDT = &m_context->getPostDominatorTree(F);
    } else {
      ownedDT = std::make_unique<DominatorTree>(F);
      ownedPDT = std::make_unique<PostDominatorTree>(F);
      DT = ownedDT.get();
      PDT = ownedPDT.get();
    }

    auto locksMatch = [&](LockID a, LockID b) -> bool {
      if (a == b)
        return true;
      return m_locksetAnalysis && m_locksetAnalysis->locksMustMatch(a, b);
    };

    for (const auto &[Acq, AcqLock] : acquires) {
      const Instruction *bestRel = nullptr;
      for (const auto &[Rel, RelLock] : releases) {
        if (!locksMatch(AcqLock, RelLock))
          continue;
        if (!DT->dominates(Acq, Rel) || !PDT->dominates(Rel, Acq))
          continue;
        if (!bestRel || DT->dominates(Rel, bestRel)) {
          bestRel = Rel;
        }
      }
      if (!bestRel)
        continue;

      CriticalSection CS{Acq, bestRel, {}};
      for (Instruction &J : instructions(F)) {
        if (&J == Acq || &J == bestRel || !isMemoryAccess(&J))
          continue;
        if (DT->dominates(Acq, &J) && PDT->dominates(bestRel, &J)) {
          CS.Body.push_back(&J);
        }
      }
      m_csPerFunc[&F].push_back(std::move(CS));
    }
  }
}

bool AtomicityChecker::isMemoryAccess(const Instruction *inst) const {
  return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
         isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

const Value *
AtomicityChecker::getMemoryLocation(const Instruction *inst) const {
  if (!inst)
    return nullptr;
  if (const auto *L = dyn_cast<LoadInst>(inst))
    return L->getPointerOperand();
  if (const auto *S = dyn_cast<StoreInst>(inst))
    return S->getPointerOperand();
  if (const auto *RMW = dyn_cast<AtomicRMWInst>(inst))
    return RMW->getPointerOperand();
  if (const auto *CAS = dyn_cast<AtomicCmpXchgInst>(inst))
    return CAS->getPointerOperand();
  return nullptr;
}

bool AtomicityChecker::mayAlias(const Value *v1, const Value *v2) const {
  if (!v1 || !v2)
    return false;
  if (v1 == v2)
    return true;
  const Value *s1 = v1->stripPointerCasts();
  const Value *s2 = v2->stripPointerCasts();
  if (s1 == s2)
    return true;
  if (m_aliasAnalysis)
    return m_aliasAnalysis->mayAlias(s1, s2);
  return true;
}

std::vector<ConcurrencyBugReport> AtomicityChecker::checkAtomicityViolations() {
  collectCriticalSections();

  std::vector<ConcurrencyBugReport> Reports;
  if (!m_mhpAnalysis || m_mhpAnalysis->isSingleThreaded())
    return Reports;

  const DataLayout &DL = m_module.getDataLayout();

  auto isDefinitelyThreadLocal = [&](const Instruction *inst,
                                     const Value *loc) -> bool {
    if (!inst || !loc)
      return true;
    if (isIgnorableAccess(inst, loc))
      return true;
    if (m_threadLocalAnalysis &&
        m_threadLocalAnalysis->accessesThreadLocalStorage(inst))
      return true;
    if (m_staticThreadSharingAnalysis &&
        m_staticThreadSharingAnalysis->classify(inst) ==
            lotus::StaticThreadSharingAnalysis::SharingClassification::
                DefinitelyThreadLocal)
      return true;
    const Value *base = getUnderlyingObject(loc);
    if (isa<AllocaInst>(base)) {
      if (!m_escapeAnalysis || !m_escapeAnalysis->isEscaped(base))
        return true;
    } else if (m_escapeAnalysis && isa<CallBase>(base) &&
               !m_escapeAnalysis->isEscaped(base)) {
      return true;
    }
    return false;
  };

  auto locationsMayOverlap = [&](const Instruction *i1, const Value *p1,
                                 const Instruction *i2,
                                 const Value *p2) -> bool {
    if (areDisjointConstantOffsetLocations(i1, p1, i2, p2, DL))
      return false;
    return mayAlias(p1, p2);
  };

  // Phase 1: Compare conflicting parallel critical sections protected by
  // different locks (without a common outer must-held lock).
  SmallVector<std::pair<const CriticalSection *, mhp::LockID>, 16> AllSections;
  DenseMap<const Instruction *, SmallVector<const CriticalSection *, 2>>
      instToCS;
  for (auto &FuncPair : m_csPerFunc) {
    for (const auto &CS : FuncPair.second) {
      LockID l = m_locksetAnalysis
                     ? m_locksetAnalysis->getLockValue(CS.Acquire)
                     : m_threadAPI->getAnalysisLockIdentity(CS.Acquire);
      AllSections.push_back({&CS, l});
      for (const Instruction *bodyInst : CS.Body) {
        instToCS[bodyInst].push_back(&CS);
      }
    }
  }

  DenseMap<const Function *, std::unique_ptr<DominatorTree>> domCache;
  DenseMap<const Function *, std::unique_ptr<PostDominatorTree>> postDomCache;
  auto getDomTree = [&](const Function *F) -> const DominatorTree & {
    if (m_context)
      return m_context->getDominatorTree(*F);
    auto &entry = domCache[F];
    if (!entry)
      entry = std::make_unique<DominatorTree>(const_cast<Function &>(*F));
    return *entry;
  };
  auto getPostDomTree = [&](const Function *F) -> const PostDominatorTree & {
    if (m_context)
      return m_context->getPostDominatorTree(*F);
    auto &entry = postDomCache[F];
    if (!entry)
      entry = std::make_unique<PostDominatorTree>(const_cast<Function &>(*F));
    return *entry;
  };

  for (size_t i = 0; i < AllSections.size(); ++i) {
    const CriticalSection &CS1 = *AllSections[i].first;
    mhp::LockID Lock1 = AllSections[i].second;
    if (!Lock1)
      continue;

    for (size_t j = i + 1; j < AllSections.size(); ++j) {
      const CriticalSection &CS2 = *AllSections[j].first;
      mhp::LockID Lock2 = AllSections[j].second;
      if (!Lock2)
        continue;

      if (Lock1->stripPointerCasts() == Lock2->stripPointerCasts() ||
          (m_locksetAnalysis && m_locksetAnalysis->locksMustMatch(Lock1, Lock2)))
        continue;

      if (CS1.Acquire->getFunction() == CS2.Acquire->getFunction()) {
        const DominatorTree &DT = getDomTree(CS1.Acquire->getFunction());
        if (DT.dominates(CS1.Acquire, CS2.Acquire) ||
            DT.dominates(CS2.Acquire, CS1.Acquire))
          continue;
      }

      if (!m_mhpAnalysis->mayHappenInParallel(CS1.Acquire, CS2.Acquire))
        continue;
      if (m_happensBeforeAnalysis &&
          (m_happensBeforeAnalysis->happensBefore(CS1.Acquire, CS2.Acquire) ||
           m_happensBeforeAnalysis->happensBefore(CS2.Acquire, CS1.Acquire)))
        continue;

      for (const Instruction *I1 : CS1.Body) {
        const Value *Loc1 = getMemoryLocation(I1);
        if (isDefinitelyThreadLocal(I1, Loc1))
          continue;
        for (const Instruction *I2 : CS2.Body) {
          if (!(isWrite(*I1) || isWrite(*I2)))
            continue;
          const Value *Loc2 = getMemoryLocation(I2);
          if (isDefinitelyThreadLocal(I2, Loc2))
            continue;
          if (m_locksetAnalysis &&
              m_locksetAnalysis->mustMutuallyExclude(
                  I1,
                  isWrite(*I1) ? mhp::MemoryAccessKind::Write
                               : mhp::MemoryAccessKind::Read,
                  I2,
                  isWrite(*I2) ? mhp::MemoryAccessKind::Write
                               : mhp::MemoryAccessKind::Read))
            continue;
          if (!locationsMayOverlap(I1, Loc1, I2, Loc2))
            continue;

          std::string Desc =
              "Potential atomicity violation between disjoint critical "
              "sections at " +
              formatLoc(*I1) + " and " + formatLoc(*I2);

          ConcurrencyBugReport report(ConcurrencyBugType::ATOMICITY_VIOLATION,
                                      Desc, BugDescription::BI_MEDIUM,
                                      BugDescription::BC_WARNING);
          report.addStep(I1, "Access 1 in Critical Section 1");
          report.addStep(I2, "Access 2 in Critical Section 2");
          Reports.push_back(std::move(report));
        }
      }
    }
  }

  // Phase 2: 3-access unserializable interleaving detection (R-W-R, R-W-W,
  // W-W-R, W-R-W, W-W-W).
  struct AccessRecord {
    const Instruction *Inst = nullptr;
    const Value *Loc = nullptr;
    const Value *Base = nullptr;
    const Function *Func = nullptr;
    bool IsWrite = false;
    bool IsAtomic = false;
    bool HasLock = false;
    mhp::ThreadID Tid = 0;
  };
  std::vector<AccessRecord> sharedAccesses;
  std::unordered_map<mhp::ThreadID, bool> threadSelfOverlaps;

  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      if (!isMemoryAccess(&I))
        continue;
      const Value *Loc = getMemoryLocation(&I);
      if (isDefinitelyThreadLocal(&I, Loc))
        continue;
      const mhp::ThreadID tid = m_mhpAnalysis->getThreadID(&I);
      if (threadSelfOverlaps.find(tid) == threadSelfOverlaps.end()) {
        threadSelfOverlaps.emplace(tid,
                                   m_mhpAnalysis->mayHappenInParallel(&I, &I));
      }
      const bool hasLock =
          m_locksetAnalysis && !m_locksetAnalysis->getMayLockSetAt(&I).empty();
      const Value *base = getUnderlyingObject(Loc);
      sharedAccesses.push_back(
          {&I, Loc, base, &F, isWrite(I), isAtomicInst(I), hasLock, tid});
    }
  }

  if (sharedAccesses.size() < 3 ||
      (threadSelfOverlaps.size() == 1 && !threadSelfOverlaps.begin()->second)) {
    return Reports;
  }

  // Bucket accesses by underlying / points-to base object with cached
  // getPointsToSet and wide-access separation (mirrors DataRaceChecker so wide
  // pointers never cause quadratic bucket explosion).
  constexpr size_t kMaxDirectBucketFanout = 64;
  DenseMap<const Value *, size_t> baseToId;
  std::vector<std::vector<size_t>> buckets;
  auto getBaseId = [&](const Value *b) -> size_t {
    auto it = baseToId.find(b);
    if (it != baseToId.end())
      return it->second;
    size_t id = buckets.size();
    baseToId.try_emplace(b, id);
    buckets.emplace_back();
    return id;
  };

  DenseMap<const Value *, std::vector<size_t>> ptsCache;
  std::vector<size_t> wideAccesses;

  for (size_t idx = 0; idx < sharedAccesses.size(); ++idx) {
    const AccessRecord &rec = sharedAccesses[idx];
    if (!m_aliasAnalysis) {
      buckets[getBaseId(nullptr)].push_back(idx);
      continue;
    }
    const size_t baseId = getBaseId(rec.Base);
    buckets[baseId].push_back(idx);

    if (rec.Loc->getType()->isPointerTy() && !isa<GlobalVariable>(rec.Base) &&
        !isa<AllocaInst>(rec.Base)) {
      auto ptsIt = ptsCache.find(rec.Loc);
      if (ptsIt == ptsCache.end()) {
        std::vector<size_t> targetIds;
        std::vector<const Value *> pts;
        if (m_aliasAnalysis->getPointsToSet(rec.Loc, pts)) {
          targetIds.reserve(pts.size());
          for (const Value *target : pts) {
            const Value *targetBase = getUnderlyingObject(target);
            if (targetBase && targetBase != rec.Base) {
              targetIds.push_back(getBaseId(targetBase));
            }
          }
          std::sort(targetIds.begin(), targetIds.end());
          targetIds.erase(std::unique(targetIds.begin(), targetIds.end()),
                          targetIds.end());
        }
        ptsIt = ptsCache.try_emplace(rec.Loc, std::move(targetIds)).first;
      }
      const auto &targetIds = ptsIt->second;
      if (targetIds.size() <= kMaxDirectBucketFanout) {
        for (size_t tid : targetIds) {
          buckets[tid].push_back(idx);
        }
      } else {
        wideAccesses.push_back(idx);
      }
    }
  }

  DenseMap<const Function *, std::unique_ptr<FunctionReachability>> reachCache;
  auto getReach = [&](const Function *F) -> FunctionReachability & {
    auto &entry = reachCache[F];
    if (!entry)
      entry = std::make_unique<FunctionReachability>(*F);
    return *entry;
  };

  auto inSameCriticalSection = [&](const Instruction *A1,
                                   const Instruction *A2) -> bool {
    auto it1 = instToCS.find(A1);
    auto it2 = instToCS.find(A2);
    if (it1 == instToCS.end() || it2 == instToCS.end())
      return false;
    for (const CriticalSection *cs1 : it1->second) {
      for (const CriticalSection *cs2 : it2->second) {
        if (cs1 == cs2)
          return true;
      }
    }
    return false;
  };

  auto continuouslyProtectedByOneLock = [&](const Instruction *A1,
                                            const Instruction *A2,
                                            FunctionReachability &reach) -> bool {
    if (!m_locksetAnalysis)
      return false;
    if (inSameCriticalSection(A1, A2))
      return true;
    LockSet must1 = m_locksetAnalysis->getMustLockSetAt(A1);
    if (must1.empty())
      return false;
    LockSet must2 = m_locksetAnalysis->getMustLockSetAt(A2);
    if (must2.empty())
      return false;

    for (LockID l1 : must1) {
      bool heldAt2 = false;
      for (LockID l2 : must2) {
        if (m_locksetAnalysis->locksMustMatch(l1, l2)) {
          heldAt2 = true;
          break;
        }
      }
      if (!heldAt2)
        continue;
      if (!hasUnprotectedGapBetween(A1, A2, l1, m_locksetAnalysis, reach))
        return true;
    }
    return false;
  };

  auto isUnserializableTriple = [](bool w1, bool wk, bool w2) -> const char * {
    if (!w1 && wk && !w2)
      return "R-W-R (non-repeatable read)";
    if (!w1 && wk && w2)
      return "R-W-W (check-then-act / lost update)";
    if (w1 && wk && !w2)
      return "W-W-R (overwritten before read)";
    if (w1 && !wk && w2)
      return "W-R-W (intermediate write exposed to concurrent read)";
    if (w1 && wk && w2)
      return "W-W-W (interleaved compound write)";
    return nullptr;
  };

  DenseSet<const Instruction *> reportedFirst;
  std::set<std::tuple<const Function *, const Value *, const char *>>
      reportedFuncBasePattern;

  for (auto &bucket : buckets) {
    if (bucket.size() + wideAccesses.size() < 3 || bucket.empty())
      continue;
    std::sort(bucket.begin(), bucket.end());
    bucket.erase(std::unique(bucket.begin(), bucket.end()), bucket.end());

    bool hasWrite = false;
    bool hasMultiThreadOrSelfOverlap = false;
    const mhp::ThreadID firstTid = sharedAccesses[bucket[0]].Tid;
    if (threadSelfOverlaps[firstTid])
      hasMultiThreadOrSelfOverlap = true;

    DenseMap<const Function *, SmallVector<size_t, 4>> perFunc;
    SmallVector<size_t, 8> writeInterleavers;
    SmallVector<size_t, 8> readInterleavers;

    auto registerCandidate = [&](size_t idx, bool addToPerFunc) {
      const AccessRecord &rec = sharedAccesses[idx];
      if (rec.IsWrite) {
        hasWrite = true;
        writeInterleavers.push_back(idx);
      } else {
        readInterleavers.push_back(idx);
      }
      if (!hasMultiThreadOrSelfOverlap &&
          (rec.Tid != firstTid || threadSelfOverlaps[rec.Tid])) {
        hasMultiThreadOrSelfOverlap = true;
      }
      if (addToPerFunc) {
        perFunc[rec.Func].push_back(idx);
      }
    };

    for (size_t idx : bucket)
      registerCandidate(idx, true);
    for (size_t idx : wideAccesses)
      registerCandidate(idx, false);

    if (!hasWrite || !hasMultiThreadOrSelfOverlap)
      continue;

    for (const auto &funcEntry : perFunc) {
      const Function *F1 = funcEntry.first;
      const auto &funcAccesses = funcEntry.second;
      if (funcAccesses.size() < 2)
        continue;

      // Skip functions that have zero candidate accesses with locks, atomics,
      // or a potential read-before-write/read dependence.
      FunctionReachability &reach = getReach(F1);
      const PostDominatorTree &PDT = getPostDomTree(F1);

      for (size_t a = 0; a < funcAccesses.size(); ++a) {
        const AccessRecord &A1 = sharedAccesses[funcAccesses[a]];
        if (reportedFirst.count(A1.Inst))
          continue;

        for (size_t b = a + 1; b < funcAccesses.size(); ++b) {
          const AccessRecord &A2 = sharedAccesses[funcAccesses[b]];

          // Require an atomicity relationship between A1 and A2:
          // (1) split critical section or mixed lock protection,
          // (2) atomic operation pair (e.g., atomic load + atomic store), or
          // (3) local data/control dependence (check-then-act / RMW).
          const bool lockOrAtomicPair =
              A1.HasLock || A2.HasLock || A1.IsAtomic || A2.IsAtomic;
          if (!reach.canReach(A1.Inst, A2.Inst))
            continue;
          if (!locationsMayOverlap(A1.Inst, A1.Loc, A2.Inst, A2.Loc))
            continue;
          if (!lockOrAtomicPair &&
              !hasLocalDataOrControlDependence(A1.Inst, A2.Inst, PDT, reach))
            continue;
          if (continuouslyProtectedByOneLock(A1.Inst, A2.Inst, reach))
            continue;

          auto findWitness = [&](const SmallVector<size_t, 8> &pool)
              -> std::pair<const Instruction *, const char *> {
            for (size_t kIdx : pool) {
              const AccessRecord &AK = sharedAccesses[kIdx];
              if (AK.Inst == A1.Inst || AK.Inst == A2.Inst)
                continue;
              if (AK.Func == F1 && !threadSelfOverlaps[A1.Tid])
                continue;
              const char *pat =
                  isUnserializableTriple(A1.IsWrite, AK.IsWrite, A2.IsWrite);
              if (!pat)
                continue;
              if (!locationsMayOverlap(A1.Inst, A1.Loc, AK.Inst, AK.Loc))
                continue;
              if (!(m_mhpAnalysis->mayHappenInParallel(A1.Inst, AK.Inst) ||
                    m_mhpAnalysis->mayHappenInParallel(A2.Inst, AK.Inst)))
                continue;
              if (m_happensBeforeAnalysis &&
                  (m_happensBeforeAnalysis->happensBefore(AK.Inst, A1.Inst) ||
                   m_happensBeforeAnalysis->happensBefore(A2.Inst, AK.Inst)))
                continue;
              return {AK.Inst, pat};
            }
            return {nullptr, nullptr};
          };

          // Try write interleavers first (cover R-W-R, R-W-W, W-W-R, W-W-W),
          // and read interleavers only when both A1 and A2 are writes (W-R-W).
          auto [witnessInterleaver, patternName] =
              findWitness(writeInterleavers);
          if (!witnessInterleaver && A1.IsWrite && A2.IsWrite) {
            std::tie(witnessInterleaver, patternName) =
                findWitness(readInterleavers);
          }
          if (!witnessInterleaver)
            continue;

          reportedFirst.insert(A1.Inst);
          if (!reportedFuncBasePattern
                   .emplace(F1, A1.Base, patternName)
                   .second) {
            break;
          }

          std::string Desc = std::string("Potential atomicity violation [") +
                             patternName + "] between " + formatLoc(*A1.Inst) +
                             " and " + formatLoc(*A2.Inst) +
                             " interleaved by " +
                             formatLoc(*witnessInterleaver);
          ConcurrencyBugReport report(ConcurrencyBugType::ATOMICITY_VIOLATION,
                                      Desc, BugDescription::BI_MEDIUM,
                                      BugDescription::BC_WARNING);
          report.addStep(A1.Inst, "First access in atomic region candidate");
          report.addStep(witnessInterleaver,
                         "Interleaving concurrent access from another thread");
          report.addStep(A2.Inst, "Second access in atomic region candidate");
          Reports.push_back(std::move(report));
          break;
        }
      }
    }
  }

  return Reports;
}

} // namespace concurrency
