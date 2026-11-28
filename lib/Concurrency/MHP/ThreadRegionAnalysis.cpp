/**
 * @file ThreadRegionAnalysis.cpp
 * @brief Implementation of synchronization-delimited thread region analysis.
 */

#include "Concurrency/MHP/ThreadRegionAnalysis.h"

#include <algorithm>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>

using namespace llvm;

namespace mhp {

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

bool ThreadRegionAnalysis::isSyncPoint(const Instruction *inst) const {
  for (SyncNode *node : m_tfg.getNodes(inst)) {
    SyncNodeType type = node->getType();
    if (isSynchronizationNode(type) || isThreadBoundaryNode(type)) {
      return true;
    }
  }
  return false;
}

std::vector<const Instruction *>
ThreadRegionAnalysis::collectSyncPoints(const Function *func) const {
  std::vector<const Instruction *> sync_points;

  for (const BasicBlock &BB : *func) {
    for (const Instruction &inst : BB) {
      if (isSyncPoint(&inst)) {
        sync_points.push_back(&inst);
      }
    }
  }

  return sync_points;
}

void ThreadRegionAnalysis::identifyRegions() {
  auto threads = m_tfg.getAllThreads();

  for (ThreadID tid : threads) {
    const Function *entry = m_tfg.getThreadEntry(tid);
    if (entry && !entry->isDeclaration()) {
      identifyRegionsForThread(tid, entry);
    }
  }
}

void ThreadRegionAnalysis::identifyRegionsForThread(ThreadID tid,
                                                    const Function *func) {
  if (!func || func->isDeclaration())
    return;

  auto flush_region = [&](std::unique_ptr<Region> &region) {
    if (!region || region->instructions.empty()) {
      return;
    }
    for (const Instruction *inst : region->instructions) {
      m_inst_to_region[inst] = region.get();
    }
    m_regions.push_back(std::move(region));
  };

  auto make_region = [&](SyncNode *start_node) {
    auto region = std::make_unique<Region>();
    region->region_id = m_regions.size();
    region->thread_id = tid;
    region->start_node = start_node;
    region->end_node = nullptr;
    return region;
  };

  SyncNode *pending_start = nullptr;
  std::unique_ptr<Region> region;
  const std::vector<SyncNode *> &thread_nodes =
      m_tfg.getTopologicalOrderNodes(tid);

  for (SyncNode *node : thread_nodes) {
    if (!node || node->getThreadID() != tid) {
      continue;
    }
    const Instruction *inst = node->getInstruction();
    if (!inst) {
      continue;
    }

    if (!region) {
      region = make_region(pending_start);
      pending_start = nullptr;
    }

    region->instructions.insert(inst);

    if (!(isSynchronizationNode(node->getType()) ||
          isThreadBoundaryNode(node->getType()))) {
      continue;
    }

    region->end_node = node;
    flush_region(region);
    pending_start = node;
  }

  if (region) {
    flush_region(region);
  }
}

void ThreadRegionAnalysis::computeOrderingConstraints() {
  const size_t num_regions = m_regions.size();
  if (num_regions == 0) {
    return;
  }

  for (const auto &region : m_regions) {
    region->must_precede_bits.resize(num_regions);
    region->must_follow_bits.resize(num_regions);
    region->may_be_parallel_bits.resize(num_regions);
  }

  std::unordered_map<const SyncNode *, SmallVector<size_t, 2>>
      start_node_to_regions;
  std::unordered_map<ThreadID, std::vector<size_t>> regions_by_thread;
  for (size_t i = 0; i < num_regions; ++i) {
    regions_by_thread[m_regions[i]->thread_id].push_back(i);
    if (m_regions[i]->start_node) {
      start_node_to_regions[m_regions[i]->start_node].push_back(i);
    }
  }

  std::vector<uint32_t> visit_stamp(m_tfg.getMaxNodeID() + 1, 0);
  uint32_t current_stamp = 0;
  std::vector<const SyncNode *> queue;
  std::unordered_map<const SyncNode *, std::vector<size_t>>
      reachable_regions_by_source;

  for (size_t i = 0; i < num_regions; ++i) {
    Region &region_i = *m_regions[i];
    const SyncNode *source = region_i.end_node;
    if (source) {
      auto cached = reachable_regions_by_source.find(source);
      if (cached == reachable_regions_by_source.end()) {
        std::vector<size_t> reached;
        ++current_stamp;
        if (current_stamp == 0) {
          std::fill(visit_stamp.begin(), visit_stamp.end(), 0);
          current_stamp = 1;
        }
        queue.clear();
        const size_t src_id = source->getNodeID();
        if (src_id < visit_stamp.size()) {
          visit_stamp[src_id] = current_stamp;
        }
        queue.push_back(source);
        size_t head = 0;
        const ThreadID src_tid = source->getThreadID();
        while (head < queue.size()) {
          const SyncNode *current = queue[head++];
          auto start_it = start_node_to_regions.find(current);
          if (start_it != start_node_to_regions.end()) {
            for (size_t target_region : start_it->second) {
              if (m_regions[target_region]->thread_id == src_tid) {
                reached.push_back(target_region);
              }
            }
          }
          for (SyncNode *succ : current->getSuccessors()) {
            if (succ->getThreadID() != src_tid) {
              continue;
            }
            const size_t succ_id = succ->getNodeID();
            if (succ_id < visit_stamp.size() &&
                visit_stamp[succ_id] != current_stamp) {
              visit_stamp[succ_id] = current_stamp;
              queue.push_back(succ);
            }
          }
        }
        cached = reachable_regions_by_source
                     .emplace(source, std::move(reached))
                     .first;
      }

      for (size_t j : cached->second) {
        if (j != i) {
          region_i.must_precede.insert(j);
          m_regions[j]->must_follow.insert(i);
        }
      }

      // Case 1: Fork ordering
      if (source->getType() == SyncNodeType::THREAD_FORK) {
        ThreadID forked_tid = source->getForkedThread();
        auto tid_it = regions_by_thread.find(forked_tid);
        if (forked_tid != region_i.thread_id &&
            tid_it != regions_by_thread.end()) {
          for (size_t j : tid_it->second) {
            region_i.must_precede.insert(j);
            m_regions[j]->must_follow.insert(i);
          }
        }
      }
    }

    // Case 2: Join ordering
    if (region_i.start_node &&
        region_i.start_node->getType() == SyncNodeType::THREAD_JOIN) {
      ThreadID joined_tid = region_i.start_node->getJoinedThread();
      auto tid_it = regions_by_thread.find(joined_tid);
      if (joined_tid != region_i.thread_id &&
          tid_it != regions_by_thread.end()) {
        for (size_t j : tid_it->second) {
          m_regions[j]->must_precede.insert(i);
          region_i.must_follow.insert(j);
        }
      }
    }
  }
}

bool ThreadRegionAnalysis::isReachableInTFG(const SyncNode *from,
                                            const SyncNode *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  return from->getThreadID() == to->getThreadID() && m_tfg.canReach(from, to);
}

void ThreadRegionAnalysis::computeParallelism() {
  for (size_t i = 0; i < m_regions.size(); ++i) {
    auto &region_i = m_regions[i];

    for (size_t j : region_i->must_precede) {
      region_i->must_precede_bits.set(j);
    }
    for (size_t j : region_i->must_follow) {
      region_i->must_follow_bits.set(j);
    }

    for (size_t j = i + 1; j < m_regions.size(); ++j) {
      auto &region_j = m_regions[j];

      if (region_i->thread_id == region_j->thread_id) {
        continue;
      }

      if (region_i->must_precede.find(j) != region_i->must_precede.end() ||
          region_i->must_follow.find(j) != region_i->must_follow.end()) {
        continue;
      }

      region_i->may_be_parallel.insert(j);
      region_j->may_be_parallel.insert(i);
      region_i->may_be_parallel_bits.set(j);
      region_j->may_be_parallel_bits.set(i);
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

} // namespace mhp
