/*
 *
 * Author: rainoftime
 *
 * Deadlock detection (Goblint-style):
 * - Build blocking lock-order graph (L' -> L with acquire site and must-held
 *   gate locks), skipping non-blocking try_lock and shared-read/shared-read
 *   rwlock edges.
 * - Decompose into SCCs (Tarjan) and enumerate elementary cycles via Johnson's
 *   algorithm.
 * - Report a deadlock only when:
 *   1. Acquire events in the cycle are concurrently feasible (MHP & not
 *      strictly ordered by happens-before on a non-overlapping thread).
 *   2. The cycle is not protected by a common outer must-held gate lock.
 */
#include "Checker/Concurrency/DeadlockChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

DeadlockChecker::DeadlockChecker(Module &module,
                                 LockSetAnalysis *locksetAnalysis,
                                 IMHPAnalysis *mhpAnalysis,
                                 lotus::HappensBeforeAnalysis *hbAnalysis,
                                 ThreadAPI *threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_mhpAnalysis(mhpAnalysis), m_hbAnalysis(hbAnalysis),
      m_threadAPI(threadAPI) {}

void DeadlockChecker::buildLockOrderGraph(LockOrderGraph &graph) const {
  if (!m_locksetAnalysis)
    return;

  // Deduplicate only exact (held, lock, acquireInst) triples; retain distinct
  // acquire sites for the same lock pair so MHP / gate-lock feasibility checks
  // never miss a real deadlock due to an infeasible first edge.
  struct EdgeTriple {
    LockID from;
    LockID to;
    const Instruction *inst;
    bool operator<(const EdgeTriple &o) const {
      if (from != o.from)
        return from < o.from;
      if (to != o.to)
        return to < o.to;
      return inst < o.inst;
    }
  };
  std::set<EdgeTriple> seenTriples;

  for (LockID lock : m_locksetAnalysis->getAllLocks()) {
    if (!lock)
      continue;
    LockID canonLock = m_locksetAnalysis->getCanonicalLock(lock);
    if (!canonLock)
      continue;

    auto acquires = m_locksetAnalysis->getLockAcquires(lock);
    for (const Instruction *inst : acquires) {
      if (!inst)
        continue;
      // Non-blocking try_lock cannot block in a wait-for cycle.
      if (m_threadAPI && m_threadAPI->isTryLock(inst))
        continue;

      const bool reqIsSharedRead =
          m_threadAPI && m_threadAPI->isReadLockAcquire(inst) &&
          !m_threadAPI->isWriteLockAcquire(inst);

      LockSet mayHeld = m_locksetAnalysis->getMayLockSetAt(inst);
      LockSet mustHeld = m_locksetAnalysis->getMustLockSetAt(inst);
      LockSet mayReadHeld = m_locksetAnalysis->getMayReadLockSetAt(inst);
      LockSet mayWriteHeld = m_locksetAnalysis->getMayWriteLockSetAt(inst);

      for (LockID held : mayHeld) {
        if (!held)
          continue;
        LockID canonHeld = m_locksetAnalysis->getCanonicalLock(held);
        if (!canonHeld || m_locksetAnalysis->locksMustMatch(canonHeld, canonLock))
          continue;

        // Shared-read holding + shared-read acquiring cannot deadlock each
        // other on an rwlock.
        const bool heldOnlyShared =
            mayReadHeld.count(held) != 0 && mayWriteHeld.count(held) == 0;
        if (reqIsSharedRead && heldOnlyShared)
          continue;

        if (seenTriples.insert({canonHeld, canonLock, inst}).second) {
          LockOrderEdge edge;
          edge.toLock = canonLock;
          edge.acquireInst = inst;
          edge.gateLocks = mustHeld;
          graph[canonHeld].push_back(std::move(edge));
        }
      }
    }
  }
}

bool DeadlockChecker::cycleCanHappenInParallel(
    const std::vector<const Instruction *> &acquireInsts) const {
  if (acquireInsts.size() < 2)
    return false;
  if (!m_mhpAnalysis)
    return true;

  // At a genuine k-cycle deadlock, all k participating acquire sites must be
  // able to block simultaneously, so every pair of acquires in the cycle must
  // be concurrently feasible (MHP and not strictly ordered by happens-before).
  for (size_t i = 0; i < acquireInsts.size(); ++i) {
    for (size_t j = i + 1; j < acquireInsts.size(); ++j) {
      const Instruction *a = acquireInsts[i];
      const Instruction *b = acquireInsts[j];
      if (!a || !b)
        return false;
      if (!m_mhpAnalysis->mayHappenInParallel(a, b))
        return false;
      if (m_hbAnalysis &&
          (m_hbAnalysis->happensBefore(a, b) ||
           m_hbAnalysis->happensBefore(b, a))) {
        return false;
      }
    }
  }
  return true;
}

bool DeadlockChecker::cycleHasCommonGateLock(
    const std::vector<LockOrderEdge> &cycleEdges,
    const std::vector<LockID> &cycleLocks) const {
  if (cycleEdges.size() < 2 || !m_locksetAnalysis)
    return false;

  auto isCycleLock = [&](LockID candidate) {
    for (LockID cl : cycleLocks) {
      if (m_locksetAnalysis->locksMustMatch(candidate, cl))
        return true;
    }
    return false;
  };

  // Start with must-held locks at edge 0 (excluding locks in the cycle itself)
  // and check whether any outer lock is must-held at EVERY edge in the cycle.
  for (LockID gate : cycleEdges[0].gateLocks) {
    if (!gate || isCycleLock(gate))
      continue;
    bool heldByAll = true;
    for (size_t i = 1; i < cycleEdges.size(); ++i) {
      bool matched = false;
      for (LockID otherGate : cycleEdges[i].gateLocks) {
        if (m_locksetAnalysis->locksMustMatch(gate, otherGate)) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        heldByAll = false;
        break;
      }
    }
    if (heldByAll)
      return true;
  }
  return false;
}

std::vector<std::vector<std::pair<mhp::LockID, DeadlockChecker::LockOrderEdge>>>
DeadlockChecker::findLockOrderCycles(const LockOrderGraph &graph) const {
  using CycleStep = std::pair<LockID, LockOrderEdge>;
  using Cycle = std::vector<CycleStep>;

  m_cycleBudgetExceeded = false;

  // Assign deterministic integer IDs to all lock vertices.
  std::vector<LockID> nodes;
  std::unordered_map<LockID, size_t> nodeToIdx;
  auto internNode = [&](LockID l) -> size_t {
    auto it = nodeToIdx.find(l);
    if (it != nodeToIdx.end())
      return it->second;
    size_t idx = nodes.size();
    nodes.push_back(l);
    nodeToIdx.emplace(l, idx);
    return idx;
  };

  for (const auto &kv : graph) {
    internNode(kv.first);
    for (const LockOrderEdge &e : kv.second)
      internNode(e.toLock);
  }
  const size_t n = nodes.size();
  if (n < 2)
    return {};

  // Build deduplicated lock-level adjacency list (u -> unique v) and keep all
  // concrete edges per (u, v) for feasibility matching.
  std::vector<std::vector<size_t>> adj(n);
  std::map<std::pair<size_t, size_t>, std::vector<LockOrderEdge>> pairEdges;
  for (const auto &kv : graph) {
    size_t u = nodeToIdx[kv.first];
    for (const LockOrderEdge &e : kv.second) {
      size_t v = nodeToIdx[e.toLock];
      if (u == v)
        continue;
      auto &vec = pairEdges[{u, v}];
      if (vec.empty())
        adj[u].push_back(v);
      vec.push_back(e);
    }
  }
  for (auto &list : adj)
    std::sort(list.begin(), list.end());

  // 1. Tarjan's SCC decomposition: only SCCs of size >= 2 can form deadlock
  // cycles.
  std::vector<int> disc(n, -1), low(n, -1);
  std::vector<bool> onStack(n, false);
  std::stack<size_t> tarjanStack;
  int timer = 0;
  std::vector<std::vector<size_t>> sccs;

  std::function<void(size_t)> tarjan = [&](size_t u) {
    disc[u] = low[u] = timer++;
    tarjanStack.push(u);
    onStack[u] = true;
    for (size_t v : adj[u]) {
      if (disc[v] == -1) {
        tarjan(v);
        low[u] = std::min(low[u], low[v]);
      } else if (onStack[v]) {
        low[u] = std::min(low[u], disc[v]);
      }
    }
    if (low[u] == disc[u]) {
      std::vector<size_t> comp;
      while (true) {
        size_t w = tarjanStack.top();
        tarjanStack.pop();
        onStack[w] = false;
        comp.push_back(w);
        if (w == u)
          break;
      }
      if (comp.size() >= 2) {
        std::sort(comp.begin(), comp.end());
        sccs.push_back(std::move(comp));
      }
    }
  };

  for (size_t i = 0; i < n; ++i) {
    if (disc[i] == -1)
      tarjan(i);
  }

  // 2. Johnson's elementary cycle enumeration inside each non-trivial SCC.
  constexpr size_t kSafetyMaxCycles = 2048;
  constexpr size_t kSafetyMaxSteps = 500000;
  size_t steps = 0;
  std::vector<std::vector<size_t>> rawNodeCycles;

  for (const auto &comp : sccs) {
    if (rawNodeCycles.size() >= kSafetyMaxCycles || steps >= kSafetyMaxSteps) {
      m_cycleBudgetExceeded = true;
      break;
    }

    std::unordered_set<size_t> compSet(comp.begin(), comp.end());
    for (size_t startIdx = 0; startIdx < comp.size(); ++startIdx) {
      if (rawNodeCycles.size() >= kSafetyMaxCycles ||
          steps >= kSafetyMaxSteps) {
        m_cycleBudgetExceeded = true;
        break;
      }
      const size_t s = comp[startIdx];

      // Induced subgraph on nodes in comp with node >= s.
      std::unordered_set<size_t> blocked;
      std::unordered_map<size_t, std::unordered_set<size_t>> B;
      std::vector<size_t> path;

      std::function<void(size_t)> unblock = [&](size_t u) {
        blocked.erase(u);
        auto it = B.find(u);
        if (it == B.end())
          return;
        auto dependents = std::move(it->second);
        B.erase(it);
        for (size_t w : dependents) {
          if (blocked.count(w))
            unblock(w);
        }
      };

      std::function<bool(size_t)> circuit = [&](size_t v) -> bool {
        if (rawNodeCycles.size() >= kSafetyMaxCycles ||
            ++steps > kSafetyMaxSteps) {
          m_cycleBudgetExceeded = true;
          return false;
        }
        bool foundCycle = false;
        path.push_back(v);
        blocked.insert(v);

        for (size_t w : adj[v]) {
          if (w < s || !compSet.count(w))
            continue;
          if (w == s) {
            if (path.size() >= 2) {
              rawNodeCycles.push_back(path);
              foundCycle = true;
              if (rawNodeCycles.size() >= kSafetyMaxCycles) {
                m_cycleBudgetExceeded = true;
                break;
              }
            }
          } else if (!blocked.count(w)) {
            if (circuit(w))
              foundCycle = true;
          }
        }

        if (foundCycle) {
          unblock(v);
        } else {
          for (size_t w : adj[v]) {
            if (w >= s && compSet.count(w))
              B[w].insert(v);
          }
        }
        path.pop_back();
        return foundCycle;
      };

      circuit(s);
    }
  }

  // 3. For each elementary lock cycle, find a concretely feasible edge tuple
  //    (passing MHP/HB concurrency check and not eliminated by a gate lock).
  std::vector<Cycle> feasibleCycles;
  for (const auto &nodeCycle : rawNodeCycles) {
    const size_t k = nodeCycle.size();
    std::vector<LockID> cycleLocks;
    cycleLocks.reserve(k);
    std::vector<const std::vector<LockOrderEdge> *> edgeChoices;
    edgeChoices.reserve(k);
    for (size_t i = 0; i < k; ++i) {
      size_t u = nodeCycle[i];
      size_t v = nodeCycle[(i + 1) % k];
      cycleLocks.push_back(nodes[u]);
      edgeChoices.push_back(&pairEdges.at({u, v}));
    }

    // Backtrack over concrete acquire-site choices (bounded combinatorially
    // per cycle to stay polynomial while finding the first feasible witness).
    std::vector<LockOrderEdge> chosen(k);
    bool foundWitness = false;
    size_t attempts = 0;
    constexpr size_t kMaxWitnessAttempts = 256;

    std::function<void(size_t)> pickEdges = [&](size_t pos) {
      if (foundWitness || attempts >= kMaxWitnessAttempts)
        return;
      if (pos == k) {
        ++attempts;
        std::vector<const Instruction *> acquires;
        acquires.reserve(k);
        for (size_t i = 0; i < k; ++i)
          acquires.push_back(chosen[i].acquireInst);
        if (!cycleCanHappenInParallel(acquires))
          return;
        if (cycleHasCommonGateLock(chosen, cycleLocks))
          return;
        foundWitness = true;
        return;
      }
      for (const LockOrderEdge &e : *edgeChoices[pos]) {
        chosen[pos] = e;
        pickEdges(pos + 1);
        if (foundWitness || attempts >= kMaxWitnessAttempts)
          return;
      }
    };

    pickEdges(0);
    if (foundWitness) {
      Cycle witness;
      witness.reserve(k);
      for (size_t i = 0; i < k; ++i) {
        witness.emplace_back(cycleLocks[i], chosen[i]);
      }
      feasibleCycles.push_back(std::move(witness));
    }
  }

  return feasibleCycles;
}

std::vector<ConcurrencyBugReport> DeadlockChecker::checkDeadlocks() {
  std::vector<ConcurrencyBugReport> reports;

  LockOrderGraph graph;
  buildLockOrderGraph(graph);

  auto cycles = findLockOrderCycles(graph);
  for (const auto &cycle : cycles) {
    if (cycle.size() < 2)
      continue;

    std::string description = "Potential deadlock: locking order cycle (";
    for (size_t i = 0; i < cycle.size(); ++i) {
      if (i > 0)
        description += " -> ";
      description += getLockDescription(cycle[i].first);
    }
    description += " -> " + getLockDescription(cycle[0].first) +
                   "). Acquires may execute concurrently without a common gate "
                   "lock.";

    ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, description,
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    for (size_t i = 0; i < cycle.size(); ++i) {
      const LockOrderEdge &edge = cycle[i].second;
      if (edge.acquireInst) {
        report.addStep(edge.acquireInst,
                       "Acquire " + getLockDescription(edge.toLock) +
                           " while holding " +
                           getLockDescription(cycle[i].first));
      }
    }
    reports.push_back(std::move(report));
  }

  auto lostWakeups = detectLostWakeups();
  auto barrierIssues = detectBarrierDivergence();
  reports.insert(reports.end(), lostWakeups.begin(), lostWakeups.end());
  reports.insert(reports.end(), barrierIssues.begin(), barrierIssues.end());

  return reports;
}

std::string DeadlockChecker::getLockDescription(mhp::LockID lock) const {
  std::string desc;
  raw_string_ostream os(desc);
  if (!lock) {
    os << "<unknown-lock>";
  } else if (lock->hasName()) {
    os << lock->getName();
  } else {
    os << *lock;
  }
  return os.str();
}

bool DeadlockChecker::isLockOperation(const Instruction *inst) const {
  return m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst);
}

mhp::LockID DeadlockChecker::getLockID(const Instruction *inst) const {
  return m_threadAPI->getAnalysisLockIdentity(inst);
}

const Instruction *
DeadlockChecker::findMatchingUnlock(const Instruction *lockInst) const {
  if (!lockInst)
    return nullptr;

  mhp::LockID lock = getLockID(lockInst);
  if (!lock)
    return nullptr;

  auto releases = m_locksetAnalysis->getLockReleases(lock);
  for (const Instruction *release : releases) {
    if (m_hbAnalysis && m_hbAnalysis->mustPrecede(lockInst, release)) {
      return release;
    }
  }
  return nullptr;
}

std::vector<ConcurrencyBugReport> DeadlockChecker::detectLostWakeups() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI)
    return reports;

  std::vector<std::pair<const Value *, const Instruction *>> condSignals;
  std::vector<std::pair<const Value *, const Instruction *>> condWaits;

  auto normalize = [](const Value *v) -> const Value * {
    return v ? v->stripPointerCasts() : nullptr;
  };

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        if (m_threadAPI->isTDCondWait(&inst)) {
          condWaits.emplace_back(normalize(m_threadAPI->getCondVal(&inst)),
                                 &inst);
        } else if (m_threadAPI->isTDCondSignal(&inst) ||
                   m_threadAPI->isTDCondBroadcast(&inst)) {
          condSignals.emplace_back(normalize(m_threadAPI->getCondVal(&inst)),
                                   &inst);
        }
      }
    }
  }

  for (const auto &waitEntry : condWaits) {
    const Value *cond = waitEntry.first;
    const Instruction *waitInst = waitEntry.second;
    bool hasMatchingSignal = false;
    bool hasPotentialWakeup = false;
    const Instruction *exampleSignal = nullptr;

    for (const auto &sigEntry : condSignals) {
      if (!isSameValue(cond, sigEntry.first))
        continue;
      hasMatchingSignal = true;
      const Instruction *signalInst = sigEntry.second;

      bool canWake = true;
      if (m_mhpAnalysis) {
        bool parallel =
            m_mhpAnalysis->mayHappenInParallel(waitInst, signalInst);
        bool orderedAfter =
            m_hbAnalysis && m_hbAnalysis->mustPrecede(waitInst, signalInst);
        canWake = parallel || orderedAfter;
        if (!canWake &&
            !(m_hbAnalysis &&
              m_hbAnalysis->mustPrecede(signalInst, waitInst))) {
          canWake = true;
        }
      }

      if (canWake) {
        hasPotentialWakeup = true;
        exampleSignal = signalInst;
        break;
      } else if (!exampleSignal) {
        exampleSignal = signalInst;
      }
    }

    if (!hasMatchingSignal || !hasPotentialWakeup) {
      std::string description = "Potential communication deadlock (lost "
                                "wakeup) on condition variable ";
      description += describeValue(cond);
      description +=
          ": wait may not have a matching signal/broadcast reachable after it.";

      ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, description,
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);

      report.addStep(waitInst, "Thread waits on the condition variable here");
      if (exampleSignal) {
        report.addStep(
            exampleSignal,
            "Observed signal/broadcast that might not wake this wait");
      }

      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
DeadlockChecker::detectBarrierDivergence() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI)
    return reports;

  struct BarrierGroup {
    const Value *rep = nullptr;
    std::vector<const Instruction *> waits;
  };
  std::vector<BarrierGroup> barrierGroups;

  auto normalize = [](const Value *v) -> const Value * {
    return v ? v->stripPointerCasts() : nullptr;
  };

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        if (!m_threadAPI->isTDBarWait(&inst))
          continue;
        const Value *barrier = normalize(m_threadAPI->getBarrierVal(&inst));
        BarrierGroup *matched = nullptr;
        for (auto &group : barrierGroups) {
          if (isSameValue(group.rep, barrier)) {
            matched = &group;
            break;
          }
        }
        if (!matched) {
          barrierGroups.push_back({barrier, {&inst}});
        } else {
          matched->waits.push_back(&inst);
        }
      }
    }
  }

  for (const auto &group : barrierGroups) {
    const Value *barrierVal = group.rep;
    const auto &waits = group.waits;

    if (waits.size() < 2) {
      // Multi-instance / SPMD threads executing the same barrier site can
      // legitimately reach it concurrently with themselves.
      if (m_mhpAnalysis &&
          m_mhpAnalysis->mayHappenInParallel(waits.front(), waits.front())) {
        continue;
      }
      std::string desc = "Potential barrier divergence on barrier " +
                         describeValue(barrierVal) +
                         ": only one thread reaches this barrier, so it will "
                         "block indefinitely.";
      ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, desc,
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);
      report.addStep(waits.front(),
                     "Barrier wait with no matching participants");
      reports.push_back(std::move(report));
      continue;
    }

    bool hasParallelPair = false;
    for (size_t i = 0; i < waits.size() && !hasParallelPair; ++i) {
      if (m_mhpAnalysis &&
          m_mhpAnalysis->mayHappenInParallel(waits[i], waits[i])) {
        hasParallelPair = true;
        break;
      }
      for (size_t j = i + 1; j < waits.size(); ++j) {
        if (!m_mhpAnalysis ||
            m_mhpAnalysis->mayHappenInParallel(waits[i], waits[j])) {
          hasParallelPair = true;
          break;
        }
      }
    }

    if (!hasParallelPair) {
      std::string desc = "Potential barrier divergence on barrier " +
                         describeValue(barrierVal) +
                         ": threads using this barrier do not appear to reach "
                         "it concurrently.";
      ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, desc,
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);
      report.addStep(waits.front(), "Barrier wait that may stall");
      report.addStep(waits.back(),
                     "Another barrier wait that cannot run concurrently");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

bool DeadlockChecker::isSameValue(const Value *lhs, const Value *rhs) const {
  if (!lhs || !rhs)
    return lhs == rhs;
  const Value *s1 = lhs->stripPointerCasts();
  const Value *s2 = rhs->stripPointerCasts();
  if (s1 == s2)
    return true;
  if (m_aliasAnalysis && s1->getType()->isPointerTy() &&
      s2->getType()->isPointerTy()) {
    return m_aliasAnalysis->mayAlias(s1, s2);
  }
  return getUnderlyingObject(s1) == getUnderlyingObject(s2);
}

std::string DeadlockChecker::describeValue(const Value *value) const {
  std::string desc;
  raw_string_ostream os(desc);
  if (value) {
    if (value->hasName())
      os << value->getName();
    else
      os << *value;
  } else {
    os << "<unknown>";
  }
  return os.str();
}

} // namespace concurrency
