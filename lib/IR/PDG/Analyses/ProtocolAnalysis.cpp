#include "IR/PDG/Analyses/ProtocolAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"

#include "IR/PDG/Models/ValueFacts.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

using namespace llvm;

namespace pdg {
namespace {
using Location = std::pair<const GlobalVariable *, int64_t>;
using Locks = std::set<Location>;

bool named(const CallBase &call, StringRef name) {
  const Function *target = ValueFacts::callee(call);
  return target && target->isDeclaration() &&
         ValueFacts::hasLibraryName(*target, name.str());
}

Optional<Location> location(const Value *pointer, const DataLayout &layout) {
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  const auto *global = dyn_cast<GlobalVariable>(base);
  if (!global || global->isThreadLocal() || global->isConstant())
    return None;
  return Location{global, offset};
}

// Only blocking nonrecursive mutex APIs. try_lock and std::lock's multi-lock
// avoidance algorithm cannot be treated as ordinary lock-order edges.
int mutexOperation(const CallBase &call) {
  if (call.arg_empty())
    return 0;
  if (named(call, "pthread_mutex_lock") || named(call, "mtx_lock"))
    return 1;
  if (named(call, "pthread_mutex_unlock") || named(call, "mtx_unlock"))
    return -1;
  const Function *target = ValueFacts::callee(call);
  if (!target || !target->isDeclaration())
    return 0;
  std::string full = demangle(target->getName().str());
  if (full == "std::mutex::lock()" || full == "std::__1::mutex::lock()")
    return 1;
  if (full == "std::mutex::unlock()" || full == "std::__1::mutex::unlock()")
    return -1;
  return 0;
}

struct Access {
  Location address;
  uint64_t bytes;
  bool writes, atomic;
  const Instruction *site;
  Locks held;
};
struct Order {
  Location first, second;
  const Instruction *site;
};
struct Worker {
  std::vector<Access> accesses;
  std::vector<Order> orders;
};

Worker workerEvents(const Function &function, size_t limit,
                    ProtocolAnalysisResult &result) {
  Worker output;
  if (function.isDeclaration())
    return output;
  const auto &layout = function.getParent()->getDataLayout();
  using State = std::pair<const BasicBlock *, Locks>;
  std::deque<State> pending{{&function.getEntryBlock(), {}}};
  std::set<State> seen;
  while (!pending.empty()) {
    State state = std::move(pending.front());
    pending.pop_front();
    if (!seen.insert(state).second)
      continue;
    if (limit && seen.size() > limit) {
      result.incomplete = true;
      break;
    }
    Locks held = state.second;
    for (const Instruction &inst : *state.first) {
      if (const auto *call = dyn_cast<CallBase>(&inst)) {
        int operation = mutexOperation(*call);
        auto mutex =
            operation ? location(call->getArgOperand(0), layout) : None;
        if (mutex) {
          if (operation > 0) {
            for (const auto &old : held)
              if (old != *mutex)
                output.orders.push_back({old, *mutex, call});
            held.insert(*mutex);
          } else
            held.erase(*mutex);
        } else if (!isa<IntrinsicInst>(call)) {
          // Opaque helpers may unlock a held mutex. Retaining the lock would
          // silently suppress a real conflict after such a call.
          held.clear();
        }
      }
      const Value *pointer = nullptr;
      Type *type = nullptr;
      bool writes = false, atomic = false;
      if (const auto *load = dyn_cast<LoadInst>(&inst)) {
        pointer = load->getPointerOperand();
        type = load->getType();
        atomic = load->isAtomic();
      } else if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        pointer = store->getPointerOperand();
        type = store->getValueOperand()->getType();
        atomic = store->isAtomic();
        writes = true;
      } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
        pointer = rmw->getPointerOperand();
        type = rmw->getValOperand()->getType();
        atomic = writes = true;
      } else if (const auto *cmp = dyn_cast<AtomicCmpXchgInst>(&inst)) {
        pointer = cmp->getPointerOperand();
        type = cmp->getCompareOperand()->getType();
        atomic = writes = true;
      }
      if (pointer && type->isSized()) {
        auto address = location(pointer, layout);
        auto bytes = layout.getTypeStoreSize(type);
        if (address && !bytes.isScalable())
          output.accesses.push_back(
              {*address, bytes.getFixedValue(), writes, atomic, &inst, held});
      }
    }
    for (const BasicBlock *next : successors(state.first))
      pending.push_back({next, held});
  }
  return output;
}

bool overlaps(const Access &a, const Access &b) {
  if (a.address.first != b.address.first || !a.bytes || !b.bytes)
    return false;
  // Wide arithmetic prevents target-sized offsets from overflowing the check.
  APInt left(128, static_cast<uint64_t>(a.address.second), true);
  APInt right(128, static_cast<uint64_t>(b.address.second), true);
  return left.slt(right + APInt(128, b.bytes)) &&
         right.slt(left + APInt(128, a.bytes));
}

bool commonLock(const Locks &a, const Locks &b) {
  for (const auto &lock : a)
    if (b.count(lock))
      return true;
  return false;
}

bool exclusiveCreate(const CallBase &call) {
  if (!named(call, "open") || call.arg_size() < 2)
    return false;
  auto flags = ValueFacts::integer(*call.getArgOperand(1));
  auto create = ValueFacts::macroInteger(*call.getModule(), "O_CREAT");
  auto exclusive = ValueFacts::macroInteger(*call.getModule(), "O_EXCL");
  if (!flags || flags->getBitWidth() > 64 || !create || !exclusive)
    return false;
  uint64_t mask = *create | *exclusive;
  return mask && (flags->getZExtValue() & mask) == mask;
}

void filesystem(const Function &function, size_t limit,
                ProtocolAnalysisResult &result) {
  for (const Instruction &inst : instructions(function)) {
    const auto *check = dyn_cast<CallBase>(&inst);
    if (!check || check->arg_empty() ||
        !(named(*check, "access") || named(*check, "stat") ||
          named(*check, "lstat")))
      continue;
    const Value *path = check->getArgOperand(0);
    auto literal = ValueFacts::constantString(*path);
    // stat/lstat write an output structure which could alias a mutable path.
    // Without a no-alias proof, retain only their immutable-path cases.
    if (!literal && !named(*check, "access"))
      continue;
    std::deque<const Instruction *> pending;
    if (check->getNextNode())
      pending.push_back(check->getNextNode());
    std::set<const Instruction *> seen;
    while (!pending.empty()) {
      const Instruction *next = pending.front();
      pending.pop_front();
      if (!seen.insert(next).second)
        continue;
      if (limit && seen.size() > limit) {
        result.incomplete = true;
        break;
      }
      if (const auto *use = dyn_cast<CallBase>(next)) {
        bool sensitive =
            !use->arg_empty() && (named(*use, "open") || named(*use, "fopen") ||
                                  named(*use, "unlink") ||
                                  named(*use, "chmod") || named(*use, "chown"));
        if (sensitive) {
          auto other = ValueFacts::constantString(*use->getArgOperand(0));
          bool same = literal && other ? *literal == *other
                                       : path == use->getArgOperand(0);
          if (same && !exclusiveCreate(*use))
            result.findings.push_back(
                {"lotus/filesystem-toctou",
                 use,
                 "A path is checked and subsequently used by a separate "
                 "filesystem operation; another process may replace it.",
                 {check}});
        }
        if (!isa<IntrinsicInst>(use))
          // Calls may change path contents, cwd or the filesystem itself.
          continue;
      }
      if (!literal && next->mayWriteToMemory())
        continue;
      if (const Instruction *following = next->getNextNode())
        pending.push_back(following);
      else
        for (const BasicBlock *successor : successors(next->getParent()))
          pending.push_back(&successor->front());
    }
  }
}
} // namespace

const std::vector<AnalysisRuleDescriptor> &ProtocolAnalysis::catalog() {
  static const std::vector<AnalysisRuleDescriptor> rules = {
      {"lotus/shared-global-race", "warning", "",
       "Conflicting exact global byte ranges in directly registered pthread "
       "workers launched in the same block without an intervening join. "
       "Models successful blocking pthread/mtx/std::mutex operations; a shared "
       "held mutex or two atomic accesses suppresses a candidate. Thread-local "
       "objects excluded. No parent-thread, heap, helper-effect or general "
       "happens-before analysis; lock failure and branch feasibility partial."},
      {"lotus/lock-order-cycle", "warning", "",
       "Cycles of global mutex acquisition order across potentially "
       "overlapping "
       "pthread workers. CFG locksets and arbitrary cycle lengths supported; "
       "successful blocking locks assumed. No try_lock, recursive-mutex "
       "attribute, std::lock, dynamic alias or helper summaries. Reports a "
       "potential deadlock, not a feasible concurrent schedule."},
      {"lotus/filesystem-toctou", "warning", "",
       "CFG access/stat/lstat followed by open/fopen/unlink/chmod/chown on "
       "the same immutable literal, or access followed by use of an unchanged "
       "SSA path. Mutable stat output aliases remain unknown. Opaque calls and "
       "mutable-path writes cut exploration. Proven O_CREAT|O_EXCL suppresses "
       "open using target debug macros. No symlink resolution, "
       "descriptor-based "
       "fstat checks, openat-relative paths or general path feasibility."}};
  return rules;
}

ProtocolAnalysisResult ProtocolAnalysis::analyze(const Module &module,
                                           size_t max_states) const {
  ProtocolAnalysisResult result;
  std::map<const Function *, Worker> workers;
  std::set<std::pair<const Function *, const Function *>> pairs;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    filesystem(function, max_states, result);
    for (const BasicBlock &block : function) {
      std::vector<const Function *> launched;
      for (const Instruction &inst : block) {
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call)
          continue;
        if (named(*call, "pthread_join")) {
          launched.clear();
          continue;
        }
        if (!named(*call, "pthread_create") || call->arg_size() < 3)
          continue;
        const auto *worker = dyn_cast<Function>(
            call->getArgOperand(2)->stripPointerCastsAndAliases());
        if (!worker || worker->isDeclaration())
          continue;
        if (!workers.count(worker))
          workers.emplace(worker, workerEvents(*worker, max_states, result));
        for (const Function *other : launched)
          pairs.insert({other, worker});
        launched.push_back(worker);
      }
    }
  }
  for (const auto &pair : pairs) {
    const Worker &left = workers.at(pair.first),
                 &right = workers.at(pair.second);
    for (const Access &a : left.accesses)
      for (const Access &b : right.accesses)
        if ((a.writes || b.writes) && !(a.atomic && b.atomic) &&
            overlaps(a, b) && !commonLock(a.held, b.held))
          result.findings.push_back(
              {"lotus/shared-global-race",
               a.site,
               "Potentially overlapping workers access the same global bytes "
               "without a common modeled mutex; at least one access writes.",
               {b.site}});
    // Search cycles containing an order edge from each worker.
    for (const Order &edge : left.orders) {
      using State = std::pair<Location, bool>;
      std::deque<State> pending{{edge.second, false}};
      std::set<State> seen;
      std::map<State, std::pair<State, const Instruction *>> predecessor;
      while (!pending.empty()) {
        State state = pending.front();
        pending.pop_front();
        if (!seen.insert(state).second)
          continue;
        if (max_states && seen.size() > max_states) {
          result.incomplete = true;
          break;
        }
        if (state.first == edge.first && state.second) {
          std::vector<const Instruction *> evidence;
          State cursor = state;
          while (predecessor.count(cursor)) {
            auto previous = predecessor.at(cursor);
            evidence.push_back(previous.second);
            cursor = previous.first;
          }
          result.findings.push_back(
              {"lotus/lock-order-cycle",
               edge.site,
               "Potentially overlapping workers acquire global mutexes in a "
               "cyclic order.",
               std::move(evidence)});
          break;
        }
        for (const Order &next : left.orders)
          if (next.first == state.first) {
            State following{next.second, state.second};
            if (!seen.count(following) && !predecessor.count(following)) {
              predecessor.emplace(following, std::make_pair(state, next.site));
              pending.push_back(following);
            }
          }
        for (const Order &next : right.orders)
          if (next.first == state.first) {
            State following{next.second, true};
            if (!seen.count(following) && !predecessor.count(following)) {
              predecessor.emplace(following, std::make_pair(state, next.site));
              pending.push_back(following);
            }
          }
      }
    }
  }
  std::set<std::pair<std::string, const Instruction *>> emitted;
  result.findings.erase(
      std::remove_if(
          result.findings.begin(), result.findings.end(),
          [&](const AnalysisFinding &finding) {
            return !emitted.insert({finding.rule_id, finding.site}).second;
          }),
      result.findings.end());
  return result;
}
} // namespace pdg
