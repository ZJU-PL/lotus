/// @file GuardedValueFlowGraph.h
/// @brief Per-function guarded value-flow graph and its builder pass
///
/// `GuardedValueFlowGraph` stores one function's SSA-backed value-flow
/// structure: operand/opcode/region/phi/memory nodes, instruction-attached
/// sites, block-level path conditions, and interprocedural interface
/// channels.  The structural builder (`GuardedValueFlowGraphBuilderPass`)
/// creates this shape from LLVM IR, and the adapter
/// (`LotusAAWrapper`) later populates memory-producer
/// edges using pointer-analysis results.
///
/// **Key collections** (all public via const accessors):
///   - `nodes()` — all `GuardedValueFlowNode` instances in the graph
///   - `sites()` — all `GuardedValueFlowSite` instances
///   - value-node and interface-node maps (`value_nodes_`, `interface_nodes_`)
///   - block-level path conditions (`block_conditions_`)
///   - region caches (And / Or / Not, unit, semantic, block)
///   - load/store memory node registries
///   - pseudo argument / pseudo return lists
///   - function-bandary summary argument / return node registries
///   - diagnostics produced during construction or adaptation

#pragma once

#include "IR/GVFG/GuardedValueFlowNodes.h"
#include "IR/GVFG/GuardedValueFlowSites.h"

#include <map>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Argument.h>
#include <llvm/Pass.h>

namespace lotus {
namespace gvfg {

using llvm::AnalysisUsage;
using llvm::Argument;
using llvm::ArrayRef;
using llvm::BasicBlock;
using llvm::DenseMap;
using llvm::Function;
using llvm::Instruction;
using llvm::Module;
using llvm::ModulePass;
using llvm::path_cond_t;
using llvm::StringRef;
using llvm::Type;
using llvm::Value;

// GuardedValueFlowGraph stores one function's value, memory, and path-sensitive
// structure after the structural builder and optional LotusAA adapter have run.
/// Per-function container for the guarded value-flow graph.
///
/// Owns all nodes and sites via `std::unique_ptr` vectors.  Provides
/// canonical constructor accessors (`findNode`, `findOrCreateAndRegion`, …)
/// and registration methods that the builder and adapter use.  Also emits
/// `Diagnostic` records so callers can detect partial or degraded graphs.
class GuardedValueFlowGraph {
public:
  struct Diagnostic {
    enum class Origin {
      Builder,
      Adapter,
    };

    enum class Severity {
      Note,
      Warning,
      Error,
    };

    Origin origin{Origin::Builder};
    Severity severity{Severity::Warning};
    std::string message;
    Instruction *instruction{nullptr};
    BasicBlock *block{nullptr};
  };

  struct BlockCondition {
    // `control_block` contributes a guard to `block`; `guard_successor` names
    // the successor that makes `sense` true for `condition_node`.
    GuardedValueFlowNode *condition_node{nullptr};
    BasicBlock *control_block{nullptr};
    BasicBlock *guard_successor{nullptr};
    ConditionRef condition;
    bool sense{true};
  };

  explicit GuardedValueFlowGraph(Function *base_function);
  ~GuardedValueFlowGraph();

  Function *getBaseFunction() const { return base_function_; }
  Function *getBaseFunc() const { return base_function_; }

  template <typename NodeT, typename... Args>
  NodeT *createNode(Args &&...args) {
    auto node = std::make_unique<NodeT>(std::forward<Args>(args)...);
    NodeT *raw = node.get();
    raw->node_id_ = next_node_id_++;
    raw->object_id_ = next_object_id_++;
    nodes_.push_back(std::move(node));
    assignNodeRegion(raw);
    compat_caches_dirty_ = true;
    return raw;
  }

  template <typename SiteT, typename... Args>
  SiteT *createSite(Args &&...args) {
    auto site = std::make_unique<SiteT>(std::forward<Args>(args)...);
    SiteT *raw = site.get();
    raw->object_id_ = next_object_id_++;
    sites_.push_back(std::move(site));
    return raw;
  }

  GuardedValueFlowNode *findNode(Value *value) const;
  const DenseMap<Value *, GuardedValueFlowNode *> &valueNodes() const {
    return value_nodes_;
  }
  void mapValueNode(Value *value, GuardedValueFlowNode *node);
  // Interface nodes live in a separate namespace so synthetic call-boundary
  // values do not collide with ordinary SSA values.
  GuardedValueFlowNode *findInterfaceNode(Value *value) const;
  void mapInterfaceNode(Value *value, GuardedValueFlowNode *node);
  GuardedValueFlowNode *findPseudoArgumentBySource(Value *value) const;
  void mapPseudoArgumentSource(Value *value, GuardedValueFlowNode *node);
  Argument *createSyntheticInterfaceValue(Type *type, StringRef name);

  GuardedValueFlowCallSite *findCallSite(Instruction *inst) const;
  void mapCallSite(Instruction *inst, GuardedValueFlowCallSite *site);
  GuardedValueFlowNode *findSyntheticGuardNode(Instruction *inst,
                                               BasicBlock *successor) const;
  void mapSyntheticGuardNode(Instruction *inst, BasicBlock *successor,
                             GuardedValueFlowNode *node);

  GuardedValueFlowRegionNode *findRegion(BasicBlock *block) const;
  void mapRegion(BasicBlock *block, GuardedValueFlowRegionNode *node);
  GuardedValueFlowRegionNode *findUnitRegion(GuardedValueFlowNode *condition,
                                             bool sense) const;
  GuardedValueFlowRegionNode *
  findOrCreateUnitRegion(GuardedValueFlowNode *condition, bool sense,
                         BasicBlock *block, ConditionRef condition_ref);
  GuardedValueFlowRegionNode *
  findOrCreateAndRegion(GuardedValueFlowRegionNode *lhs,
                        GuardedValueFlowRegionNode *rhs, BasicBlock *block);
  GuardedValueFlowRegionNode *
  findOrCreateOrRegion(GuardedValueFlowRegionNode *lhs,
                       GuardedValueFlowRegionNode *rhs, BasicBlock *block);
  GuardedValueFlowRegionNode *
  findOrCreateNotRegion(GuardedValueFlowRegionNode *input, BasicBlock *block);
  GuardedValueFlowRegionNode *getAlwaysTrueRegion();
  GuardedValueFlowRegionNode *getAlwaysFalseRegion();
  GuardedValueFlowRegionNode *findSemanticRegion(path_cond_t path_cond) const;
  GuardedValueFlowRegionNode *findOrCreateSemanticRegion(
      path_cond_t path_cond, BasicBlock *block,
      GuardedValueFlowNode *origin_condition_node = nullptr);
  GuardedValueFlowNode *findSemanticConditionNode(path_cond_t path_cond) const;
  void mapSemanticConditionNode(path_cond_t path_cond,
                                GuardedValueFlowNode *node);

  void addBlockCondition(BasicBlock *block, BlockCondition condition);
  ArrayRef<BlockCondition> getBlockConditions(BasicBlock *block) const;

  GuardedValueFlowNode *findLoadMemoryNode(Instruction *inst) const;
  void mapLoadMemoryNode(Instruction *inst, GuardedValueFlowNode *node);

  // Store-memory nodes are keyed by the value being materialized together with
  // the producing instruction. Anonymous producers are used for imported
  // summary/undef cases where no stable source value exists.
  GuardedValueFlowNode *findStoreMemoryNode(Value *value,
                                            Instruction *inst) const;
  void mapStoreMemoryNode(Value *value, Instruction *inst,
                          GuardedValueFlowNode *node);
  GuardedValueFlowNode *
  findOrCreateStoreMemoryNode(Value *value, Instruction *inst, Type *type,
                              BasicBlock *block,
                              StringRef description = "store.mem");
  GuardedValueFlowNode *
  createAnonymousStoreMemoryNode(Type *type, BasicBlock *block,
                                 Instruction *inst,
                                 StringRef description = "store.mem");

  GuardedValueFlowReturnSite *findReturnSite(Instruction *inst) const;
  void mapReturnSite(Instruction *inst, GuardedValueFlowReturnSite *site);
  void registerPseudoArgument(GuardedValueFlowNode *node);
  void registerPseudoReturn(GuardedValueFlowReturnNode *node);
  ArrayRef<GuardedValueFlowNode *> pseudoArguments() const {
    return pseudo_arguments_;
  }
  ArrayRef<GuardedValueFlowReturnNode *> pseudoReturns() const {
    return pseudo_returns_;
  }
  GuardedValueFlowNode *getPseudoArgument(unsigned idx) const;
  GuardedValueFlowReturnNode *getPseudoReturn(unsigned idx) const;
  const GuardedValueFlowReturnNode *getCommonReturn() const;
  unsigned getNumCommonArgument() const;
  unsigned getNumPseudoArgument() const {
    return static_cast<unsigned>(pseudo_arguments_.size());
  }
  unsigned getNumVarArgument() const;
  GuardedValueFlowNode *getCommonArgument(unsigned idx) const;
  GuardedValueFlowNode *getVarArgument(unsigned idx) const;
  std::vector<GuardedValueFlowNode *> getCommonArguments() const;
  std::vector<GuardedValueFlowReturnNode *> getReturnNodes() const;
  /// Owning snapshots. Further graph queries cannot invalidate their
  /// iterators; graph mutation does not change their contents.
  std::vector<GuardedValueFlowNode *> arguments() const;
  std::vector<GuardedValueFlowReturnNode *> returns() const;
  auto arg_begin() const {
    if (compat_caches_dirty_)
      refreshCompatCaches();
    return compat_arg_nodes_.begin();
  }
  auto arg_end() const {
    if (compat_caches_dirty_)
      refreshCompatCaches();
    return compat_arg_nodes_.end();
  }
  auto return_begin() const {
    if (compat_caches_dirty_)
      refreshCompatCaches();
    return compat_return_nodes_.begin();
  }
  auto return_end() const {
    if (compat_caches_dirty_)
      refreshCompatCaches();
    return compat_return_nodes_.end();
  }
  auto pseudo_return_begin() const {
    return pseudo_returns_.begin();
  }
  auto pseudo_return_end() const {
    return pseudo_returns_.end();
  }

  template <typename SiteT> SiteT *findSite(Instruction *inst) const {
    if (!inst)
      return nullptr;
    if constexpr (std::is_same_v<SiteT, GuardedValueFlowCallSite>) {
      return findCallSite(inst);
    } else if constexpr (std::is_same_v<SiteT, GuardedValueFlowReturnSite>) {
      return findReturnSite(inst);
    } else {
      for (const auto &site : sites_) {
        if (site->getInstruction() == inst) {
          if (auto *typed_site = dynamic_cast<SiteT *>(site.get()))
            return typed_site;
        }
      }
      return nullptr;
    }
  }
  GuardedValueFlowNode *findFunctionSummaryArgumentNode(unsigned ap_depth,
                                                        Value *source) const;
  void mapFunctionSummaryArgumentNode(unsigned ap_depth, Value *source,
                                      GuardedValueFlowNode *node);
  GuardedValueFlowNode *findFunctionSummaryReturnNode(unsigned ap_depth) const;
  void mapFunctionSummaryReturnNode(unsigned ap_depth,
                                    GuardedValueFlowNode *node);
  bool isSummaryReturn(const GuardedValueFlowNode *node) const {
    for (const auto &entry : function_summary_return_nodes_)
      if (entry.second == node) return true;
    for (const auto &entry : summary_return_nodes_)
      for (auto *summary : entry.second) if (summary == node) return true;
    return false;
  }
  void resetFunctionSummaryInterface();
  void registerSummaryArgumentNode(unsigned ap_depth,
                                   GuardedValueFlowNode *node);
  void registerSummaryReturnNode(unsigned ap_depth, GuardedValueFlowNode *node);
  ArrayRef<GuardedValueFlowNode *>
  getSummaryArgumentNodes(unsigned ap_depth) const;
  ArrayRef<GuardedValueFlowNode *>
  getSummaryReturnNodes(unsigned ap_depth) const;
  void refreshNodeRegions();

  void addDiagnostic(Diagnostic diagnostic);
  ArrayRef<Diagnostic> diagnostics() const { return diagnostics_; }
  bool hasDiagnostics() const { return !diagnostics_.empty(); }
  bool isDegraded() const;

  struct MemoryProducer {
    GuardedValueFlowNode *producer_memory{nullptr};
    GuardedValueFlowNode *producer_value{nullptr};
    GuardedValueFlowRegionNode *region{nullptr};
    ConditionRef condition;
    float confidence{1.0f};
    bool is_summary{false};
    bool is_unknown{false};
  };

  struct CallTargetInfo {
    Function *callee{nullptr};
    ConditionRef condition;
    GuardedValueFlowRegionNode *region{nullptr};
    bool is_back_edge{false};
  };

  std::vector<GuardedValueFlowNode *>
  getDirectDataDependencies(const GuardedValueFlowNode *node) const;
  std::vector<BlockCondition>
  getDirectControlDependencies(const GuardedValueFlowNode *node) const;
  GuardedValueFlowRegionNode *
  getEffectiveControlRegion(const GuardedValueFlowNode *node) const;
  // Compatibility wrapper. Prefer getDirectControlDependencies() for the
  // immediate guards or getEffectiveControlRegion() for the composed guard.
  std::vector<BlockCondition>
  getEffectiveControlDependencies(const GuardedValueFlowNode *node) const;
  std::vector<MemoryProducer>
  getMemoryProducers(const GuardedValueFlowNode *node) const;
  std::vector<CallTargetInfo>
  getResolvedCallTargets(const GuardedValueFlowCallSite *site) const;

  ArrayRef<std::unique_ptr<GuardedValueFlowNode>> nodes() const {
    return nodes_;
  }
  ArrayRef<std::unique_ptr<GuardedValueFlowSite>> sites() const {
    return sites_;
  }

private:
  struct PointerPairLess {
    bool operator()(const std::pair<Value *, Instruction *> &lhs,
                    const std::pair<Value *, Instruction *> &rhs) const {
      return lhs < rhs;
    }
  };

  struct NodeBoolPairLess {
    bool operator()(const std::pair<GuardedValueFlowNode *, bool> &lhs,
                    const std::pair<GuardedValueFlowNode *, bool> &rhs) const {
      return lhs < rhs;
    }
  };

  struct RegionPairLess {
    bool operator()(const std::pair<GuardedValueFlowRegionNode *,
                                    GuardedValueFlowRegionNode *> &lhs,
                    const std::pair<GuardedValueFlowRegionNode *,
                                    GuardedValueFlowRegionNode *> &rhs) const {
      return lhs < rhs;
    }
  };

  struct InstBlockPairLess {
    bool operator()(const std::pair<Instruction *, BasicBlock *> &lhs,
                    const std::pair<Instruction *, BasicBlock *> &rhs) const {
      return lhs < rhs;
    }
  };

  Function *base_function_;
  unsigned next_node_id_{0};
  unsigned next_object_id_{0};
  std::vector<std::unique_ptr<GuardedValueFlowNode>> nodes_;
  std::vector<std::unique_ptr<GuardedValueFlowSite>> sites_;
  DenseMap<Value *, GuardedValueFlowNode *> value_nodes_;
  DenseMap<Value *, GuardedValueFlowNode *> interface_nodes_;
  DenseMap<Value *, GuardedValueFlowNode *> pseudo_argument_sources_;
  DenseMap<Instruction *, GuardedValueFlowCallSite *> call_sites_;
  DenseMap<Instruction *, GuardedValueFlowReturnSite *> return_sites_;
  std::map<std::pair<Instruction *, BasicBlock *>, GuardedValueFlowNode *,
           InstBlockPairLess>
      synthetic_guard_nodes_;
  DenseMap<BasicBlock *, GuardedValueFlowRegionNode *> regions_;
  DenseMap<Instruction *, GuardedValueFlowNode *> load_memory_nodes_;
  DenseMap<BasicBlock *, std::vector<BlockCondition>> block_conditions_;
  DenseMap<path_cond_t, GuardedValueFlowRegionNode *> semantic_regions_;
  DenseMap<path_cond_t, GuardedValueFlowNode *> semantic_condition_nodes_;
  std::vector<GuardedValueFlowNode *> pseudo_arguments_;
  std::vector<GuardedValueFlowReturnNode *> pseudo_returns_;
  std::map<std::pair<unsigned, Value *>, GuardedValueFlowNode *>
      function_summary_argument_nodes_;
  std::map<unsigned, GuardedValueFlowNode *> function_summary_return_nodes_;
  std::map<unsigned, std::vector<GuardedValueFlowNode *>>
      summary_argument_nodes_;
  std::map<unsigned, std::vector<GuardedValueFlowNode *>> summary_return_nodes_;
  std::map<std::pair<Value *, Instruction *>, GuardedValueFlowNode *,
           PointerPairLess>
      store_memory_nodes_;
  std::map<std::pair<GuardedValueFlowNode *, bool>,
           GuardedValueFlowRegionNode *, NodeBoolPairLess>
      unit_regions_;
  std::map<
      std::pair<GuardedValueFlowRegionNode *, GuardedValueFlowRegionNode *>,
      GuardedValueFlowRegionNode *, RegionPairLess>
      and_regions_;
  std::map<
      std::pair<GuardedValueFlowRegionNode *, GuardedValueFlowRegionNode *>,
      GuardedValueFlowRegionNode *, RegionPairLess>
      or_regions_;
  DenseMap<GuardedValueFlowRegionNode *, GuardedValueFlowRegionNode *>
      not_regions_;
  GuardedValueFlowRegionNode *always_true_region_{nullptr};
  GuardedValueFlowRegionNode *always_false_region_{nullptr};
  std::vector<Value *> owned_synthetic_values_;
  std::vector<Diagnostic> diagnostics_;
  mutable std::vector<GuardedValueFlowNode *> compat_arg_nodes_;
  mutable std::vector<GuardedValueFlowReturnNode *> compat_return_nodes_;
  mutable bool compat_caches_dirty_{true};

  void assignNodeRegion(GuardedValueFlowNode *node);
  void refreshCompatCaches() const;
};

/// ModulePass that builds one `GuardedValueFlowGraph` per function.
///
/// The pass only creates **structural** nodes and edges — expression
/// opcodes, phi nodes, load/store memory placeholders, block-level region
/// nodes, and per-instruction sites.  Memory matching, interprocedural
/// interface nodes, and imported path conditions are layered on later by
/// `LotusAAWrapper`.
class GuardedValueFlowGraphBuilderPass : public ModulePass {
public:
  static char ID;

  GuardedValueFlowGraphBuilderPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
  StringRef getPassName() const override {
    return "GuardedValueFlowGraphBuilderPass";
  }

  // This pass only builds the structural intra-procedural graph. LotusAA memory
  // matches, imported path conditions, and interprocedural interface nodes are
  // layered on later by LotusAAWrapper.
  bool hasGraphFor(const Function &F) const;
  GuardedValueFlowGraph &getGraph(const Function &F);
  void invalidateGraph(const Function &F);

private:
  DenseMap<const Function *, std::unique_ptr<GuardedValueFlowGraph>> graphs_;

  std::unique_ptr<GuardedValueFlowGraph> buildGraph(Function &F);
};

ModulePass *createGuardedValueFlowGraphBuilderPass();

} // namespace gvfg
} // namespace lotus
