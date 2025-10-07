/**
 * @file MHPAnalysis.cpp
 * @brief Implementation of May-Happen-in-Parallel Analysis
 */

#include "Analysis/Concurrency/MHPAnalysis.h"

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
// SyncNode Implementation
// ============================================================================

size_t SyncNode::next_id = 0;

void SyncNode::print(raw_ostream &os) const {
  os << "SyncNode[" << m_node_id << "]: ";
  os << "Type=" << getSyncNodeTypeName(m_type);
  os << ", Thread=" << m_thread_id;

  if (m_instruction) {
    os << ", Inst=";
    m_instruction->print(os);
  }

  if (m_lock_value) {
    os << ", Lock=";
    m_lock_value->printAsOperand(os, false);
  }

  if (m_forked_thread != 0) {
    os << ", ForkedThread=" << m_forked_thread;
  }

  if (m_joined_thread != 0) {
    os << ", JoinedThread=" << m_joined_thread;
  }
}

std::string SyncNode::toString() const {
  std::string str;
  raw_string_ostream os(str);
  print(os);
  return os.str();
}

// ============================================================================
// ThreadFlowGraph Implementation
// ============================================================================

ThreadFlowGraph::~ThreadFlowGraph() {
  for (auto *node : m_all_nodes) {
    delete node;
  }
}

SyncNode *ThreadFlowGraph::createNode(const Instruction *inst,
                                      SyncNodeType type, ThreadID tid) {
  auto *node = new SyncNode(inst, type, tid);
  m_all_nodes.push_back(node);

  if (inst) {
    m_inst_to_node[inst] = node;
  }

  return node;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst) const {
  auto it = m_inst_to_node.find(inst);
  return it != m_inst_to_node.end() ? it->second : nullptr;
}

void ThreadFlowGraph::addThread(ThreadID tid, const Function *entry) {
  m_thread_entries[tid] = entry;
}

const Function *ThreadFlowGraph::getThreadEntry(ThreadID tid) const {
  auto it = m_thread_entries.find(tid);
  return it != m_thread_entries.end() ? it->second : nullptr;
}

std::vector<ThreadID> ThreadFlowGraph::getAllThreads() const {
  std::vector<ThreadID> threads;
  threads.reserve(m_thread_entries.size());
  for (const auto &pair : m_thread_entries) {
    threads.push_back(pair.first);
  }
  return threads;
}

void ThreadFlowGraph::setThreadEntry(ThreadID tid, SyncNode *entry) {
  m_thread_entry_nodes[tid] = entry;
}

void ThreadFlowGraph::setThreadExit(ThreadID tid, SyncNode *exit) {
  m_thread_exit_nodes[tid] = exit;
}

SyncNode *ThreadFlowGraph::getThreadEntry(ThreadID tid) {
  auto it = m_thread_entry_nodes.find(tid);
  return it != m_thread_entry_nodes.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getThreadExit(ThreadID tid) {
  auto it = m_thread_exit_nodes.find(tid);
  return it != m_thread_exit_nodes.end() ? it->second : nullptr;
}

void ThreadFlowGraph::addIntraThreadEdge(SyncNode *from, SyncNode *to) {
  if (from && to) {
    from->addSuccessor(to);
    to->addPredecessor(from);
  }
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to) {
  // Inter-thread edges are also represented as regular edges
  addIntraThreadEdge(from, to);
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodesOfType(SyncNodeType type) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getType() == type) {
      result.push_back(node);
    }
  }
  return result;
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodesInThread(ThreadID tid) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getThreadID() == tid) {
      result.push_back(node);
    }
  }
  return result;
}

void ThreadFlowGraph::print(raw_ostream &os) const {
  os << "Thread Flow Graph:\n";
  os << "==================\n";
  os << "Total Nodes: " << m_all_nodes.size() << "\n";
  os << "Total Threads: " << m_thread_entries.size() << "\n\n";

  for (auto *node : m_all_nodes) {
    node->print(os);
    os << "\n";

    if (!node->getSuccessors().empty()) {
      os << "  Successors: ";
      for (auto *succ : node->getSuccessors()) {
        os << succ->getNodeID() << " ";
      }
      os << "\n";
    }
  }
}

void ThreadFlowGraph::printAsDot(raw_ostream &os) const {
  os << "digraph ThreadFlowGraph {\n";
  os << "  rankdir=TB;\n";
  os << "  node [shape=box];\n\n";

  // Define nodes
  for (auto *node : m_all_nodes) {
    os << "  node" << node->getNodeID() << " [label=\"";
    os << "ID:" << node->getNodeID() << "\\n";
    os << "T:" << node->getThreadID() << "\\n";
    os << getSyncNodeTypeName(node->getType());
    os << "\"];\n";
  }

  os << "\n";

  // Define edges
  for (auto *node : m_all_nodes) {
    for (auto *succ : node->getSuccessors()) {
      os << "  node" << node->getNodeID() << " -> node" << succ->getNodeID();

      // Different colors for different edge types
      if (node->getThreadID() != succ->getThreadID()) {
        os << " [color=red, style=dashed]"; // Inter-thread edge
      } else if (isSynchronizationNode(node->getType())) {
        os << " [color=blue]"; // Synchronization edge
      }

      os << ";\n";
    }
  }

  os << "}\n";
}

void ThreadFlowGraph::dumpToFile(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  printAsDot(file);
  file.close();
}

// ============================================================================
// ThreadRegionAnalysis Implementation
// ============================================================================

void ThreadRegionAnalysis::analyze() {
  identifyRegions();
  computeOrderingConstraints();
  computeParallelism();
}

const ThreadRegionAnalysis::Region *
ThreadRegionAnalysis::getRegion(size_t region_id) const {
  if (region_id < m_regions.size()) {
    return m_regions[region_id].get();
  }
  return nullptr;
}

const ThreadRegionAnalysis::Region *
ThreadRegionAnalysis::getRegionContaining(const Instruction *inst) const {
  auto it = m_inst_to_region.find(inst);
  return it != m_inst_to_region.end() ? it->second : nullptr;
}

void ThreadRegionAnalysis::identifyRegions() {
  auto threads = m_tfg.getAllThreads();

  for (ThreadID tid : threads) {
    auto nodes = m_tfg.getNodesInThread(tid);

    SyncNode *region_start = nullptr;
    std::vector<SyncNode *> region_nodes;

    for (auto *node : nodes) {
      // Start a new region if we haven't started one
      if (!region_start) {
        region_start = node;
      }

      region_nodes.push_back(node);

      // End region at synchronization boundaries
      if (isSynchronizationNode(node->getType()) ||
          isThreadBoundaryNode(node->getType())) {

        // Create region
        auto region = std::make_unique<Region>();
        region->region_id = m_regions.size();
        region->thread_id = tid;
        region->start_node = region_start;
        region->end_node = node;

        // Collect instructions
        for (auto *n : region_nodes) {
          if (n->getInstruction()) {
            region->instructions.insert(n->getInstruction());
            m_inst_to_region[n->getInstruction()] = region.get();
          }
        }

        m_regions.push_back(std::move(region));

        // Reset for next region
        region_start = nullptr;
        region_nodes.clear();
      }
    }

    // Handle remaining nodes
    if (region_start) {
      auto region = std::make_unique<Region>();
      region->region_id = m_regions.size();
      region->thread_id = tid;
      region->start_node = region_start;
      region->end_node = region_nodes.empty() ? region_start : region_nodes.back();

      for (auto *n : region_nodes) {
        if (n->getInstruction()) {
          region->instructions.insert(n->getInstruction());
          m_inst_to_region[n->getInstruction()] = region.get();
        }
      }

      m_regions.push_back(std::move(region));
    }
  }
}

void ThreadRegionAnalysis::computeOrderingConstraints() {
  // Compute must-precede and must-follow relationships based on:
  // 1. Intra-thread control flow
  // 2. Fork-join relationships
  // 3. Lock ordering

  for (size_t i = 0; i < m_regions.size(); ++i) {
    auto &region_i = m_regions[i];

    for (size_t j = 0; j < m_regions.size(); ++j) {
      if (i == j)
        continue;

      auto &region_j = m_regions[j];

      // Same thread: check control flow order
      if (region_i->thread_id == region_j->thread_id) {
        if (region_i->region_id < region_j->region_id) {
          region_i->must_precede.insert(j);
          region_j->must_follow.insert(i);
        }
      }

      // Different threads: check synchronization
      
      // Fork-join ordering: 
      // 1. Fork node must precede child thread entry
      // 2. Child thread exit must precede join node
      
      // Check if region_i contains a fork that creates region_j's thread
      for (const Instruction *inst : region_i->instructions) {
        SyncNode *node = m_tfg.getNode(inst);
        if (node && node->getType() == SyncNodeType::THREAD_FORK) {
          if (node->getForkedThread() == region_j->thread_id) {
            // Fork must precede everything in child thread
            region_i->must_precede.insert(j);
            region_j->must_follow.insert(i);
          }
        }
      }
      
      // Check if region_i is in a child thread and region_j contains the join
      for (const Instruction *inst : region_j->instructions) {
        SyncNode *node = m_tfg.getNode(inst);
        if (node && node->getType() == SyncNodeType::THREAD_JOIN) {
          if (node->getJoinedThread() == region_i->thread_id) {
            // Child thread must precede join
            region_i->must_precede.insert(j);
            region_j->must_follow.insert(i);
          }
        }
      }
      
      // Note: Lock-based ordering is complex and would require tracking
      // which lock acquisition happens first in the global execution.
      // This would need a more sophisticated lock order analysis.
      // For now, we rely on the LockSetAnalysis to identify conflicts.
    }
  }
}

void ThreadRegionAnalysis::computeParallelism() {
  // Two regions may run in parallel if:
  // 1. They are in different threads
  // 2. Neither must precede the other
  // 3. They don't hold conflicting locks

  for (size_t i = 0; i < m_regions.size(); ++i) {
    auto &region_i = m_regions[i];

    for (size_t j = i + 1; j < m_regions.size(); ++j) {
      auto &region_j = m_regions[j];

      // Same thread => not parallel
      if (region_i->thread_id == region_j->thread_id) {
        continue;
      }

      // Check ordering constraints
      if (region_i->must_precede.find(j) != region_i->must_precede.end() ||
          region_i->must_follow.find(j) != region_i->must_follow.end()) {
        continue;
      }

      // May be parallel
      region_i->may_be_parallel.insert(j);
      region_j->may_be_parallel.insert(i);
    }
  }
}

void ThreadRegionAnalysis::print(raw_ostream &os) const {
  os << "Thread Region Analysis Results:\n";
  os << "================================\n";
  os << "Total Regions: " << m_regions.size() << "\n\n";

  for (const auto &region : m_regions) {
    os << "Region " << region->region_id << " (Thread " << region->thread_id
       << "):\n";
    os << "  Instructions: " << region->instructions.size() << "\n";
    os << "  Must Precede: {";
    bool first = true;
    for (auto r : region->must_precede) {
      if (!first)
        os << ", ";
      os << r;
      first = false;
    }
    os << "}\n";

    os << "  May Be Parallel: {";
    first = true;
    for (auto r : region->may_be_parallel) {
      if (!first)
        os << ", ";
      os << r;
      first = false;
    }
    os << "}\n\n";
  }
}

// ============================================================================
// MHPAnalysis Implementation
// ============================================================================

MHPAnalysis::MHPAnalysis(Module &module)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
}

void MHPAnalysis::analyze() {
  errs() << "Starting MHP Analysis...\n";

  buildThreadFlowGraph();
  
  // Optional: LockSet analysis for more precise reasoning
  if (m_enable_lockset_analysis) {
    analyzeLockSets();
  }
  
  analyzeThreadRegions();
  computeMHPPairs();

  errs() << "MHP Analysis Complete!\n";
}

void MHPAnalysis::enableLockSetAnalysis() {
  m_enable_lockset_analysis = true;
}

void MHPAnalysis::buildThreadFlowGraph() {
  errs() << "Building Thread Flow Graph...\n";

  // Find main function
  Function *main_func = m_module.getFunction("main");
  if (!main_func) {
    errs() << "Warning: No main function found\n";
    return;
  }

  // Main thread (thread 0)
  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0);

  errs() << "Thread Flow Graph built with " << m_tfg->getAllNodes().size()
         << " nodes\n";
}

void MHPAnalysis::processFunction(const Function *func, ThreadID tid) {
  if (!func || func->isDeclaration())
    return;

  SyncNode *prev_node = nullptr;
  SyncNode *entry_node = nullptr;

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);

      SyncNode *node = nullptr;

      // Check if this is a thread API call
      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        if (m_thread_api->isTDFork(&inst)) {
          node = m_tfg->createNode(&inst, SyncNodeType::THREAD_FORK, tid);
          handleThreadFork(&inst, node);
        } else if (m_thread_api->isTDJoin(&inst)) {
          node = m_tfg->createNode(&inst, SyncNodeType::THREAD_JOIN, tid);
          handleThreadJoin(&inst, node);
        } else if (m_thread_api->isTDAcquire(&inst)) {
          node = m_tfg->createNode(&inst, SyncNodeType::LOCK_ACQUIRE, tid);
          handleLockAcquire(&inst, node);
        } else if (m_thread_api->isTDRelease(&inst)) {
          node = m_tfg->createNode(&inst, SyncNodeType::LOCK_RELEASE, tid);
          handleLockRelease(&inst, node);
        } else if (m_thread_api->isTDExit(&inst)) {
          node = m_tfg->createNode(&inst, SyncNodeType::THREAD_EXIT, tid);
        } else if (m_thread_api->isTDBarWait(&inst)) {
          node = m_tfg->createNode(&inst, SyncNodeType::BARRIER_WAIT, tid);
          handleBarrier(&inst, node);
        }
      }

      // Regular instruction
      if (!node) {
        node = m_tfg->createNode(&inst, SyncNodeType::REGULAR_INST, tid);
      }

      // Link to previous node
      if (prev_node) {
        m_tfg->addIntraThreadEdge(prev_node, node);
      }

      if (!entry_node) {
        entry_node = node;
        m_tfg->setThreadEntry(tid, entry_node);
      }

      prev_node = node;
    }
  }

  if (prev_node) {
    m_tfg->setThreadExit(tid, prev_node);
  }
}

void MHPAnalysis::processInstruction(const Instruction * /*inst*/, ThreadID /*tid*/,
                                      SyncNode *& /*current_node*/) {
  // This method is a helper for more fine-grained processing if needed
  // Currently unused but kept for future extensibility
}

void MHPAnalysis::handleThreadFork(const Instruction *fork_inst,
                                    SyncNode *node) {
  // Allocate new thread ID
  ThreadID new_tid = allocateThreadID();
  ThreadID parent_tid = getThreadID(fork_inst);
  
  node->setForkedThread(new_tid);

  // Track fork-join relationships
  m_thread_fork_sites[new_tid] = fork_inst;
  m_thread_parents[new_tid] = parent_tid;
  m_thread_children[parent_tid].push_back(new_tid);
  m_fork_to_thread[fork_inst] = new_tid;

  // Track pthread_t value for this thread
  // The first argument to pthread_create is the pthread_t* where the thread ID is stored
  const Value *pthread_ptr = m_thread_api->getForkedThread(fork_inst);
  if (pthread_ptr) {
    // Map this pthread_t pointer to the thread ID
    // We need to track both the pointer and any loads from it
    m_pthread_value_to_thread[pthread_ptr] = new_tid;
    m_thread_to_pthread_value[new_tid] = pthread_ptr;
    
    // Also track the store if it exists (for later load tracking)
    // In a more sophisticated implementation, we'd do def-use chain analysis
  }

  // Get the forked function
  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(forked_fun_val)) {
    m_tfg->addThread(new_tid, forked_fun);

    // Process the forked function
    processFunction(forked_fun, new_tid);

    // Add inter-thread edge from fork to new thread entry
    SyncNode *new_thread_entry = m_tfg->getThreadEntry(new_tid);
    if (new_thread_entry) {
      m_tfg->addInterThreadEdge(node, new_thread_entry);
    }
  }
}

void MHPAnalysis::handleThreadJoin(const Instruction *join_inst,
                                    SyncNode *node) {
  // Track which thread is being joined using value analysis
  // pthread_join takes the pthread_t value (not pointer) as first argument
  
  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found_thread = false;
  
  if (joined_thread_val) {
    // Case 1: Direct match - the value is the pthread_t pointer we tracked
    auto it = m_pthread_value_to_thread.find(joined_thread_val);
    if (it != m_pthread_value_to_thread.end()) {
      joined_tid = it->second;
      found_thread = true;
    } else {
      // Case 2: Value comes from a load instruction
      // Track back through loads to find the original pthread_t pointer
      if (const LoadInst *load = dyn_cast<LoadInst>(joined_thread_val)) {
        const Value *loaded_from = load->getPointerOperand();
        auto it2 = m_pthread_value_to_thread.find(loaded_from);
        if (it2 != m_pthread_value_to_thread.end()) {
          joined_tid = it2->second;
          found_thread = true;
          // Update our tracking to include this load
          m_pthread_value_to_thread[joined_thread_val] = joined_tid;
        }
      }
      
      // Case 3: Check if it's a bitcast or other simple transformation
      const Value *stripped = joined_thread_val->stripPointerCasts();
      if (stripped != joined_thread_val) {
        auto it3 = m_pthread_value_to_thread.find(stripped);
        if (it3 != m_pthread_value_to_thread.end()) {
          joined_tid = it3->second;
          found_thread = true;
          m_pthread_value_to_thread[joined_thread_val] = joined_tid;
        }
      }
    }
  }
  
  if (found_thread && joined_tid != 0) {
    // We successfully identified the joined thread
    SyncNode *child_exit = m_tfg->getThreadExit(joined_tid);
    if (child_exit) {
      m_tfg->addInterThreadEdge(child_exit, node);
      node->setJoinedThread(joined_tid);
      m_join_to_thread[join_inst] = joined_tid;
    }
  } else {
    // Fallback: couldn't determine specific thread, so conservatively
    // assume it could be any child thread of the current thread
    ThreadID parent_tid = getThreadID(join_inst);
    
    if (m_thread_children.find(parent_tid) != m_thread_children.end()) {
      for (ThreadID child_tid : m_thread_children[parent_tid]) {
        SyncNode *child_exit = m_tfg->getThreadExit(child_tid);
        if (child_exit) {
          m_tfg->addInterThreadEdge(child_exit, node);
          // Note: We set joined thread even in fallback case
          // In reality, only one thread is joined, but we're conservative
          node->setJoinedThread(child_tid);
          m_join_to_thread[join_inst] = child_tid;
        }
      }
    }
  }
}

void MHPAnalysis::handleLockAcquire(const Instruction *lock_inst,
                                     SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(lock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleLockRelease(const Instruction *unlock_inst,
                                     SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(unlock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleCondWait(const Instruction * /*wait_inst*/,
                                  SyncNode * /*node*/) {
  // Condition variable handling
  // TODO: Implement condition variable synchronization analysis
}

void MHPAnalysis::handleCondSignal(const Instruction * /*signal_inst*/,
                                    SyncNode * /*node*/) {
  // Condition variable handling
  // TODO: Implement condition variable synchronization analysis
}

void MHPAnalysis::handleBarrier(const Instruction * /*barrier_inst*/,
                                 SyncNode * /*node*/) {
  // Barrier synchronization handling
  // TODO: Implement barrier synchronization analysis
}

void MHPAnalysis::analyzeLockSets() {
  errs() << "Analyzing Lock Sets...\n";
  m_lockset = std::make_unique<LockSetAnalysis>(m_module);
  m_lockset->analyze();
}

void MHPAnalysis::analyzeThreadRegions() {
  errs() << "Analyzing Thread Regions...\n";
  m_region_analysis = std::make_unique<ThreadRegionAnalysis>(*m_tfg);
  m_region_analysis->analyze();
  errs() << "Identified " << m_region_analysis->getAllRegions().size()
         << " regions\n";
}

void MHPAnalysis::computeMHPPairs() {
  errs() << "Computing MHP Pairs...\n";

  // For each pair of instructions, check if they may run in parallel
  std::vector<const Instruction *> all_insts;

  for (Function &func : m_module) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      all_insts.push_back(&*I);
    }
  }

  size_t num_pairs = 0;
  for (size_t i = 0; i < all_insts.size(); ++i) {
    for (size_t j = i + 1; j < all_insts.size(); ++j) {
      const Instruction *i1 = all_insts[i];
      const Instruction *i2 = all_insts[j];

      // Skip if in same thread and ordered
      if (isInSameThread(i1, i2)) {
        continue;
      }

      // Check if they may happen in parallel
      if (!hasHappenBeforeRelation(i1, i2) &&
          !hasHappenBeforeRelation(i2, i1)) {
        m_mhp_pairs.insert({i1, i2});
        num_pairs++;
      }
    }
  }

  errs() << "Found " << num_pairs << " MHP pairs\n";
}

bool MHPAnalysis::mayHappenInParallel(const Instruction *i1,
                                       const Instruction *i2) const {
  // Quick check: same instruction
  if (i1 == i2)
    return false;

  // Check if in same thread
  if (isInSameThread(i1, i2))
    return false;

  // Check cached results
  if (m_mhp_pairs.find({i1, i2}) != m_mhp_pairs.end() ||
      m_mhp_pairs.find({i2, i1}) != m_mhp_pairs.end()) {
    return true;
  }

  // Check happens-before relation
  if (hasHappenBeforeRelation(i1, i2) || hasHappenBeforeRelation(i2, i1)) {
    return false;
  }

  // Check lock-based ordering
  if (isOrderedByLocks(i1, i2)) {
    return false;
  }

  return true;
}

InstructionSet
MHPAnalysis::getParallelInstructions(const Instruction *inst) const {
  InstructionSet result;

  for (const auto &pair : m_mhp_pairs) {
    if (pair.first == inst) {
      result.insert(pair.second);
    } else if (pair.second == inst) {
      result.insert(pair.first);
    }
  }

  return result;
}

bool MHPAnalysis::mustBeSequential(const Instruction *i1,
                                    const Instruction *i2) const {
  return !mayHappenInParallel(i1, i2);
}

bool MHPAnalysis::mustPrecede(const Instruction *i1,
                               const Instruction *i2) const {
  return hasHappenBeforeRelation(i1, i2);
}

ThreadID MHPAnalysis::getThreadID(const Instruction *inst) const {
  auto it = m_inst_to_thread.find(inst);
  return it != m_inst_to_thread.end() ? it->second : 0;
}

InstructionSet MHPAnalysis::getInstructionsInThread(ThreadID tid) const {
  InstructionSet result;
  for (const auto &pair : m_inst_to_thread) {
    if (pair.second == tid) {
      result.insert(pair.first);
    }
  }
  return result;
}

std::set<LockID> MHPAnalysis::getLocksHeldAt(const Instruction *inst) const {
  // LockSet analysis is optional - only available if enabled
  if (m_lockset) {
    return m_lockset->getMayLockSetAt(inst);
  }
  // If lockset analysis not run, return empty set
  return std::set<LockID>();
}

ThreadID MHPAnalysis::allocateThreadID() { return m_next_thread_id++; }

void MHPAnalysis::mapInstructionToThread(const Instruction *inst,
                                          ThreadID tid) {
  m_inst_to_thread[inst] = tid;
}

bool MHPAnalysis::hasHappenBeforeRelation(const Instruction *i1,
                                           const Instruction *i2) const {
  // Check various happens-before relations:
  // 1. Same thread, program order
  // 2. Fork-join ordering
  // 3. Lock-based ordering

  if (isInSameThread(i1, i2)) {
    // In same thread, check program order
    const Function *func = i1->getFunction();
    for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
         ++I) {
      if (&*I == i1)
        return true;
      if (&*I == i2)
        return false;
    }
  }

  if (isOrderedByForkJoin(i1, i2))
    return true;

  if (isOrderedByLocks(i1, i2))
    return true;

  return false;
}

bool MHPAnalysis::isInSameThread(const Instruction *i1,
                                  const Instruction *i2) const {
  return getThreadID(i1) == getThreadID(i2);
}

bool MHPAnalysis::isOrderedByLocks(const Instruction *i1,
                                    const Instruction *i2) const {
  if (!m_lockset)
    return false;

  // If they hold a common lock in different threads, they're ordered
  if (!isInSameThread(i1, i2)) {
    return m_lockset->mayHoldCommonLock(i1, i2);
  }

  return false;
}

bool MHPAnalysis::isOrderedByForkJoin(const Instruction *i1,
                                       const Instruction *i2) const {
  ThreadID tid1 = getThreadID(i1);
  ThreadID tid2 = getThreadID(i2);
  
  if (tid1 == tid2) {
    return false; // Same thread, use program order instead
  }
  
  // Check if i1's thread is an ancestor of i2's thread
  // If so, and i1 is before the fork, then i1 happens-before i2
  
  // Check if tid2 is a descendant of tid1
  ThreadID current = tid2;
  while (m_thread_parents.find(current) != m_thread_parents.end()) {
    ThreadID parent = m_thread_parents.at(current);
    if (parent == tid1) {
      // tid1 is ancestor of tid2
      // Check if i1 comes before the fork site
      const Instruction *fork_site = m_thread_fork_sites.at(current);
      
      // Simple check: if i1 is in same function as fork and comes before it
      if (i1->getFunction() == fork_site->getFunction()) {
        // Check program order in same function
        for (const_inst_iterator I = inst_begin(i1->getFunction()),
                                 E = inst_end(i1->getFunction());
             I != E; ++I) {
          if (&*I == i1)
            return true; // i1 before fork
          if (&*I == fork_site)
            return false; // fork before i1
        }
      }
    }
    current = parent;
  }
  
  // Check if i2's thread has been joined before i1
  // This requires checking if there's a join for tid2 that happens before i1
  for (const auto &pair : m_join_to_thread) {
    if (pair.second == tid2) {
      const Instruction *join_inst = pair.first;
      if (getThreadID(join_inst) == tid1) {
        // Join is in same thread as i1
        // Check if join comes before i1
        if (join_inst->getFunction() == i1->getFunction()) {
          for (const_inst_iterator I = inst_begin(i1->getFunction()),
                                   E = inst_end(i1->getFunction());
               I != E; ++I) {
            if (&*I == join_inst)
              return true; // join before i1, so tid2 before i1
            if (&*I == i1)
              return false; // i1 before join
          }
        }
      }
    }
  }
  
  return false;
}

// ============================================================================
// Fork-Join Helper Methods
// ============================================================================

bool MHPAnalysis::isAncestorThread(ThreadID ancestor,
                                    ThreadID descendant) const {
  ThreadID current = descendant;
  while (m_thread_parents.find(current) != m_thread_parents.end()) {
    ThreadID parent = m_thread_parents.at(current);
    if (parent == ancestor) {
      return true;
    }
    current = parent;
  }
  return false;
}

bool MHPAnalysis::isForkSite(const Instruction *inst) const {
  return m_fork_to_thread.find(inst) != m_fork_to_thread.end();
}

bool MHPAnalysis::isJoinSite(const Instruction *inst) const {
  return m_join_to_thread.find(inst) != m_join_to_thread.end();
}

ThreadID MHPAnalysis::getForkedThreadID(const Instruction *fork_inst) const {
  auto it = m_fork_to_thread.find(fork_inst);
  return it != m_fork_to_thread.end() ? it->second : 0;
}

ThreadID MHPAnalysis::getJoinedThreadID(const Instruction *join_inst) const {
  auto it = m_join_to_thread.find(join_inst);
  return it != m_join_to_thread.end() ? it->second : 0;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void MHPAnalysis::Statistics::print(raw_ostream &os) const {
  os << "MHP Analysis Statistics:\n";
  os << "========================\n";
  os << "Threads:          " << num_threads << "\n";
  os << "Forks:            " << num_forks << "\n";
  os << "Joins:            " << num_joins << "\n";
  os << "Locks:            " << num_locks << "\n";
  os << "Unlocks:          " << num_unlocks << "\n";
  os << "Regions:          " << num_regions << "\n";
  os << "MHP Pairs:        " << num_mhp_pairs << "\n";
  os << "Ordered Pairs:    " << num_ordered_pairs << "\n";
}

MHPAnalysis::Statistics MHPAnalysis::getStatistics() const {
  Statistics stats;

  if (m_tfg) {
    stats.num_threads = m_tfg->getAllThreads().size();
    stats.num_forks = m_tfg->getNodesOfType(SyncNodeType::THREAD_FORK).size();
    stats.num_joins = m_tfg->getNodesOfType(SyncNodeType::THREAD_JOIN).size();
    stats.num_locks =
        m_tfg->getNodesOfType(SyncNodeType::LOCK_ACQUIRE).size();
    stats.num_unlocks =
        m_tfg->getNodesOfType(SyncNodeType::LOCK_RELEASE).size();
  }

  if (m_region_analysis) {
    stats.num_regions = m_region_analysis->getAllRegions().size();
  }

  stats.num_mhp_pairs = m_mhp_pairs.size();

  return stats;
}

void MHPAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void MHPAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== MHP Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== Thread Flow Graph ===\n";
  if (m_tfg) {
    m_tfg->print(os);
  }

  os << "\n=== Thread Region Analysis ===\n";
  if (m_region_analysis) {
    m_region_analysis->print(os);
  }

  // Optional: Lock Set Analysis (only if enabled)
  if (m_lockset) {
    os << "\n=== Lock Set Analysis ===\n";
    m_lockset->print(os);
  }

  os << "\n=== MHP Pairs (sample) ===\n";
  size_t count = 0;
  for (const auto &pair : m_mhp_pairs) {
    os << "MHP: ";
    pair.first->print(os);
    os << " ||| ";
    pair.second->print(os);
    os << "\n";

    if (++count >= 20) {
      os << "... (" << (m_mhp_pairs.size() - 20) << " more pairs)\n";
      break;
    }
  }
}

void MHPAnalysis::dumpThreadFlowGraph(const std::string &filename) const {
  if (m_tfg) {
    m_tfg->dumpToFile(filename);
    errs() << "Thread flow graph dumped to " << filename << "\n";
  }
}

void MHPAnalysis::dumpMHPMatrix(raw_ostream &os) const {
  os << "MHP Matrix:\n";
  os << "===========\n";
  // Matrix visualization would go here
  // This is a placeholder for a more sophisticated visualization
}

// ============================================================================
// Utility Functions
// ============================================================================

namespace mhp {

StringRef getSyncNodeTypeName(SyncNodeType type) {
  switch (type) {
  case SyncNodeType::THREAD_START:
    return "THREAD_START";
  case SyncNodeType::THREAD_FORK:
    return "THREAD_FORK";
  case SyncNodeType::THREAD_JOIN:
    return "THREAD_JOIN";
  case SyncNodeType::THREAD_EXIT:
    return "THREAD_EXIT";
  case SyncNodeType::LOCK_ACQUIRE:
    return "LOCK_ACQUIRE";
  case SyncNodeType::LOCK_RELEASE:
    return "LOCK_RELEASE";
  case SyncNodeType::COND_WAIT:
    return "COND_WAIT";
  case SyncNodeType::COND_SIGNAL:
    return "COND_SIGNAL";
  case SyncNodeType::COND_BROADCAST:
    return "COND_BROADCAST";
  case SyncNodeType::BARRIER_WAIT:
    return "BARRIER_WAIT";
  case SyncNodeType::REGULAR_INST:
    return "REGULAR_INST";
  case SyncNodeType::FUNCTION_CALL:
    return "FUNCTION_CALL";
  case SyncNodeType::FUNCTION_RETURN:
    return "FUNCTION_RETURN";
  }
  return "UNKNOWN";
}

bool isSynchronizationNode(SyncNodeType type) {
  return type == SyncNodeType::LOCK_ACQUIRE ||
         type == SyncNodeType::LOCK_RELEASE ||
         type == SyncNodeType::COND_WAIT ||
         type == SyncNodeType::COND_SIGNAL ||
         type == SyncNodeType::COND_BROADCAST ||
         type == SyncNodeType::BARRIER_WAIT;
}

bool isThreadBoundaryNode(SyncNodeType type) {
  return type == SyncNodeType::THREAD_START ||
         type == SyncNodeType::THREAD_FORK ||
         type == SyncNodeType::THREAD_JOIN ||
         type == SyncNodeType::THREAD_EXIT;
}

} // namespace mhp
