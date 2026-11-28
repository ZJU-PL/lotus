#include "Concurrency/CUDA/CUDAKernelProtocolAnalysis.h"

#include "Concurrency/API/ThreadAPI.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>

namespace concurrency::cuda {

CUDAKernelProtocolAnalysis::CUDAKernelProtocolAnalysis(
    const llvm::Function &kernel, CUDAAbstractState &state)
    : m_kernel(kernel), m_state(state) {}

void CUDAKernelProtocolAnalysis::runAnalysis() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api) {
    return;
  }

  llvm::PostDominatorTree pdt;
  pdt.recalculate(const_cast<llvm::Function &>(m_kernel));

  llvm::SmallVector<const llvm::CallBase *, 8> barriers;
  llvm::SmallVector<const llvm::CallBase *, 8> warp_barriers;
  llvm::SmallVector<const llvm::CallBase *, 8> fences;
  llvm::SmallVector<const llvm::CallBase *, 16> boundaries;

  for (const auto &bb : m_kernel) {
    for (const auto &inst : bb) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto td_type = api->getType(call);

      if (td_type == ThreadAPI::TD_CUDA_BARRIER) {
        barriers.push_back(call);
        boundaries.push_back(call);
      } else if (td_type == ThreadAPI::TD_CUDA_WARP_BARRIER) {
        warp_barriers.push_back(call);
        boundaries.push_back(call);
      } else if (td_type == ThreadAPI::TD_CUDA_MEMORY_BARRIER) {
        fences.push_back(call);
        boundaries.push_back(call);
      }
    }
  }

  const llvm::Instruction *unique_exit = nullptr;
  for (const llvm::BasicBlock &bb : m_kernel) {
    if (llvm::isa<llvm::ReturnInst>(bb.getTerminator())) {
      if (unique_exit) {
        unique_exit = nullptr;
        break;
      }
      unique_exit = bb.getTerminator();
    }
  }
  llvm::SmallPtrSet<const llvm::Instruction *, 16> boundary_set;
  boundary_set.insert(boundaries.begin(), boundaries.end());
  auto epoch_exits = [&](const llvm::CallBase *boundary) {
    llvm::SmallVector<const llvm::Instruction *, 4> exits;
    llvm::SmallVector<const llvm::BasicBlock *, 8> worklist;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> visited;

    for (const llvm::Instruction *next = boundary->getNextNode(); next;
         next = next->getNextNode()) {
      if (boundary_set.count(next)) {
        exits.push_back(next);
        return exits;
      }
    }
    for (const llvm::BasicBlock *successor :
         llvm::successors(boundary->getParent())) {
      worklist.push_back(successor);
    }
    while (!worklist.empty()) {
      const llvm::BasicBlock *block = worklist.pop_back_val();
      if (!visited.insert(block).second) {
        continue;
      }
      const llvm::Instruction *first_boundary = nullptr;
      for (const llvm::Instruction &inst : *block) {
        if (boundary_set.count(&inst)) {
          first_boundary = &inst;
          break;
        }
      }
      if (first_boundary) {
        if (!llvm::is_contained(exits, first_boundary)) {
          exits.push_back(first_boundary);
        }
        continue;
      }
      if (llvm::isa<llvm::ReturnInst>(block->getTerminator())) {
        if (!llvm::is_contained(exits, block->getTerminator())) {
          exits.push_back(block->getTerminator());
        }
        continue;
      }
      for (const llvm::BasicBlock *successor : llvm::successors(block)) {
        worklist.push_back(successor);
      }
    }
    if (exits.empty() && unique_exit) {
      exits.push_back(unique_exit);
    }
    return exits;
  };

  auto set_epoch_exits = [&](CUDAProtocolEpoch &epoch,
                             const llvm::CallBase *boundary) {
    auto exits = epoch_exits(boundary);
    epoch.possible_exits.assign(exits.begin(), exits.end());
    epoch.exit = exits.size() == 1 ? exits.front() : nullptr;
  };

  for (size_t i = 0; i < barriers.size(); ++i) {
    const llvm::CallBase *barrier = barriers[i];
    CUDAProtocolEpoch epoch;
    epoch.epoch_id =
        m_state.barrier_epochs.size() + m_state.fence_epochs.size();
    epoch.kernel = &m_kernel;
    epoch.state = ProtocolState::BarrierActive;
    epoch.entry = barrier;
    set_epoch_exits(epoch, barrier);
    epoch.scope = static_cast<int>(2);
    m_barrier_epochs.push_back(epoch);
    m_state.barrier_epochs.push_back(epoch);
    m_state.protocol_epoch_by_class[epoch.epoch_id] = epoch;
  }

  for (size_t i = 0; i < warp_barriers.size(); ++i) {
    const llvm::CallBase *warp_barrier = warp_barriers[i];
    CUDAProtocolEpoch epoch;
    epoch.epoch_id =
        m_state.barrier_epochs.size() + m_state.fence_epochs.size();
    epoch.kernel = &m_kernel;
    epoch.state = ProtocolState::WarpSyncRequired;
    epoch.entry = warp_barrier;
    set_epoch_exits(epoch, warp_barrier);
    epoch.scope = static_cast<int>(1);
    m_barrier_epochs.push_back(epoch);
    m_state.barrier_epochs.push_back(epoch);
    m_state.protocol_epoch_by_class[epoch.epoch_id] = epoch;
  }

  for (size_t i = 0; i < fences.size(); ++i) {
    const llvm::CallBase *fence = fences[i];
    const llvm::Function *callee = fence->getCalledFunction();
    int fence_scope = static_cast<int>(3);
    if (callee) {
      llvm::StringRef name = callee->getName();
      if (name.contains("threadfence_block") || name.contains("membar.cta")) {
        fence_scope = static_cast<int>(2);
      } else if (name.contains("threadfence_system") ||
                 name.contains("membar.sys")) {
        fence_scope = static_cast<int>(4);
      }
    }
    CUDAProtocolEpoch epoch;
    epoch.epoch_id =
        m_state.barrier_epochs.size() + m_state.fence_epochs.size();
    epoch.kernel = &m_kernel;
    epoch.state = ProtocolState::FenceRequired;
    epoch.entry = fence;
    set_epoch_exits(epoch, fence);
    epoch.scope = fence_scope;
    m_fence_epochs.push_back(epoch);
    m_state.fence_epochs.push_back(epoch);
    m_state.protocol_epoch_by_class[epoch.epoch_id] = epoch;
  }

  bool has_proper_sync = true;

  for (const auto *barrier : barriers) {
    if (!barrier || llvm::succ_empty(barrier->getParent())) {
      has_proper_sync = false;
      break;
    }
  }

  if (!barriers.empty()) {
    for (const auto *barrier : barriers) {
      if (!barrier) {
        continue;
      }
      const llvm::BasicBlock *barrier_bb = barrier->getParent();
      const llvm::BasicBlock *entry =
          m_kernel.empty() ? nullptr : &m_kernel.getEntryBlock();
      const bool post_dominates_entry =
          barrier_bb && entry && pdt.getNode(barrier_bb) &&
          pdt.getNode(entry) && pdt.dominates(barrier_bb, entry);
      if (!post_dominates_entry) {
        has_proper_sync = false;
        break;
      }
    }
  }

  m_has_proper_sync = has_proper_sync;
}

} // namespace concurrency::cuda
