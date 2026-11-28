#pragma once

#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/API/CppAtomics.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace concurrency {
class ConcurrencyContext;
} // namespace concurrency

namespace lotus {

class AliasAnalysisWrapper;

/**
 * Happens-before relation for race detection. HB is the union of:
 * - Program order (TFG): intra-thread and fork/join/lock/barrier edges.
 * - Synchronizes-with (m_sync_with): promise/future and selected modeled
 *   library/runtime edges.
 *
 * Atomic and fence synchronization edges are emitted only when the analysis can
 * prove a witness strong enough to avoid inventing reads-from relationships.
 * Remaining candidates are left in deferred counters with explicit reasons.
 */
class HappensBeforeAnalysis {
public:
  explicit HappensBeforeAnalysis(llvm::Module &module, const mhp::IMHPAnalysis &mhp);

  void analyze();

  void setContext(::concurrency::ConcurrencyContext *context) {
    m_context = context;
  }

  /**
   * @brief Set optional alias analysis for synchronizes-with same-location
   * check
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    m_alias_analysis = aa;
  }

  void setCppMemoryModel(CppAtomics::CppMemoryModel model) {
    m_cpp_memory_model = model;
  }

  const std::unordered_map<std::string, size_t> &getDeferredSyncCounts() const {
    return m_deferred_sync_counts;
  }
  const std::vector<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>> &
  getSynchronizesWithEdges() const {
    return m_sync_with;
  }

  /**
   * @brief Check if instruction A happens-before instruction B
   * Includes program order (TFG) and modeled synchronizes-with edges.
   * @param A The first instruction
   * @param B The second instruction
   * @return true if A happens-before B
   */
  bool happensBefore(const llvm::Instruction *A,
                     const llvm::Instruction *B) const;

  bool mustPrecede(const llvm::Instruction *A,
                   const llvm::Instruction *B) const {
    return happensBefore(A, B);
  }

private:
  struct InstPairHash {
    size_t operator()(const std::pair<const llvm::Instruction *,
                                      const llvm::Instruction *> &p) const {
      size_t h1 = std::hash<const llvm::Instruction *>()(p.first);
      size_t h2 = std::hash<const llvm::Instruction *>()(p.second);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };

  void buildSynchronizesWith();
  void computeAtomicHappensBefore();
  void buildHBReachabilityIndex();
  bool canThreadReachHB(mhp::ThreadID from_tid, mhp::ThreadID to_tid) const;
  int getHBTopologicalOrder(const mhp::SyncNode *node) const;
  size_t getHBSCCID(const mhp::SyncNode *node) const;
  bool hasProgramOrder(const llvm::Instruction *A,
                       const llvm::Instruction *B) const;
  bool canReach(const mhp::SyncNode *start, const mhp::SyncNode *end) const;
  bool canReachWithHB(const mhp::SyncNode *start,
                      const mhp::SyncNode *end) const;
  bool isInstructionThreadAmbiguous(const llvm::Instruction *inst) const;
  void addExtraHBEdge(const llvm::Instruction *from,
                      const llvm::Instruction *to);
  void addExtraHBEdge(const mhp::SyncNode *from, const mhp::SyncNode *to);
  std::vector<const llvm::Instruction *>
  collectThreadSuffixInstructions(const llvm::Instruction *inst) const;
  const llvm::Instruction *
  findNearestAtomicInBlock(const llvm::Instruction *inst, bool search_backward,
                           bool require_load_like,
                           bool require_store_like) const;
  bool isFenceAnchorCompatibleInstruction(const llvm::Instruction *inst) const;
  const llvm::PostDominatorTree &
  getPostDominatorTree(const llvm::Function *func) const;

  bool sameAtomicLocation(const llvm::Instruction *store_inst,
                          const llvm::Instruction *load_inst) const;
  const llvm::Instruction *
  getSinglePrecedingAtomicLoad(const llvm::Instruction *inst) const;
  const llvm::Instruction *
  getSinglePrecedingReleaseFence(const llvm::Instruction *inst) const;
  const llvm::Instruction *
  getSingleFollowingAcquireFence(const llvm::Instruction *inst) const;
  bool hasSupportedAtomicWitness(const llvm::Instruction *inst) const;

  /**
   * @brief Check if promise and future operate on the same shared state
   */
  bool samePromiseFuturePair(const llvm::Instruction *promise,
                             const llvm::Instruction *future) const;

  /**
   * @brief Check if two call_once calls use the same once_flag
   */
  bool sameOnceFlag(const llvm::Instruction *call1,
                    const llvm::Instruction *call2) const;

  /**
   * @brief Check if two latch operations use the same latch object
   */
  bool sameLatch(const llvm::Instruction *inst1,
                 const llvm::Instruction *inst2) const;

  /**
   * @brief Check if two barrier operations use the same barrier object
   */
  bool sameBarrier(const llvm::Instruction *inst1,
                   const llvm::Instruction *inst2) const;

  const llvm::Value *traceSharedState(const llvm::Value *value) const;

  llvm::Module &m_module;
  const mhp::IMHPAnalysis &m_mhp;
  ::concurrency::ConcurrencyContext *m_context = nullptr;
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  CppAtomics::CppMemoryModel m_cpp_memory_model =
      CppAtomics::CppMemoryModel::Cpp20AndLater;
  std::unordered_map<const llvm::Value *, const llvm::Value *>
      m_future_shared_state;
  mutable std::unordered_map<const llvm::Value *, const llvm::Value *>
      m_shared_state_trace_cache;
  std::unordered_map<std::string, size_t> m_deferred_sync_counts;
  std::vector<const llvm::Instruction *> m_atomic_instructions;
  std::unordered_map<const mhp::SyncNode *, std::vector<const mhp::SyncNode *>>
      m_extra_hb_successors;
  mutable std::unordered_map<const llvm::Function *,
                             std::unique_ptr<llvm::PostDominatorTree>>
      m_post_dom_cache;

  /// Synchronizes-with pairs proven by non-atomic witness mechanisms.
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_sync_with;
  mutable std::unordered_map<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>, bool,
      InstPairHash>
      m_hb_cache;
  static constexpr size_t kMaxHBCacheEntries = 250000;
  bool m_has_custom_hb_index = false;
  std::vector<int> m_hb_topo_order;
  std::vector<size_t> m_hb_scc_id;
  std::unordered_map<mhp::ThreadID, std::unordered_set<mhp::ThreadID>>
      m_hb_thread_reachability;
  mutable std::vector<uint32_t> m_visit_stamp;
  mutable uint32_t m_current_stamp = 0;
  mutable std::vector<const mhp::SyncNode *> m_bfs_queue;
  size_t m_analyzed_generation = 0;
};

} // namespace lotus

