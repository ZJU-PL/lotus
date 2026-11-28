/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/DataRaceChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/API/CppAtomics.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/MHP/MHPAnalysis.h"
#include "Concurrency/OpenMP/OpenMPSemantics.h"
#include "Concurrency/ValueFlow/WholeProgramSparseRefinement.h"

#include <algorithm>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

namespace {

struct AccessOperand {
  const Value *ptr = nullptr;
  bool isWrite = false;
  uint64_t sizeBytes = 0; // 0 = unknown
};

bool isPrivateLike(OpenMP::DataSharingAttribute attribute) {
  switch (attribute) {
  case OpenMP::DataSharingAttribute::Private:
  case OpenMP::DataSharingAttribute::Firstprivate:
  case OpenMP::DataSharingAttribute::Lastprivate:
  case OpenMP::DataSharingAttribute::Linear:
    return true;
  default:
    return false;
  }
}

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

uint64_t getTypeSizeOrZero(const Type *ty, const DataLayout &DL) {
  if (!ty || !ty->isSized()) {
    return 0;
  }
  return DL.getTypeStoreSize(const_cast<Type *>(ty));
}

SmallVector<AccessOperand, 2> getAccessOperands(const Instruction *inst,
                                                const DataLayout &DL) {
  SmallVector<AccessOperand, 2> ops;
  if (!inst) {
    return ops;
  }
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    ops.push_back(
        {load->getPointerOperand(), false, getTypeSizeOrZero(load->getType(), DL)});
  } else if (const auto *store = dyn_cast<StoreInst>(inst)) {
    ops.push_back({store->getPointerOperand(), true,
                   getTypeSizeOrZero(store->getValueOperand()->getType(), DL)});
  } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    ops.push_back({rmw->getPointerOperand(), true,
                   getTypeSizeOrZero(rmw->getValOperand()->getType(), DL)});
  } else if (const auto *cas = dyn_cast<AtomicCmpXchgInst>(inst)) {
    ops.push_back({cas->getPointerOperand(), true,
                   getTypeSizeOrZero(cas->getNewValOperand()->getType(), DL)});
  } else if (const auto *mset = dyn_cast<MemSetInst>(inst)) {
    uint64_t len = 0;
    if (const auto *ci = dyn_cast_or_null<ConstantInt>(mset->getLength())) {
      len = ci->getZExtValue();
    }
    ops.push_back({mset->getRawDest(), true, len});
  } else if (const auto *mtrans = dyn_cast<MemTransferInst>(inst)) {
    uint64_t len = 0;
    if (const auto *ci = dyn_cast_or_null<ConstantInt>(mtrans->getLength())) {
      len = ci->getZExtValue();
    }
    ops.push_back({mtrans->getRawDest(), true, len});
    ops.push_back({mtrans->getRawSource(), false, len});
  }
  return ops;
}

const Value *resolveRegionKey(const Value *value, const DataLayout &DL,
                              int64_t &offset, bool &has_precise_offset) {
  offset = 0;
  has_precise_offset = false;
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(value);
  const Value *resolved = nullptr;
  int64_t resolved_offset = 0;
  bool resolved_precise = false;

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = stripValue(current);
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    int64_t current_offset = 0;
    bool current_precise = false;
    const Value *base = nullptr;
    if (current->getType()->isPointerTy()) {
      if (const Value *base_with_offset =
              GetPointerBaseWithConstantOffset(current, current_offset, DL)) {
        base = stripValue(base_with_offset);
        if (!(isa<GEPOperator>(current) && base == stripValue(current))) {
          current_precise = true;
        }
      }
    }
    if (!base && current->getType()->isPointerTy()) {
      base = stripValue(getUnderlyingObject(current));
    }
    if (!base) {
      base = current;
    }

    if (!resolved) {
      resolved = base;
      resolved_offset = current_offset;
      resolved_precise = current_precise;
    } else if (resolved != base) {
      has_precise_offset = false;
      offset = 0;
      return nullptr;
    } else if (resolved_precise && current_precise &&
               resolved_offset != current_offset) {
      resolved_precise = false;
    }
  }

  offset = resolved_precise ? resolved_offset : 0;
  has_precise_offset = resolved_precise;
  return resolved;
}

bool isIgnorableStructType(const StructType *st) {
  if (!st || !st->hasName()) {
    return false;
  }
  StringRef name = st->getName();
  if (name.startswith("\01")) {
    name = name.drop_front(1);
  }
  // Strip common LLVM IR struct prefixes like "struct." / "class." / "union."
  if (name.startswith("struct.")) {
    name = name.drop_front(7);
  } else if (name.startswith("class.")) {
    name = name.drop_front(6);
  } else if (name.startswith("union.")) {
    name = name.drop_front(6);
  }
  // Strip std:: prefix / numeric suffixes (.0, .1)
  if (name.startswith("std::")) {
    name = name.drop_front(5);
  }
  if (size_t dot = name.find('.'); dot != StringRef::npos) {
    StringRef suffix = name.substr(dot + 1);
    bool allDigits = !suffix.empty() &&
                     std::all_of(suffix.begin(), suffix.end(),
                                 [](char c) { return c >= '0' && c <= '9'; });
    if (allDigits) {
      name = name.substr(0, dot);
    }
  }

  static const char *ignorable[] = {
      // pthread primitives
      "pthread_mutex_t",
      "pthread_cond_t",
      "pthread_barrier_t",
      "pthread_rwlock_t",
      "pthread_spinlock_t",
      "pthread_once_t",
      "__pthread_mutex_s",
      "__pthread_cond_s",
      "__pthread_rwlock_arch_t",
      "pthread_condattr_t",
      "pthread_mutexattr_t",
      "pthread_barrierattr_t",
      "_pthread_cleanup_buffer",
      "__cancel_jmp_buf_tag",
      // C++ standard library synchronization primitives
      "mutex",
      "recursive_mutex",
      "shared_mutex",
      "shared_timed_mutex",
      "timed_mutex",
      "lock_guard",
      "unique_lock",
      "scoped_lock",
      "shared_lock",
      "condition_variable",
      "condition_variable_any",
      "once_flag",
      "promise",
      "future",
      "shared_future",
      "latch",
      "barrier",
      "counting_semaphore",
      "binary_semaphore",
      "jthread",
      // Atomic and low-level primitives
      "atomic_flag",
      "atomic_t",
      "atomic",
      "spinlock_t",
      "lock_class_key",
      // File I/O & setjmp buffers
      "FILE",
      "__FILE",
      "_IO_FILE",
      "__jmp_buf_tag",
  };
  for (const char *ig : ignorable) {
    if (name.equals(ig)) {
      return true;
    }
  }
  return false;
}

// Opaque-pointer-friendly pointee type inspection: inspect allocation, global,
// GEP, or access value type first, falling back to typed pointer element type.
bool isIgnorableAccessForRace(const Instruction *inst, const Value *loc) {
  if (!loc) {
    return false;
  }
  auto checkType = [](const Type *ty) -> bool {
    if (const auto *st = dyn_cast_or_null<StructType>(ty)) {
      return isIgnorableStructType(st);
    }
    return false;
  };

  if (const auto *load = dyn_cast_or_null<LoadInst>(inst)) {
    if (checkType(load->getType())) {
      return true;
    }
  } else if (const auto *store = dyn_cast_or_null<StoreInst>(inst)) {
    if (checkType(store->getValueOperand()->getType())) {
      return true;
    }
  }

  const Value *stripped = loc->stripPointerCasts();
  if (const auto *gep = dyn_cast<GEPOperator>(stripped)) {
    if (checkType(gep->getResultElementType()) ||
        checkType(gep->getSourceElementType())) {
      return true;
    }
  }
  const Value *base = getUnderlyingObject(stripped);
  if (const auto *gv = dyn_cast_or_null<GlobalVariable>(base)) {
    if (checkType(gv->getValueType())) {
      return true;
    }
  } else if (const auto *alloca = dyn_cast_or_null<AllocaInst>(base)) {
    if (checkType(alloca->getAllocatedType())) {
      return true;
    }
  }

  if (const auto *pty = dyn_cast<PointerType>(loc->getType())) {
    if (!pty->isOpaque() && checkType(pty->getPointerElementType())) {
      return true;
    }
  }
  return false;
}

bool areDisjointConstantOffsetOperands(const AccessOperand &op1,
                                       const AccessOperand &op2,
                                       const DataLayout &DL) {
  if (!op1.ptr || !op2.ptr) {
    return false;
  }
  const Value *p1 = stripValue(op1.ptr);
  const Value *p2 = stripValue(op2.ptr);
  if (!p1->getType()->isPointerTy() || !p2->getType()->isPointerTy()) {
    return false;
  }
  int64_t off1 = 0;
  int64_t off2 = 0;
  const Value *b1 = GetPointerBaseWithConstantOffset(p1, off1, DL);
  const Value *b2 = GetPointerBaseWithConstantOffset(p2, off2, DL);
  if (!b1 || !b2) {
    return false;
  }
  b1 = stripValue(b1);
  b2 = stripValue(b2);

  // Distinct concrete global variables or stack allocations never overlap.
  auto isDistinctObject = [](const Value *v) {
    return isa<GlobalVariable>(v) || isa<AllocaInst>(v);
  };
  if (b1 != b2) {
    return isDistinctObject(b1) && isDistinctObject(b2);
  }

  // Same base object: disjoint if byte intervals [off, off + size) do not
  // intersect.
  if (op1.sizeBytes == 0 || op2.sizeBytes == 0) {
    return false;
  }
  const int64_t end1 = off1 + static_cast<int64_t>(op1.sizeBytes);
  const int64_t end2 = off2 + static_cast<int64_t>(op2.sizeBytes);
  return end1 <= off2 || end2 <= off1;
}

} // namespace

DataRaceChecker::DataRaceChecker(
    Module &module, IMHPAnalysis *mhpAnalysis, LockSetAnalysis *locksetAnalysis,
    EscapeAnalysis *escapeAnalysis,
    ThreadLocal::ThreadLocalAnalysis *threadLocalAnalysis,
    lotus::StaticThreadSharingAnalysis *staticThreadSharingAnalysis,
    AliasAnalysisWrapper *aliasAnalysis,
    HappensBeforeAnalysis *happensBeforeAnalysis)
    : m_module(module), m_mhpAnalysis(mhpAnalysis),
      m_locksetAnalysis(locksetAnalysis), m_escapeAnalysis(escapeAnalysis),
      m_threadLocalAnalysis(threadLocalAnalysis),
      m_staticThreadSharingAnalysis(staticThreadSharingAnalysis),
      m_aliasAnalysis(aliasAnalysis),
      m_happensBeforeAnalysis(happensBeforeAnalysis),
      m_threadAPI(ThreadAPI::getThreadAPI()) {}

bool DataRaceChecker::areIndependent(const Instruction *inst1,
                                     const Instruction *inst2) const {
  return !mayAccessSameLocation(inst1, inst2);
}

bool DataRaceChecker::wouldReportDataRaceCore(const Instruction *inst1,
                                              const Instruction *inst2) const {
  if (!isWriteAccess(inst1) && !isWriteAccess(inst2))
    return false;
  if (isAtomicOperation(inst1) && isAtomicOperation(inst2))
    return false;
  if (!m_mhpAnalysis->mayHappenInParallel(inst1, inst2))
    return false;
  if (areIndependent(inst1, inst2))
    return false;

  if (m_locksetAnalysis) {
    const auto access_kind = [this](const Instruction *inst) {
      return isWriteAccess(inst) ? mhp::MemoryAccessKind::Write
                                 : mhp::MemoryAccessKind::Read;
    };
    if (m_locksetAnalysis->mustMutuallyExclude(inst1, access_kind(inst1), inst2,
                                               access_kind(inst2))) {
      return false;
    }
  }
  if (m_happensBeforeAnalysis &&
      (m_happensBeforeAnalysis->happensBefore(inst1, inst2) ||
       m_happensBeforeAnalysis->happensBefore(inst2, inst1)))
    return false;
  return true;
}

bool DataRaceChecker::wouldReportDataRace(const Instruction *inst1,
                                          const Instruction *inst2) const {
  auto isDefinitelyThreadLocalAccess = [this](const Instruction *inst) {
    if (!inst) {
      return false;
    }
    if (m_threadLocalAnalysis &&
        m_threadLocalAnalysis->accessesThreadLocalStorage(inst)) {
      return true;
    }
    if (m_staticThreadSharingAnalysis &&
        m_staticThreadSharingAnalysis->classify(inst) ==
            lotus::StaticThreadSharingAnalysis::SharingClassification::
                DefinitelyThreadLocal) {
      return true;
    }
    return false;
  };

  if (!isWriteAccess(inst1) && !isWriteAccess(inst2))
    return false;
  if (isAtomicOperation(inst1) && isAtomicOperation(inst2))
    return false;
  if (isDefinitelyThreadLocalAccess(inst1) ||
      isDefinitelyThreadLocalAccess(inst2)) {
    return false;
  }
  return wouldReportDataRaceCore(inst1, inst2);
}

std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
  if (!m_mhpAnalysis || m_mhpAnalysis->isSingleThreaded()) {
    return {};
  }
  buildSyncObjectSet();
  std::vector<const Instruction *> accesses;
  collectVariableAccesses(accesses);
  if (accesses.empty()) {
    return {};
  }

  const DataLayout &DL = m_module.getDataLayout();
  const size_t numAccesses = accesses.size();
  std::vector<bool> accessIsWrite(numAccesses, false);
  std::vector<ThreadID> accessThread(numAccesses, 0);
  std::unordered_map<ThreadID, bool> threadSelfOverlaps;
  for (size_t idx = 0; idx < numAccesses; ++idx) {
    const Instruction *I = accesses[idx];
    accessIsWrite[idx] = isWriteAccess(I);
    const ThreadID tid = m_mhpAnalysis->getThreadID(I);
    accessThread[idx] = tid;
    if (threadSelfOverlaps.find(tid) == threadSelfOverlaps.end()) {
      threadSelfOverlaps.emplace(tid, m_mhpAnalysis->mayHappenInParallel(I, I));
    }
  }

  if (threadSelfOverlaps.size() == 1 && !threadSelfOverlaps.begin()->second) {
    return {};
  }

  DenseMap<const Value *, const Value *> underlyingCache;
  underlyingCache.reserve(numAccesses);
  auto getUnderlyingCached = [&](const Value *v) -> const Value * {
    if (!v)
      return nullptr;
    auto it = underlyingCache.find(v);
    if (it != underlyingCache.end())
      return it->second;
    const Value *base = llvm::getUnderlyingObject(v);
    underlyingCache.try_emplace(v, base);
    return base;
  };

  DenseMap<const Value *, size_t> baseToId;
  baseToId.reserve(numAccesses / 2 + 16);
  std::vector<std::vector<size_t>> byBase;
  auto getBaseId = [&](const Value *base) -> size_t {
    auto it = baseToId.find(base);
    if (it != baseToId.end())
      return it->second;
    size_t id = byBase.size();
    baseToId.try_emplace(base, id);
    byBase.emplace_back();
    return id;
  };

  constexpr size_t kMaxDirectBucketFanout = 64;
  std::vector<std::vector<size_t>> ptsStorage;
  DenseMap<const Value *, size_t> ptsLocToStorageIdx;
  std::vector<size_t> wideAccesses;
  std::vector<size_t> wideAccessPtsIdx;
  std::vector<uint32_t> baseSeenStamp;
  uint32_t curStamp = 0;

  for (size_t idx = 0; idx < numAccesses; ++idx) {
    const Instruction *I = accesses[idx];
    const auto operands = getAccessOperands(I, DL);
    if (operands.empty()) {
      continue;
    }

    ++curStamp;
    if (curStamp == 0) {
      std::fill(baseSeenStamp.begin(), baseSeenStamp.end(), 0);
      curStamp = 1;
    }

    for (const AccessOperand &op : operands) {
      const Value *loc = op.ptr;
      if (!loc)
        continue;
      const Value *base = getUnderlyingCached(loc);
      if (!base)
        continue;
      const size_t baseId = getBaseId(base);

      if (baseSeenStamp.size() <= baseId)
        baseSeenStamp.resize(baseId + 1, 0);
      if (baseSeenStamp[baseId] != curStamp) {
        baseSeenStamp[baseId] = curStamp;
        byBase[baseId].push_back(idx);
      }

      if (m_aliasAnalysis && base->getType()->isPointerTy() &&
          !isa<GlobalVariable>(base) && !isa<AllocaInst>(base)) {
        auto cacheIt = ptsLocToStorageIdx.find(base);
        if (cacheIt == ptsLocToStorageIdx.end()) {
          std::vector<const Value *> pts;
          std::vector<size_t> targetIds;
          DenseSet<size_t> seenTargetIds;
          if (m_aliasAnalysis->getPointsToSet(base, pts) && !pts.empty()) {
            targetIds.reserve(std::min<size_t>(pts.size(), 256));
            for (const Value *target : pts) {
              const Value *targetBase = getUnderlyingCached(target);
              if (!targetBase)
                continue;
              const size_t tid = getBaseId(targetBase);
              if (seenTargetIds.insert(tid).second) {
                targetIds.push_back(tid);
              }
            }
          }
          size_t storageIdx = ptsStorage.size();
          ptsStorage.push_back(std::move(targetIds));
          cacheIt = ptsLocToStorageIdx.try_emplace(base, storageIdx).first;
        }

        const size_t storageIdx = cacheIt->second;
        const auto &targetIds = ptsStorage[storageIdx];
        if (targetIds.size() > kMaxDirectBucketFanout) {
          wideAccesses.push_back(idx);
          wideAccessPtsIdx.push_back(storageIdx);
        } else {
          for (size_t targetBaseId : targetIds) {
            if (baseSeenStamp.size() <= targetBaseId)
              baseSeenStamp.resize(targetBaseId + 1, 0);
            if (baseSeenStamp[targetBaseId] != curStamp) {
              baseSeenStamp[targetBaseId] = curStamp;
              byBase[targetBaseId].push_back(idx);
            }
          }
        }
      }
    }
  }

  // Incremental union-find over dense access indices, recording an actual
  // witnessed conflicting pair per component so reported representatives are
  // guaranteed to conflict.
  std::vector<int> ufParent(numAccesses, -1);
  std::vector<std::pair<int, int>> compWitness(numAccesses, {-1, -1});
  auto findRoot = [&ufParent](int x) -> int {
    int root = x;
    while (ufParent[root] != root) {
      root = ufParent[root];
    }
    while (ufParent[x] != root) {
      int next = ufParent[x];
      ufParent[x] = root;
      x = next;
    }
    return root;
  };
  auto inSameComponent = [&](size_t a, size_t b) -> bool {
    int pa = ufParent[a];
    if (pa < 0)
      return false;
    int pb = ufParent[b];
    if (pb < 0)
      return false;
    return findRoot(static_cast<int>(a)) == findRoot(static_cast<int>(b));
  };
  auto unite = [&](size_t a, size_t b) {
    if (ufParent[a] < 0)
      ufParent[a] = static_cast<int>(a);
    if (ufParent[b] < 0)
      ufParent[b] = static_cast<int>(b);
    int ra = findRoot(static_cast<int>(a));
    int rb = findRoot(static_cast<int>(b));
    if (ra != rb) {
      ufParent[ra] = rb;
      if (compWitness[rb].first < 0) {
        if (compWitness[ra].first >= 0) {
          compWitness[rb] = compWitness[ra];
        } else {
          compWitness[rb] = accessIsWrite[a]
                                ? std::make_pair(static_cast<int>(a),
                                                 static_cast<int>(b))
                                : std::make_pair(static_cast<int>(b),
                                                 static_cast<int>(a));
        }
      }
    } else if (compWitness[rb].first < 0) {
      compWitness[rb] = accessIsWrite[a]
                            ? std::make_pair(static_cast<int>(a),
                                             static_cast<int>(b))
                            : std::make_pair(static_cast<int>(b),
                                             static_cast<int>(a));
    }
  };

  auto checkPairAndUnite = [&](size_t a, size_t b) {
    if (inSameComponent(a, b))
      return;
    if (wouldReportDataRaceCore(accesses[a], accesses[b])) {
      unite(a, b);
    }
  };

  auto checkBucket = [&](std::vector<size_t> &vec) {
    if (vec.size() < 2)
      return;
    std::sort(vec.begin(), vec.end());
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    if (vec.size() < 2)
      return;

    bool hasWrite = false;
    ThreadID firstTid = accessThread[vec[0]];
    bool allSameTid = true;
    for (size_t idx : vec) {
      if (accessIsWrite[idx])
        hasWrite = true;
      if (accessThread[idx] != firstTid)
        allSameTid = false;
    }
    if (!hasWrite)
      return;
    if (allSameTid && !threadSelfOverlaps[firstTid])
      return;

    struct ThreadGroup {
      ThreadID tid;
      bool selfOverlaps = false;
      std::vector<size_t> writes;
      std::vector<size_t> reads;
    };
    std::vector<ThreadGroup> groups;
    std::unordered_map<ThreadID, size_t> groupIdx;
    for (size_t idx : vec) {
      ThreadID tid = accessThread[idx];
      auto it = groupIdx.find(tid);
      if (it == groupIdx.end()) {
        size_t pos = groups.size();
        groupIdx.emplace(tid, pos);
        groups.push_back({tid, threadSelfOverlaps[tid], {}, {}});
        it = groupIdx.find(tid);
      }
      if (accessIsWrite[idx])
        groups[it->second].writes.push_back(idx);
      else
        groups[it->second].reads.push_back(idx);
    }

    auto allInOneComponent = [&]() -> bool {
      int r0 = ufParent[vec[0]];
      if (r0 < 0)
        return false;
      r0 = findRoot(static_cast<int>(vec[0]));
      for (size_t k = 1; k < vec.size(); ++k) {
        if (ufParent[vec[k]] < 0 || findRoot(static_cast<int>(vec[k])) != r0)
          return false;
      }
      return true;
    };

    auto compareSets = [&](const std::vector<size_t> &lhs,
                           const std::vector<size_t> &rhs, bool symmetric) {
      if (lhs.empty() || rhs.empty())
        return;
      for (size_t i = 0; i < lhs.size(); ++i) {
        const size_t a = lhs[i];
        const size_t jStart = symmetric ? (i + 1) : 0;
        for (size_t j = jStart; j < rhs.size(); ++j) {
          checkPairAndUnite(a, rhs[j]);
        }
        if (i == 0 && (lhs.size() + rhs.size() > 32) && allInOneComponent()) {
          return;
        }
      }
    };

    for (const ThreadGroup &g : groups) {
      if (!g.selfOverlaps || g.writes.empty())
        continue;
      compareSets(g.writes, g.writes, true);
      if (vec.size() > 32 && allInOneComponent())
        break;
      compareSets(g.writes, g.reads, false);
      if (vec.size() > 32 && allInOneComponent())
        break;
    }

    if (vec.size() > 32 && allInOneComponent())
      return;

    for (size_t gi = 0; gi < groups.size(); ++gi) {
      for (size_t gj = gi + 1; gj < groups.size(); ++gj) {
        const ThreadGroup &g1 = groups[gi];
        const ThreadGroup &g2 = groups[gj];
        if (!g1.writes.empty()) {
          compareSets(g1.writes, g2.writes, false);
          compareSets(g1.writes, g2.reads, false);
        }
        if (!g2.writes.empty()) {
          compareSets(g2.writes, g1.reads, false);
        }
        if (vec.size() > 32 && allInOneComponent())
          break;
      }
      if (vec.size() > 32 && allInOneComponent())
        break;
    }
  };

  for (auto &vec : byBase) {
    checkBucket(vec);
  }

  if (!wideAccesses.empty()) {
    std::vector<size_t> wideBucket = wideAccesses;
    checkBucket(wideBucket);

    struct WideRepKey {
      int compRoot;
      ThreadID tid;
      bool isWrite;
      size_t ptsIdx;
      bool operator==(const WideRepKey &o) const {
        return compRoot == o.compRoot && tid == o.tid &&
               isWrite == o.isWrite && ptsIdx == o.ptsIdx;
      }
    };
    struct WideRepKeyHash {
      size_t operator()(const WideRepKey &k) const {
        size_t h = std::hash<int>{}(k.compRoot);
        h ^= std::hash<ThreadID>{}(k.tid) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<size_t>{}(k.ptsIdx) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h ^ (k.isWrite ? 1u : 0u);
      }
    };
    std::unordered_set<WideRepKey, WideRepKeyHash> seenWideReps;
    seenWideReps.reserve(wideAccesses.size());

    for (size_t wi = 0; wi < wideAccesses.size(); ++wi) {
      const size_t wIdx = wideAccesses[wi];
      const bool wIsWrite = accessIsWrite[wIdx];
      const ThreadID wTid = accessThread[wIdx];
      const size_t wPtsIdx = wideAccessPtsIdx[wi];
      const int compRoot =
          ufParent[wIdx] >= 0 ? findRoot(static_cast<int>(wIdx)) : -1;
      const bool hasMustLocks =
          m_locksetAnalysis &&
          !m_locksetAnalysis->getMustLockSetAt(accesses[wIdx]).empty();
      if (compRoot >= 0 && !hasMustLocks &&
          !seenWideReps.insert({compRoot, wTid, wIsWrite, wPtsIdx}).second) {
        continue;
      }
      const bool wSelfOverlap = threadSelfOverlaps[wTid];
      for (size_t targetBaseId : ptsStorage[wPtsIdx]) {
        if (targetBaseId >= byBase.size())
          continue;
        const auto &bucket = byBase[targetBaseId];
        if (bucket.empty())
          continue;
        for (size_t otherIdx : bucket) {
          if (otherIdx == wIdx)
            continue;
          if (!wIsWrite && !accessIsWrite[otherIdx])
            continue;
          if (accessThread[otherIdx] == wTid && !wSelfOverlap)
            continue;
          if (inSameComponent(wIdx, otherIdx))
            continue;
          if (wouldReportDataRaceCore(accesses[wIdx], accesses[otherIdx])) {
            unite(wIdx, otherIdx);
          }
        }
      }
    }
  }

  std::unordered_map<int, std::vector<size_t>> components;
  for (size_t idx = 0; idx < numAccesses; ++idx) {
    if (ufParent[idx] < 0)
      continue;
    int root = findRoot(static_cast<int>(idx));
    components[root].push_back(idx);
  }

  std::vector<ConcurrencyBugReport> reports;
  for (auto &kv : components) {
    const int root = kv.first;
    std::vector<size_t> &comp = kv.second;
    if (comp.empty())
      continue;

    // Pick the exact witnessed conflicting pair for this component.
    size_t repIdx1 = comp[0];
    size_t repIdx2 = comp.size() > 1 ? comp[1] : comp[0];
    if (compWitness[root].first >= 0 && compWitness[root].second >= 0) {
      repIdx1 = static_cast<size_t>(compWitness[root].first);
      repIdx2 = static_cast<size_t>(compWitness[root].second);
    }

    const Instruction *rep1 = accesses[repIdx1];
    const Instruction *rep2 = accesses[repIdx2];
    std::string desc = "Potential data race between " +
                       getInstructionLocation(rep1) + " and " +
                       getInstructionLocation(rep2);
    if (comp.size() > 2)
      desc += " (" + std::to_string(comp.size()) + " conflicting accesses)";

    ConcurrencyBugReport report(ConcurrencyBugType::DATA_RACE, desc,
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    const std::string path1 = getAccessPath(rep1);
    const std::string path2 = getAccessPath(rep2);
    report.setDataRaceInfo(path1, path2, isWriteAccess(rep1),
                           isWriteAccess(rep2), path1 + " / " + path2);

    auto formatStep = [&](size_t idx) -> std::string {
      std::string s = accessIsWrite[idx] ? "Write" : "Read";
      const ThreadID tid = accessThread[idx];
      if (tid != std::numeric_limits<ThreadID>::max()) {
        s += " (thread " + std::to_string(tid) + ")";
      }
      return s;
    };

    report.addStep(rep1, formatStep(repIdx1));
    if (repIdx2 != repIdx1) {
      report.addStep(rep2, formatStep(repIdx2));
    }
    for (size_t idx : comp) {
      if (idx == repIdx1 || idx == repIdx2)
        continue;
      report.addStep(accesses[idx], formatStep(idx));
    }
    reports.push_back(std::move(report));
  }
  return reports;
}

void DataRaceChecker::collectVariableAccesses(
    std::vector<const Instruction *> &accesses) {
  const DataLayout &DL = m_module.getDataLayout();
  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      if (!isMemoryAccess(inst))
        continue;

      const auto operands = getAccessOperands(inst, DL);
      if (operands.empty())
        continue;

      if (m_threadLocalAnalysis &&
          m_threadLocalAnalysis->accessesThreadLocalStorage(inst)) {
        continue;
      }
      if (m_staticThreadSharingAnalysis &&
          m_staticThreadSharingAnalysis->classify(inst) ==
              lotus::StaticThreadSharingAnalysis::SharingClassification::
                  DefinitelyThreadLocal) {
        continue;
      }

      // Keep the instruction if ANY operand may access non-private, non-sync
      // shared memory.
      bool hasCandidateOperand = false;
      for (const AccessOperand &op : operands) {
        const Value *memLoc = op.ptr;
        if (!memLoc)
          continue;
        if (isOpenMPPrivateLikeAccess(inst, memLoc))
          continue;
        if (isSyncObjectAccess(memLoc))
          continue;
        if (isIgnorableAccessForRace(inst, memLoc))
          continue;
        if (m_escapeAnalysis && !m_escapeAnalysis->isEscaped(memLoc)) {
          const Value *baseObj = getUnderlyingObject(memLoc->stripPointerCasts());
          if (isa_and_nonnull<AllocaInst>(baseObj)) {
            continue;
          }
        }
        hasCandidateOperand = true;
        break;
      }
      if (hasCandidateOperand) {
        accesses.push_back(inst);
      }
    }
  }
}

bool DataRaceChecker::isOpenMPPrivateLikeAccess(const Instruction *inst,
                                                const Value *loc) const {
  if (!inst || !loc || !m_mhpAnalysis) {
    return false;
  }
  const OpenMP::OpenMPSemantics *semantics =
      m_mhpAnalysis->getOpenMPSemantics();
  if (!semantics || semantics->getTasks().empty()) {
    return false;
  }

  const Function *func = inst->getFunction();
  if (!func) {
    return false;
  }

  int64_t offset = 0;
  bool precise = false;
  const Value *base =
      resolveRegionKey(loc, m_module.getDataLayout(), offset, precise);
  if (!base) {
    base = stripValue(loc);
  }

  for (const auto &task_uptr : semantics->getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    if (!task || task->task_function != func) {
      continue;
    }
    for (const OpenMP::DataSharingEntry &entry : task->data_sharing_entries) {
      if (!isPrivateLike(entry.attribute) || !entry.canonical_base) {
        continue;
      }
      if (entry.canonical_base != base) {
        continue;
      }
      if (entry.has_precise_offset && precise && entry.offset != offset) {
        continue;
      }
      return true;
    }
  }
  return false;
}

bool DataRaceChecker::mayAccessSameLocation(const Instruction *inst1,
                                            const Instruction *inst2) const {
  const Instruction *a =
      std::less<const Instruction *>{}(inst1, inst2) ? inst1 : inst2;
  const Instruction *b = a == inst1 ? inst2 : inst1;
  auto cacheResult = [this, a, b](bool value) {
    if (a && b && m_location_overlap_cache.size() < kMaxLocationCacheEntries) {
      m_location_overlap_cache.emplace(std::make_pair(a, b), value);
    }
    return value;
  };
  if (a && b) {
    auto cache_it = m_location_overlap_cache.find({a, b});
    if (cache_it != m_location_overlap_cache.end()) {
      return cache_it->second;
    }
  }

  const DataLayout &DL = m_module.getDataLayout();
  const auto ops1 = getAccessOperands(inst1, DL);
  const auto ops2 = getAccessOperands(inst2, DL);

  auto operandsMayOverlap = [&](const AccessOperand &op1,
                                const AccessOperand &op2) -> bool {
    const Value *ptr1 = op1.ptr;
    const Value *ptr2 = op2.ptr;
    if (!ptr1 || !ptr2) {
      return false;
    }
    if (stripValue(ptr1) == stripValue(ptr2)) {
      return true;
    }
    if (areDisjointConstantOffsetOperands(op1, op2, DL)) {
      return false;
    }

    // Refine with thread-aware sparse flow-sensitive PTA when available and
    // converged. Only trust a disjoint verdict when both pointers have
    // complete, non-empty flow-sensitive points-to sets.
    if (m_sparseRefinement) {
      if (const auto *solver = m_sparseRefinement->solver()) {
        if (!solver->timedOut()) {
          const auto left = solver->pointsTo(ptr1);
          const auto right = solver->pointsTo(ptr2);
          if (left && right && !left->empty() && !right->empty()) {
            const std::optional<bool> sparseAlias = solver->mayAlias(ptr1, ptr2);
            if (sparseAlias.has_value() && !*sparseAlias) {
              return false;
            }
          }
        }
      }
    }

    if (mayAlias(ptr1, ptr2)) {
      return true;
    }
    std::vector<const Value *> pts1, pts2;
    if (m_aliasAnalysis && m_aliasAnalysis->getPointsToSet(ptr1, pts1)) {
      for (const Value *target : pts1) {
        if (target == ptr2 || m_aliasAnalysis->mayAlias(target, ptr2)) {
          return true;
        }
      }
    }
    if (m_aliasAnalysis && m_aliasAnalysis->getPointsToSet(ptr2, pts2)) {
      for (const Value *target : pts2) {
        if (target == ptr1 || m_aliasAnalysis->mayAlias(target, ptr1)) {
          return true;
        }
      }
    }
    return false;
  };

  for (const AccessOperand &op1 : ops1) {
    for (const AccessOperand &op2 : ops2) {
      if (!op1.isWrite && !op2.isWrite) {
        continue;
      }
      if (operandsMayOverlap(op1, op2)) {
        return cacheResult(true);
      }
    }
  }
  return cacheResult(false);
}

bool DataRaceChecker::mayAlias(const Value *v1, const Value *v2) const {
  if (!v1 || !v2)
    return false;
  if (v1 == v2)
    return true;
  if (m_aliasAnalysis) {
    return m_aliasAnalysis->mayAlias(v1, v2);
  }
  return true;
}

bool DataRaceChecker::isMemoryAccess(const Instruction *inst) const {
  return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
         isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst) ||
         isa<MemIntrinsic>(inst);
}

bool DataRaceChecker::isWriteAccess(const Instruction *inst) const {
  return isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) ||
         isa<AtomicCmpXchgInst>(inst) || isa<MemIntrinsic>(inst);
}

bool DataRaceChecker::isAtomicOperation(const Instruction *inst) const {
  if (CppAtomics::isAtomic(inst))
    return true;

  if (isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst))
    return true;
  if (const auto *L = dyn_cast<LoadInst>(inst))
    return L->isAtomic();
  if (const auto *S = dyn_cast<StoreInst>(inst))
    return S->isAtomic();
  if (CppAtomics::isFence(inst))
    return true;

  return false;
}

const Value *DataRaceChecker::getMemoryLocation(const Instruction *inst) const {
  if (const auto *load = dyn_cast<LoadInst>(inst))
    return load->getPointerOperand();
  if (const auto *store = dyn_cast<StoreInst>(inst))
    return store->getPointerOperand();
  if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst))
    return rmw->getPointerOperand();
  if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst))
    return cmpxchg->getPointerOperand();
  if (const auto *mi = dyn_cast<MemIntrinsic>(inst))
    return mi->getRawDest();
  return nullptr;
}

std::string
DataRaceChecker::getInstructionLocation(const Instruction *inst) const {
  std::string location;
  raw_string_ostream os(location);
  if (const Function *func = inst->getFunction())
    os << func->getName();
  if (const BasicBlock *bb = inst->getParent())
    os << ":" << bb->getName();
  return os.str();
}

void DataRaceChecker::buildSyncObjectSet() {
  m_syncObjects.clear();
  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      const CallBase *cb = dyn_cast<CallBase>(inst);
      if (!cb || !m_threadAPI->getCallee(inst))
        continue;
      const Value *v = nullptr;

      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst))
        v = m_threadAPI->getAnalysisLockIdentity(inst);
      else if (m_threadAPI->isTDCondWait(inst) ||
               m_threadAPI->isTDCondSignal(inst) ||
               m_threadAPI->isTDCondBroadcast(inst))
        v = m_threadAPI->getCondVal(inst);
      else if (m_threadAPI->isTDBarWait(inst))
        v = m_threadAPI->getBarrierVal(inst);
      else {
        ThreadAPI::TD_TYPE type = m_threadAPI->getType(cb);

        switch (type) {
        case ThreadAPI::TD_SHARED_RDLOCK:
        case ThreadAPI::TD_SHARED_WRLOCK:
        case ThreadAPI::TD_SHARED_UNLOCK:
        case ThreadAPI::TD_LOCK_GUARD_CTOR:
        case ThreadAPI::TD_LOCK_GUARD_DTOR:
        case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
        case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
        case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
        case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
        case ThreadAPI::TD_SCOPED_LOCK_CTOR:
        case ThreadAPI::TD_SCOPED_LOCK_DTOR:
        case ThreadAPI::TD_SHARED_LOCK_CTOR:
        case ThreadAPI::TD_SHARED_LOCK_DTOR:
        case ThreadAPI::TD_SEMAPHORE_ACQUIRE:
        case ThreadAPI::TD_SEMAPHORE_RELEASE:
        case ThreadAPI::TD_CALL_ONCE:
        case ThreadAPI::TD_FUTURE_GET:
        case ThreadAPI::TD_FUTURE_WAIT:
        case ThreadAPI::TD_PROMISE_SET:
        case ThreadAPI::TD_LATCH_COUNT_DOWN:
        case ThreadAPI::TD_LATCH_WAIT:
        case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
        case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
        case ThreadAPI::TD_BARRIER_ARRIVE:
        case ThreadAPI::TD_BARRIER_WAIT_CPP20:
        case ThreadAPI::TD_CUDA_BARRIER:
        case ThreadAPI::TD_CUDA_WARP_BARRIER:
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;
        default:
          break;
        }
      }

      if (v)
        m_syncObjects.insert(v->stripPointerCasts());
    }
  }
}

bool DataRaceChecker::isSyncObjectAccess(const Value *loc) const {
  if (!loc || m_syncObjects.empty())
    return false;
  Value *stripped = const_cast<Value *>(loc)->stripPointerCasts();
  if (m_syncObjects.count(stripped))
    return true;
  if (!m_aliasAnalysis)
    return false;
  const Value *locBase = getUnderlyingObject(stripped);
  const bool locDistinct =
      locBase && (isa<GlobalValue>(locBase) || isa<AllocaInst>(locBase));
  for (const Value *sync : m_syncObjects) {
    if (locDistinct) {
      const Value *syncBase = getUnderlyingObject(sync);
      if (syncBase &&
          (isa<GlobalValue>(syncBase) || isa<AllocaInst>(syncBase)) &&
          syncBase != locBase) {
        continue;
      }
    }
    if (m_aliasAnalysis->mustAlias(stripped, sync))
      return true;
  }
  return false;
}

std::string DataRaceChecker::getAccessPath(const Instruction *inst) const {
  const Value *loc = getMemoryLocation(inst);
  if (!loc)
    return getInstructionLocation(inst);
  std::string s;
  raw_string_ostream os(s);
  loc->getType()->print(os);
  if (loc->hasName()) {
    os << (isa<GlobalValue>(loc) ? " @" : " %") << loc->getName();
    return os.str();
  }
  if (const Value *base = getUnderlyingObject(loc)) {
    if (base->hasName()) {
      os << (isa<GlobalValue>(base) ? " @" : " %") << base->getName();
      if (base != loc) {
        os << "+offset";
      }
      return os.str();
    }
  }
  os << " <" << getInstructionLocation(inst) << ">";
  return os.str();
}

} // namespace concurrency
