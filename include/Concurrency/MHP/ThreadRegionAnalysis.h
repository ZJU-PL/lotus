/**
 * @file ThreadRegionAnalysis.h
 * @brief Divides threads into synchronization-delimited execution regions.
 */

#pragma once

#include "Concurrency/Core/ThreadFlowGraph.h"
#include "Concurrency/MHP/IMHPAnalysis.h"

#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace mhp {

using RegionBitVector = llvm::BitVector;

/**
 * @brief Divides a program into thread regions based on synchronization.
 *
 * A thread region is a maximal sequence of instructions within a single thread
 * that are not separated by synchronization operations.
 */
class ThreadRegionAnalysis {
public:
  struct Region {
    size_t region_id;
    ThreadID thread_id;
    SyncNode *start_node;
    SyncNode *end_node;
    InstructionSet instructions;

    // Synchronization constraints using bitvectors for efficiency
    RegionBitVector must_precede_bits;
    RegionBitVector must_follow_bits;
    RegionBitVector may_be_parallel_bits;

    // Set views for direct iteration
    std::set<size_t> must_precede;
    std::set<size_t> must_follow;
    std::set<size_t> may_be_parallel;
  };

  explicit ThreadRegionAnalysis(const ThreadFlowGraph &tfg) : m_tfg(tfg) {}

  void analyze();

  const Region *getRegion(size_t region_id) const;
  const Region *getRegionContaining(const llvm::Instruction *inst) const;
  const std::vector<std::unique_ptr<Region>> &getAllRegions() const {
    return m_regions;
  }

  void print(llvm::raw_ostream &os) const;

private:
  const ThreadFlowGraph &m_tfg;

  std::vector<std::unique_ptr<Region>> m_regions;
  std::unordered_map<const llvm::Instruction *, const Region *>
      m_inst_to_region;

  void identifyRegions();
  void identifyRegionsForThread(ThreadID tid, const llvm::Function *entry);
  void computeOrderingConstraints();
  void computeParallelism();

  bool isSyncPoint(const llvm::Instruction *inst) const;
  std::vector<const llvm::Instruction *>
  collectSyncPoints(const llvm::Function *func) const;
  bool isReachableInTFG(const SyncNode *from, const SyncNode *to) const;
};

} // namespace mhp
