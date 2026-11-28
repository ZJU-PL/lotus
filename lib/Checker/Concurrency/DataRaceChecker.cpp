/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/DataRaceChecker.h"
#include "Concurrency/MHP/MHPAnalysis.h"
#include "Concurrency/OpenMP/OpenMPSemantics.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/API/CppAtomics.h"

#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

namespace {

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

// Types that are never considered racy (sync objects, FILE, etc.). Borrowed
// from Goblint.
static bool isIgnorableTypeForRace(const Type *ty) {
  if (!ty || !ty->isPointerTy())
    return false;
  const Type *elem = cast<PointerType>(ty)->getPointerElementType();
  const StructType *st = dyn_cast<StructType>(elem);
  if (!st || !st->hasName())
    return false;
  StringRef name = st->getName();
  // Strip LLVM name prefix if present
  if (name.startswith("\01"))
    name = name.drop_front(1);
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
      // File I/O
      "FILE",
      "__FILE",
      "_IO_FILE",
      "__jmp_buf_tag",
  };
  for (const char *ig : ignorable)
    if (name.equals(ig))
      return true;
  if (name.startswith("__anon"))
    return true; // anonymous sync structs
  return false;
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

// Single predicate for "would we report a data race for this pair?" (borrowed
// from Goblint MCP idea).
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

// Detects data races by checking all pairs of memory accesses.
// A data race occurs when:
//   1. Two instructions may happen in parallel (MHP analysis)
//   2. At least one is a write operation
//   3. They may access the same memory location (alias analysis)
//   4. The pair is not atomic-vs-atomic. Atomic-vs-non-atomic conflicts are
//      still races and must be checked here.
//   5. They are not protected by a common lock (LockSet analysis)
//   6. The memory location is shared/escaped (Escape analysis)
// Reports one bug per "racy component" (connected set of conflicting accesses),
// not per pair (borrowed from Goblint).
std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
  buildSyncObjectSet();
  std::vector<const Instruction *> accesses;
  collectVariableAccesses(accesses);
  if (accesses.empty()) {
    return {};
  }

  // Precompute per-access metadata once and index accesses densely for O(1)
  // union-find operations.
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

  // Fast exit: if every collected access belongs to a single non-overlapping
  // thread, no two accesses can happen in parallel.
  if (threadSelfOverlaps.size() == 1 && !threadSelfOverlaps.begin()->second) {
    return {};
  }

  // Group accesses by underlying base object to avoid quadratic scans across
  // unrelated objects. Cache getUnderlyingObject() and points-to base sets so
  // large modules with coarse Andersen points-to sets do not repeatedly walk
  // IR or explode into hundreds of millions of redundant bucket entries.
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
    const Value *loc = getMemoryLocation(I);
    if (!loc)
      continue;
    const Value *base = getUnderlyingCached(loc);
    if (!base)
      continue;
    const size_t baseId = getBaseId(base);

    ++curStamp;
    if (curStamp == 0) {
      std::fill(baseSeenStamp.begin(), baseSeenStamp.end(), 0);
      curStamp = 1;
    }
    if (baseSeenStamp.size() <= baseId)
      baseSeenStamp.resize(baseId + 1, 0);
    baseSeenStamp[baseId] = curStamp;
    byBase[baseId].push_back(idx);

    // Direct accesses to a GlobalVariable or AllocaInst (or constant/dynamic
    // GEPs/casts rooted directly at them) already have their exact underlying
    // object in byBase[baseId]; querying Andersen points-to adds nothing.
    // For indirect pointers (loads, args, calls, phis), cache points-to base
    // objects by the underlying pointer `base` so multiple GEPs off the same
    // struct pointer share a single points-to query.
    if (m_aliasAnalysis && base->getType()->isPointerTy() &&
        !isa<GlobalVariable>(base) && !isa<AllocaInst>(base)) {
      auto cacheIt = ptsLocToStorageIdx.find(base);
      if (cacheIt == ptsLocToStorageIdx.end()) {
        std::vector<const Value *> pts;
        std::vector<size_t> targetIds;
        if (m_aliasAnalysis->getPointsToSet(base, pts) && !pts.empty()) {
          targetIds.reserve(std::min<size_t>(pts.size(), 256));
          for (const Value *target : pts) {
            const Value *targetBase = getUnderlyingCached(target);
            if (!targetBase)
              continue;
            const size_t tid = getBaseId(targetBase);
            if (baseSeenStamp.size() <= tid)
              baseSeenStamp.resize(tid + 1, 0);
            if (baseSeenStamp[tid] != curStamp) {
              baseSeenStamp[tid] = curStamp;
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
          if (targetBaseId != baseId) {
            byBase[targetBaseId].push_back(idx);
          }
        }
      }
    }
  }

  // Incremental union-find over dense access indices (-1 = not in any racy pair
  // yet). Skipping pairs already in the same connected component is exact and
  // avoids O(N^2) work on large racy buckets.
  std::vector<int> ufParent(numAccesses, -1);
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
    if (ra != rb)
      ufParent[ra] = rb;
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

    // 1. Self-overlapping threads (e.g., multi-instance threads / unknown tid)
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

    // 2. Distinct threads
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

  // Collect racy components within each base object group.
  for (auto &vec : byBase) {
    checkBucket(vec);
  }

  // Compare accesses with wide points-to sets against each other once (instead
  // of repeating across thousands of base buckets), and against matching base
  // buckets using component-deduplicated representatives.
  if (!wideAccesses.empty()) {
    std::vector<size_t> wideBucket = wideAccesses;
    checkBucket(wideBucket);

    // Deduplicate wide accesses only when they are provably equivalent for
    // remaining narrow-bucket checks: same connected component, same thread,
    // same read/write kind, identical points-to set, and no must-locks held.
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
      for (size_t targetBaseId : ptsStorage[wideAccessPtsIdx[wi]]) {
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

  // Group by root: root -> list of instructions in component
  std::unordered_map<int, std::vector<const Instruction *>> components;
  for (size_t idx = 0; idx < numAccesses; ++idx) {
    if (ufParent[idx] < 0)
      continue;
    int root = findRoot(static_cast<int>(idx));
    components[root].push_back(accesses[idx]);
  }

  // One report per component (representative pair = first two in component)
  std::vector<ConcurrencyBugReport> reports;
  for (auto &kv : components) {
    std::vector<const Instruction *> &comp = kv.second;
    if (comp.empty())
      continue;
    const Instruction *rep1 = comp[0];
    const Instruction *rep2 = comp.size() > 1 ? comp[1] : comp[0];
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
    for (const Instruction *inst : comp)
      report.addStep(inst, isWriteAccess(inst) ? "Write" : "Read");
    reports.push_back(std::move(report));
  }
  return reports;
}

// Collect all candidate memory accesses into a flat list so that alias
// checks catch distinct pointer expressions referencing the same memory.
void DataRaceChecker::collectVariableAccesses(
    std::vector<const Instruction *> &accesses) {
  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      if (isMemoryAccess(&*I)) {
        const Value *memLoc = getMemoryLocation(&*I);
        if (memLoc) {
          if (m_threadLocalAnalysis &&
              m_threadLocalAnalysis->accessesThreadLocalStorage(&*I)) {
            continue;
          }
          if (m_staticThreadSharingAnalysis &&
              m_staticThreadSharingAnalysis->classify(&*I) ==
                  lotus::StaticThreadSharingAnalysis::SharingClassification::
                      DefinitelyThreadLocal) {
            continue;
          }
          if (isOpenMPPrivateLikeAccess(&*I, memLoc))
            continue;
          if (isSyncObjectAccess(memLoc))
            continue;
          if (isIgnorableTypeForRace(memLoc->getType()))
            continue;
          if (m_escapeAnalysis && !m_escapeAnalysis->isEscaped(memLoc)) {
            // If it's a local variable that hasn't escaped, it can't race
            // Check if it's a stack allocation (AllocaInst)
            const Value *baseObj = memLoc->stripPointerCasts();
            if (isa<AllocaInst>(baseObj)) {
              continue;
            }
            // For other types (globals, etc), let escape analysis decide.
            // Do NOT discard the access here so that shared globals are
            // considered.
          }
          accesses.push_back(&*I);
        }
      }
    }
  }
}

bool DataRaceChecker::isOpenMPPrivateLikeAccess(const Instruction *inst,
                                                const Value *loc) const {
  if (!inst || !loc || !m_mhpAnalysis) {
    return false;
  }
  auto *regionMHP = dynamic_cast<const MHPAnalysis *>(m_mhpAnalysis);
  if (!regionMHP) {
    return false;
  }
  const OpenMP::OpenMPSemantics *semantics = regionMHP->getOpenMPSemantics();
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

  bool saw_matching_task = false;
  for (const auto &task_uptr : semantics->getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    if (!task || task->task_function != func) {
      continue;
    }
    saw_matching_task = true;
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

  if (!saw_matching_task) {
    return false;
  }

  return false;
}

// Checks if two instructions may access the same memory location using alias
// and points-to analysis (so that *alias_ptr and shared_var are recognized when
// alias_ptr points to shared_var).
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

  const Value *ptr1 = getMemoryLocation(inst1);
  const Value *ptr2 = getMemoryLocation(inst2);
  if (mayAlias(ptr1, ptr2)) {
    return cacheResult(true);
  }
  // Points-to: if one pointer may point to the other's object, they may access
  // same location.
  std::vector<const Value *> pts1, pts2;
  if (m_aliasAnalysis && m_aliasAnalysis->getPointsToSet(ptr1, pts1)) {
    for (const Value *target : pts1) {
      if (target == ptr2 || (m_aliasAnalysis->mayAlias(target, ptr2))) {
        return cacheResult(true);
      }
    }
  }
  if (m_aliasAnalysis && m_aliasAnalysis->getPointsToSet(ptr2, pts2)) {
    for (const Value *target : pts2) {
      if (target == ptr1 || (m_aliasAnalysis->mayAlias(target, ptr1))) {
        return cacheResult(true);
      }
    }
  }
  // Conservative fallback: two globals where one is pointer-typed (e.g.
  // alias_ptr = &shared_var). Alias analysis may not connect *alias_ptr and
  // shared_var; treat as may-access-same.
  if (ptr1 && ptr2 && ptr1 != ptr2) {
    const auto *g1 = dyn_cast<GlobalValue>(ptr1);
    const auto *g2 = dyn_cast<GlobalValue>(ptr2);
    if (g1 && g2 &&
        (ptr1->getType()->isPointerTy() || ptr2->getType()->isPointerTy()))
      return cacheResult(true);
  }
  return cacheResult(false);
}

// Returns true if two values may alias (point to overlapping memory).
// Uses alias analysis wrapper when available, otherwise conservatively assumes
// aliasing.
bool DataRaceChecker::mayAlias(const Value *v1, const Value *v2) const {
  if (!v1 || !v2)
    return false;
  if (v1 == v2)
    return true;
  if (m_aliasAnalysis) {
    return m_aliasAnalysis->mayAlias(v1, v2);
  }
  return true; // Conservative: assume may alias if we can't prove otherwise.
}

bool DataRaceChecker::isMemoryAccess(const Instruction *inst) const {
  return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
         isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isWriteAccess(const Instruction *inst) const {
  return isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) ||
         isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isAtomicOperation(const Instruction *inst) const {
  // Use enhanced CppAtomics module for better atomic recognition
  if (CppAtomics::isAtomic(inst))
    return true;

  // Legacy check for backward compatibility
  if (isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst))
    return true;
  if (const auto *L = dyn_cast<LoadInst>(inst))
    return L->isAtomic();
  if (const auto *S = dyn_cast<StoreInst>(inst))
    return S->isAtomic();

  // Check if it's a fence instruction
  if (CppAtomics::isFence(inst))
    return true;

  return false;
}

// Extracts the memory location (pointer operand) from a memory access
// instruction.
const Value *DataRaceChecker::getMemoryLocation(const Instruction *inst) const {
  if (const auto *load = dyn_cast<LoadInst>(inst))
    return load->getPointerOperand();
  if (const auto *store = dyn_cast<StoreInst>(inst))
    return store->getPointerOperand();
  if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst))
    return rmw->getPointerOperand();
  if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst))
    return cmpxchg->getPointerOperand();
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

      // Traditional pthread primitives
      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst))
        v = m_threadAPI->getAnalysisLockIdentity(inst);
      else if (m_threadAPI->isTDCondWait(inst) ||
               m_threadAPI->isTDCondSignal(inst) ||
               m_threadAPI->isTDCondBroadcast(inst))
        v = m_threadAPI->getCondVal(inst);
      else if (m_threadAPI->isTDBarWait(inst))
        v = m_threadAPI->getBarrierVal(inst);

      // Modern C++ synchronization primitives
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
          // These take mutex/semaphore as argument
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_CALL_ONCE:
          // Takes once_flag as first argument
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_FUTURE_GET:
        case ThreadAPI::TD_FUTURE_WAIT:
        case ThreadAPI::TD_PROMISE_SET:
          // Future/promise are synchronization objects themselves
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_LATCH_COUNT_DOWN:
        case ThreadAPI::TD_LATCH_WAIT:
        case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
          // Latch object
          if (cb->arg_size() >= 1)
            v = cb->getArgOperand(0);
          break;

        case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
        case ThreadAPI::TD_BARRIER_ARRIVE:
        case ThreadAPI::TD_BARRIER_WAIT_CPP20:
        case ThreadAPI::TD_CUDA_BARRIER:
        case ThreadAPI::TD_CUDA_WARP_BARRIER:
          // Barrier object
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
  // Avoid Value::printAsOperand()/ModuleSlotTracker, which eagerly walk all
  // module debug metadata on every call and become quadratic on large modules.
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
