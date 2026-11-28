#pragma once

#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <cstdint>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>

namespace tpa::taint {

class EvalResult;
class ProgramPoint;
class TaintMemo;
class WorkList;

class TaintPropagator {
private:
  using NodeSet = util::VectorSet<DefUseInstruction *>;

  TaintMemo &memo;
  WorkList &workList;

  // Reusable scratch buffers across propagate() calls to avoid per-step heap
  // allocations.
  std::vector<std::pair<uint32_t, uint64_t>> scratchChunks;
  llvm::SmallDenseMap<uint32_t, std::pair<uint64_t, uint64_t>, 4>
      overrideChunks;
  struct SuccChunkBatch {
    const NodeSet *succs = nullptr;
    llvm::SmallDenseMap<uint32_t, uint64_t, 8> chunks;
  };
  llvm::SmallDenseMap<const NodeSet *, uint32_t, 16> succsToBatch;
  std::vector<SuccChunkBatch> batches;

  void propagateObjFrom(const ProgramPoint &, const tpa::MemoryObject *,
                        TaintLattice);
  void applyChunkBatchToSuccs(
      const context::Context *ctx, const NodeSet &succs,
      llvm::ArrayRef<std::pair<uint32_t, uint64_t>> chunkVals);
  void flushBatches(const context::Context *ctx);

public:
  TaintPropagator(TaintMemo &m, WorkList &w) : memo(m), workList(w) {}

  void propagate(const EvalResult &, llvm::ArrayRef<uint32_t> dirtyObjIds = {});
};

} // namespace tpa::taint