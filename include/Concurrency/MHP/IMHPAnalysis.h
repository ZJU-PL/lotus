#pragma once

#include <unordered_set>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus { class AliasAnalysisWrapper; }
namespace lotus::concurrency::OpenMP { class OpenMPSemantics; }
namespace OpenMP { class OpenMPSemantics; }

namespace mhp {

class ThreadFlowGraph;
class SyncNode;

using ThreadID = size_t;
using InstructionSet = std::unordered_set<const llvm::Instruction *>;

class IMHPAnalysis {
public:
  virtual ~IMHPAnalysis() = default;

  virtual void analyze() = 0;

  virtual const ThreadFlowGraph& getThreadFlowGraph() const = 0;
  virtual size_t getAnalysisGeneration() const = 0;
  virtual lotus::AliasAnalysisWrapper* getAliasAnalysis() const = 0;
  virtual const OpenMP::OpenMPSemantics* getOpenMPSemantics() const = 0;
  virtual bool instructionMayExecuteMultipleTimes(const llvm::Instruction* inst) const = 0;
  virtual bool joinEdgeMustOrderTarget(const SyncNode* join_node, const SyncNode* target_node) const = 0;

  virtual bool mayHappenInParallel(const llvm::Instruction *i1,
                                   const llvm::Instruction *i2) const = 0;

  /// Return true when the analysis proved the program executes strictly on a
  /// single non-overlapping thread (so no pair of instructions may happen in
  /// parallel).
  virtual bool isSingleThreaded() const { return false; }

  virtual bool isPrecomputedMHP(const llvm::Instruction *i1,
                                const llvm::Instruction *i2) const = 0;

  virtual InstructionSet
  getParallelInstructions(const llvm::Instruction *inst) const = 0;

  virtual bool mustBeSequential(const llvm::Instruction *i1,
                                const llvm::Instruction *i2) const = 0;

  virtual ThreadID getThreadID(const llvm::Instruction *inst) const = 0;

  virtual InstructionSet getInstructionsInThread(ThreadID tid) const = 0;

  virtual size_t getMhpPairCount() const = 0;

  virtual void printStatistics(llvm::raw_ostream &os) const = 0;
  virtual void printResults(llvm::raw_ostream &os) const = 0;
};

} // namespace mhp
