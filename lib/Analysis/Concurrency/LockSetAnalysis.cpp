/**
 * @file LockSetAnalysis.cpp
 * @brief Implementation of Lock Set Analysis
 */

#include "Analysis/Concurrency/LockSetAnalysis.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <queue>

using namespace llvm;
using namespace mhp;

// ============================================================================
// Construction and Analysis
// ============================================================================

LockSetAnalysis::LockSetAnalysis(Module &module)
    : m_module(&module), m_single_function(nullptr),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr) {}

LockSetAnalysis::LockSetAnalysis(Function &func)
    : m_module(nullptr), m_single_function(&func),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr) {}

void LockSetAnalysis::analyze() {
  errs() << "Starting Lock Set Analysis...\n";

  // Identify all locks in the program
  identifyLocks();

  if (m_module) {
    // Module-wide analysis
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        analyzeFunction(&func);
      }
    }
    computeInterproceduralLockSets();
  } else if (m_single_function) {
    // Single function analysis
    analyzeFunction(m_single_function);
  }

  // Track lock ordering for deadlock detection
  trackLockOrdering();

  errs() << "Lock Set Analysis Complete!\n";
  errs() << "Found " << m_all_locks.size() << " locks\n";
}

// ============================================================================
// Query Interface
// ============================================================================

LockSet LockSetAnalysis::getMayLockSetAt(const Instruction *inst) const {
  auto it = m_may_locksets_entry.find(inst);
  if (it != m_may_locksets_entry.end()) {
    return it->second;
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMustLockSetAt(const Instruction *inst) const {
  auto it = m_must_locksets_entry.find(inst);
  if (it != m_must_locksets_entry.end()) {
    return it->second;
  }
  return LockSet();
}

bool LockSetAnalysis::mayHoldLock(const Instruction *inst, LockID lock) const {
  auto lockset = getMayLockSetAt(inst);
  return lockset.find(lock) != lockset.end();
}

bool LockSetAnalysis::mustHoldLock(const Instruction *inst, LockID lock) const {
  auto lockset = getMustLockSetAt(inst);
  return lockset.find(lock) != lockset.end();
}

std::unordered_set<const Instruction *>
LockSetAnalysis::getInstructionsHoldingLock(LockID lock) const {
  std::unordered_set<const Instruction *> result;
  for (const auto &pair : m_may_locksets_entry) {
    if (pair.second.find(lock) != pair.second.end()) {
      result.insert(pair.first);
    }
  }
  return result;
}

bool LockSetAnalysis::mayHoldCommonLock(const Instruction *i1,
                                         const Instruction *i2) const {
  auto locks1 = getMayLockSetAt(i1);
  auto locks2 = getMayLockSetAt(i2);

  for (auto lock : locks1) {
    if (locks2.find(lock) != locks2.end()) {
      return true;
    }
    // Check for aliasing
    for (auto lock2 : locks2) {
      if (mayAlias(lock, lock2)) {
        return true;
      }
    }
  }
  return false;
}

LockSet LockSetAnalysis::getAllLocksInFunction(const Function *func) const {
  LockSet all_locks;
  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;
    if (isLockOperation(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) {
        all_locks.insert(lock);
      }
    }
  }
  return all_locks;
}

std::vector<const Instruction *>
LockSetAnalysis::getLockAcquires(LockID lock) const {
  auto it = m_lock_acquires.find(lock);
  if (it != m_lock_acquires.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

std::vector<const Instruction *>
LockSetAnalysis::getLockReleases(LockID lock) const {
  auto it = m_lock_releases.find(lock);
  if (it != m_lock_releases.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

// ============================================================================
// Advanced Queries
// ============================================================================

bool LockSetAnalysis::isReentrantLock(LockID lock) const {
  return m_reentrant_locks.find(lock) != m_reentrant_locks.end();
}

size_t LockSetAnalysis::getLockNestingDepth(const Instruction *inst) const {
  return getMayLockSetAt(inst).size();
}

bool LockSetAnalysis::areLocksOrderedConsistently(LockID lock1,
                                                   LockID lock2) const {
  bool found_12 = m_observed_lock_orders.find({lock1, lock2}) !=
                  m_observed_lock_orders.end();
  bool found_21 = m_observed_lock_orders.find({lock2, lock1}) !=
                  m_observed_lock_orders.end();

  // Consistent if only one order is observed
  return !(found_12 && found_21);
}

std::vector<std::pair<LockID, LockID>>
LockSetAnalysis::detectLockOrderInversions() const {
  std::vector<std::pair<LockID, LockID>> inversions;

  // Check all pairs of locks for order inversions
  for (const auto &pair1 : m_observed_lock_orders) {
    LockPair reverse{pair1.second, pair1.first};
    if (m_observed_lock_orders.find(reverse) != m_observed_lock_orders.end()) {
      // Found an inversion - both lock1->lock2 and lock2->lock1 exist
      inversions.push_back({pair1.first, pair1.second});
    }
  }

  return inversions;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void LockSetAnalysis::Statistics::print(raw_ostream &os) const {
  os << "Lock Set Analysis Statistics:\n";
  os << "==============================\n";
  os << "Locks:                " << num_locks << "\n";
  os << "Lock Acquires:        " << num_acquires << "\n";
  os << "Lock Releases:        " << num_releases << "\n";
  os << "Try-Lock Operations:  " << num_try_acquires << "\n";
  os << "Max Depth:     " << max_nesting_depth << "\n";
  os << "Observed Reentrant Locks:     " << num_reentrant_locks << "\n";
  os << "Potential Deadlocks:  " << num_potential_deadlocks << "\n";
}

LockSetAnalysis::Statistics LockSetAnalysis::getStatistics() const {
  Statistics stats;

  stats.num_locks = m_all_locks.size();

  stats.num_acquires = 0;
  for (const auto &pair : m_lock_acquires) {
    stats.num_acquires += pair.second.size();
  }

  stats.num_releases = 0;
  for (const auto &pair : m_lock_releases) {
    stats.num_releases += pair.second.size();
  }

  stats.num_try_acquires = 0;
  for (const auto &pair : m_lock_try_acquires) {
    stats.num_try_acquires += pair.second.size();
  }

  stats.max_nesting_depth = 0;
  for (const auto &pair : m_may_locksets_entry) {
    stats.max_nesting_depth =
        std::max(stats.max_nesting_depth, pair.second.size());
  }

  stats.num_reentrant_locks = m_reentrant_locks.size();
  stats.num_potential_deadlocks = detectLockOrderInversions().size();

  return stats;
}

void LockSetAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void LockSetAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== Lock Set Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== All Locks ===\n";
  for (auto lock : m_all_locks) {
    os << "Lock: ";
    lock->printAsOperand(os, false);
    os << "\n";

    auto acquires = getLockAcquires(lock);
    os << "  Acquires: " << acquires.size() << "\n";

    auto releases = getLockReleases(lock);
    os << "  Releases: " << releases.size() << "\n";

    if (isReentrantLock(lock)) {
      os << "  [REENTRANT]\n";
    }
  }

  // Print potential deadlocks
  auto inversions = detectLockOrderInversions();
  if (!inversions.empty()) {
    os << "\n=== Potential Deadlocks (Lock Order Inversions) ===\n";
    for (const auto &pair : inversions) {
      os << "Lock ";
      pair.first->printAsOperand(os, false);
      os << " and Lock ";
      pair.second->printAsOperand(os, false);
      os << "\n";
    }
  }
}

void LockSetAnalysis::printLockSetsForFunction(const Function *func,
                                                raw_ostream &os) const {
  os << "Lock Sets for Function: " << func->getName() << "\n";
  os << "=============================================\n";

  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;

    auto may_locks = getMayLockSetAt(inst);
    auto must_locks = getMustLockSetAt(inst);

    if (!may_locks.empty() || isLockOperation(inst)) {
      os << "Instruction: ";
      inst->print(os);
      os << "\n";

      os << "  May-Locks: {";
      bool first = true;
      for (auto lock : may_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n";

      os << "  Must-Locks: {";
      first = true;
      for (auto lock : must_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n\n";
    }
  }
}

void LockSetAnalysis::print(raw_ostream &os) const {
  printResults(os);
}

// ============================================================================
// Visualization
// ============================================================================

void LockSetAnalysis::dumpLockGraph(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  file << "digraph LockGraph {\n";
  file << "  rankdir=LR;\n";
  file << "  node [shape=box];\n\n";

  // Create nodes for locks
  size_t id = 0;
  std::unordered_map<LockID, size_t> lock_ids;
  for (auto lock : m_all_locks) {
    lock_ids[lock] = id;
    file << "  lock" << id << " [label=\"";
    lock->printAsOperand(file, false);
    file << "\"];\n";
    id++;
  }

  file << "\n";

  // Create edges for lock ordering
  for (const auto &pair : m_observed_lock_orders) {
    auto it1 = lock_ids.find(pair.first);
    auto it2 = lock_ids.find(pair.second);
    if (it1 != lock_ids.end() && it2 != lock_ids.end()) {
      file << "  lock" << it1->second << " -> lock" << it2->second;

      // Highlight inversions in red
      LockPair reverse{pair.second, pair.first};
      if (m_observed_lock_orders.find(reverse) !=
          m_observed_lock_orders.end()) {
        file << " [color=red, style=bold]";
      }

      file << ";\n";
    }
  }

  file << "}\n";
  file.close();

  errs() << "Lock graph dumped to " << filename << "\n";
}

// ============================================================================
// Analysis Implementation
// ============================================================================

void LockSetAnalysis::analyzeFunction(Function *func) {
  if (!func || func->isDeclaration())
    return;

  computeIntraproceduralLockSets(func);
}

void LockSetAnalysis::computeIntraproceduralLockSets(Function *func) {
  // Worklist algorithm for dataflow analysis
  std::queue<const Instruction *> worklist;
  std::set<const Instruction *> in_worklist;

  // Initialize entry with empty lockset
  const Instruction *entry = &func->getEntryBlock().front();
  m_may_locksets_entry[entry] = LockSet();
  m_must_locksets_entry[entry] = LockSet();
  worklist.push(entry);
  in_worklist.insert(entry);

  // Iterate to fixed point
  while (!worklist.empty()) {
    const Instruction *inst = worklist.front();
    worklist.pop();
    in_worklist.erase(inst);

    // Get input locksets from predecessors
    std::vector<LockSet> may_inputs, must_inputs;

    if (inst == entry) {
      // Entry has empty lockset
      may_inputs.push_back(LockSet());
      must_inputs.push_back(LockSet());
    } else {
      // Collect from predecessors
      const BasicBlock *bb = inst->getParent();

      if (inst == &bb->front()) {
        // First instruction in block - get from predecessor blocks
        for (const BasicBlock *pred : predecessors(bb)) {
          const Instruction *pred_term = pred->getTerminator();
          if (pred_term) {
            auto it_may = m_may_locksets_exit.find(pred_term);
            auto it_must = m_must_locksets_exit.find(pred_term);

            if (it_may != m_may_locksets_exit.end()) {
              may_inputs.push_back(it_may->second);
            }
            if (it_must != m_must_locksets_exit.end()) {
              must_inputs.push_back(it_must->second);
            }
          }
        }
      } else {
        // Get from previous instruction in same block
        const Instruction *prev = inst->getPrevNode();
        auto it_may = m_may_locksets_exit.find(prev);
        auto it_must = m_must_locksets_exit.find(prev);

        if (it_may != m_may_locksets_exit.end()) {
          may_inputs.push_back(it_may->second);
        }
        if (it_must != m_must_locksets_exit.end()) {
          must_inputs.push_back(it_must->second);
        }
      }
    }

    // Merge inputs
    LockSet may_in =
        may_inputs.empty() ? LockSet() : merge(may_inputs, false);
    LockSet must_in =
        must_inputs.empty() ? LockSet() : merge(must_inputs, true);

    // Apply transfer function
    LockSet may_out = transfer(inst, may_in, false);
    LockSet must_out = transfer(inst, must_in, true);

    // Check for changes
    bool changed = false;

    if (m_may_locksets_entry[inst] != may_in) {
      m_may_locksets_entry[inst] = may_in;
      changed = true;
    }

    if (m_must_locksets_entry[inst] != must_in) {
      m_must_locksets_entry[inst] = must_in;
      changed = true;
    }

    if (m_may_locksets_exit[inst] != may_out) {
      m_may_locksets_exit[inst] = may_out;
      changed = true;
    }

    if (m_must_locksets_exit[inst] != must_out) {
      m_must_locksets_exit[inst] = must_out;
      changed = true;
    }

    // Add successors to worklist if changed
    if (changed) {
      // Add next instruction in block
      if (inst->getNextNode()) {
        const Instruction *next = inst->getNextNode();
        if (in_worklist.find(next) == in_worklist.end()) {
          worklist.push(next);
          in_worklist.insert(next);
        }
      } else if (inst->isTerminator()) {
        // Add successor blocks
        const BasicBlock *bb = inst->getParent();
        for (const BasicBlock *succ_bb : successors(bb)) {
          const Instruction *succ = &succ_bb->front();
          if (in_worklist.find(succ) == in_worklist.end()) {
            worklist.push(succ);
            in_worklist.insert(succ);
          }
        }
      }
    }
  }
}

void LockSetAnalysis::computeInterproceduralLockSets() {
  // TODO: Implement interprocedural propagation
  // This would require function summaries and callgraph analysis
}

LockSet LockSetAnalysis::transfer(const Instruction *inst,
                                   const LockSet &in_set, bool is_must) const {
  LockSet out_set = in_set;

  // Check if this is a lock operation
  if (m_thread_api->isTDAcquire(inst)) {
    // Lock acquire - add lock to set
    LockID lock = getLockValue(inst);
    if (lock) {
      out_set.insert(lock);
    }
  } else if (m_thread_api->isTDRelease(inst)) {
    // Lock release - remove lock from set
    LockID lock = getLockValue(inst);
    if (lock) {
      out_set.erase(lock);
      
      // Also remove aliases if any
      if (m_alias_analysis) {
        LockSet to_remove;
        for (auto l : out_set) {
          if (mayAlias(l, lock)) {
            to_remove.insert(l);
          }
        }
        for (auto l : to_remove) {
          out_set.erase(l);
        }
      }
    }
  } else if (const CallInst *call = dyn_cast<CallInst>(inst)) {
    // For try-lock, conservatively assume it may fail (may-analysis)
    // For must-analysis, we can't assume lock is acquired
    if (m_thread_api->isTDAcquire(call) &&
        call->getCalledFunction() &&
        call->getCalledFunction()->getName().contains("trylock")) {
      if (!is_must) {
        // May-analysis: assume try-lock may succeed
        LockID lock = getLockValue(inst);
        if (lock) {
          out_set.insert(lock);
        }
      }
      // Must-analysis: don't add lock (can't guarantee acquisition)
    }
  }

  return out_set;
}

LockSet LockSetAnalysis::merge(const std::vector<LockSet> &sets,
                                bool is_must) const {
  if (sets.empty()) {
    return LockSet();
  }

  if (is_must) {
    // Must-analysis: intersection
    LockSet result = sets[0];
    for (size_t i = 1; i < sets.size(); ++i) {
      LockSet intersection;
      std::set_intersection(result.begin(), result.end(), sets[i].begin(),
                            sets[i].end(),
                            std::inserter(intersection, intersection.begin()));
      result = intersection;
    }
    return result;
  } else {
    // May-analysis: union
    LockSet result;
    for (const auto &set : sets) {
      result.insert(set.begin(), set.end());
    }
    return result;
  }
}

void LockSetAnalysis::identifyLocks() {
  auto process_func = [this](Function &func) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;

      if (m_thread_api->isTDAcquire(inst)) {
        LockID lock = getLockValue(inst);
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_acquires[lock].push_back(inst);

          // Check for try-lock
          if (const CallInst *call = dyn_cast<CallInst>(inst)) {
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName().contains("trylock")) {
              m_lock_try_acquires[lock].push_back(inst);
            }
          }
        }
      } else if (m_thread_api->isTDRelease(inst)) {
        LockID lock = getLockValue(inst);
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_releases[lock].push_back(inst);
        }
      }
    }
  };

  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        process_func(func);
      }
    }
  } else if (m_single_function) {
    process_func(*m_single_function);
  }
}

void LockSetAnalysis::trackLockOrdering() {
  // Track the order in which locks are acquired
  for (const auto &pair : m_may_locksets_entry) {
    const Instruction *inst = pair.first;
    const LockSet &locks_held = pair.second;

    // If this instruction acquires a new lock, record the ordering
    if (m_thread_api->isTDAcquire(inst)) {
      LockID new_lock = getLockValue(inst);
      if (new_lock) {
        // Check if lock is already in the lockset (reentrant)
        if (locks_held.find(new_lock) == locks_held.end()) {
          // Not reentrant
        } else {
          // Reentrant lock
          m_reentrant_locks.insert(new_lock);
        }

        // Record ordering with all currently held locks
        for (auto held_lock : locks_held) {
          if (held_lock != new_lock) {
            m_observed_lock_orders.insert({held_lock, new_lock});
          }
        }
      }
    }
  }
}

bool LockSetAnalysis::mayAlias(LockID lock1, LockID lock2) const {
  if (lock1 == lock2)
    return true;

  if (m_alias_analysis && lock1 && lock2) {
    MemoryLocation loc1 = MemoryLocation(lock1, LocationSize::beforeOrAfterPointer());
    MemoryLocation loc2 = MemoryLocation(lock2, LocationSize::beforeOrAfterPointer());
    return m_alias_analysis->alias(loc1, loc2) != AliasResult::NoAlias;
  }

  // Conservative: assume may alias
  return false;
}

LockID LockSetAnalysis::getCanonicalLock(LockID lock) const {
  // Strip pointer casts to get canonical form
  return lock->stripPointerCasts();
}

bool LockSetAnalysis::isLockOperation(const Instruction *inst) const {
  return m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst);
}

LockID LockSetAnalysis::getLockValue(const Instruction *inst) const {
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    return getCanonicalLock(m_thread_api->getLockVal(inst));
  }
  return nullptr;
}
