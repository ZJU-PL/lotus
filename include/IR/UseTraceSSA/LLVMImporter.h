#pragma once

#include "IR/UseTraceSSA/TraceFlowGraph.h"
#include "IR/UseTraceSSA/UseTraceSSA.h"
#include <unordered_map>

namespace llvm {
class BasicBlock;
class Function;
class Module;
class ModuleSlotTracker;
class Instruction;
class Use;
class Value;
} // namespace llvm

namespace lotus {
namespace usetracessa {
namespace detail {
class InstructionLabels;
} // namespace detail

struct LLVMHistoryOptions {
  /// False: track all eligible SSA values. True: track pointer values only.
  bool pointersOnly = false;
  /// Track directly referenced global-variable ADDRESSES as entry definitions.
  /// This does not add memory-state or alias analysis.
  bool trackGlobalAddresses = false;
  /// Record pointer-null branch assumptions on the appropriate CFG edges.
  bool recordNullGuards = true;
};

struct LLVMEdge {
  const llvm::BasicBlock *from = nullptr;
  const llvm::BasicBlock *to = nullptr;
  unsigned successorIndex = 0;
};

struct LLVMNullGuard {
  EdgeID edge = InvalidID;
  SiteID site = InvalidID;
  ValueID value = InvalidID;
  bool nonnull = false;
};

struct LLVMOperandVersion {
  /// InvalidID for an ordinary operand; concrete CFG edge for a phi operand.
  EdgeID edge = InvalidID;
  UseVersion history;
};

/// Owns the UseTraceSSA graph, but not the LLVM function/values/uses. Rebuild after any
/// LLVM mutation; in particular, operand replacement can change use histories
/// without changing the CFG. The result must not outlive its LLVM function.
class LLVMHistoryResult {
public:
  const Graph &graph() const { return History; }
  const llvm::Function &function() const { return *Function; }
  ValueID valueID(const llvm::Value &value) const;
  const llvm::Value *value(ValueID id) const { return Values.at(id); }
  VersionID definition(const llvm::Value &value) const;
  const std::vector<LLVMEdge> &edges() const { return Edges; }
  BlockID blockID(const llvm::BasicBlock &block) const;
  /// Ordinary instruction sites. PHI outputs are an atomic definition batch;
  /// use uses(operand) for their incoming-edge operand sites.
  SiteID siteID(const llvm::Instruction &instruction) const;
  const std::vector<LLVMNullGuard> &nullGuards() const { return NullGuards; }

  /// Exact llvm::Use identity, not merely its User. An untracked/debug/dead use
  /// returns an empty vector. Repeated ordinary operands share one history.
  std::vector<LLVMOperandVersion> uses(const llvm::Use &operand) const;

private:
  struct Binding {
    SiteID site;
    ValueID value;
    EdgeID edge;
  };
  const llvm::Function *Function = nullptr;
  Graph History;
  std::vector<const llvm::Value *> Values;
  std::vector<LLVMEdge> Edges;
  std::vector<LLVMNullGuard> NullGuards;
  std::unordered_map<const llvm::Instruction *, SiteID> InstructionSites;
  std::unordered_map<const llvm::Value *, ValueID> ValueIDs;
  std::unordered_map<const llvm::BasicBlock *, BlockID> BlockIDs;
  std::unordered_map<const llvm::Use *, std::vector<Binding>> Bindings;
  friend class LLVMHistoryBuilder;
};

class LLVMHistoryBuilder {
public:
  /// Non-mutating import and construction. Verifies LLVM SSA first. Throws
  /// invalid_argument with diagnostics on invalid input or a tracked non-void
  /// callbr result (whose availability differs across LLVM versions).
  /// Function declarations yield an empty graph.
  static LLVMHistoryResult build(const llvm::Function &function,
                            LLVMHistoryOptions options = {});
  /// Import all definitions in module order with one shared label-print pass.
  static std::vector<LLVMHistoryResult> build(const llvm::Module &module,
                                              LLVMHistoryOptions options = {});

private:
  static LLVMHistoryResult buildImpl(const llvm::Function &function,
                                     LLVMHistoryOptions options,
                                     const detail::InstructionLabels *labels,
                                     bool verified = false,
                                     llvm::ModuleSlotTracker *slots = nullptr);
};

// ---------------------------------------------------------------------------
// TraceFlowGraph integration: append LLVM histories and scalar transfers.
// ---------------------------------------------------------------------------

/// Append non-mutating LLVM scalar histories, including edge-local null facts.
/// This does NOT run alias analysis or build memory flow. Import SVFG alongside it.
LLVMHistoryResult appendLLVMHistory(TraceFlowGraph &graph, FunctionID id,
                                    const llvm::Function &function,
                                    LLVMHistoryOptions options = {});

/// Supplement a pointer-centric SVFG with integer/scalar SSA data dependencies
/// (e.g. parsed network lengths). Skip loads, stores, calls, atomic memory
/// operations and va_arg: pointer addresses are NOT loaded byte contents and
/// arbitrary argument-to-return flow is not a library summary.
void appendLLVMScalarTransfers(TraceFlowGraph &graph, FunctionID id,
                                const LLVMHistoryResult &history,
                                bool includePointerResults = false);

} // namespace usetracessa
} // namespace lotus
