/// @file GuardedValueFlowNodes.h
/// @brief Node types in the Guarded Value Flow Graph (GVFG)
///
/// GVFG models intra-procedural value flow as a directed graph.  Nodes
/// fall into four broad categories:
///
///   - **Operand–like nodes** (arguments, simple operands, undef, load-mem,
///     store-mem, call-output, return) carry an LLVM `Value *` and represent
///     SSA values or memory-access placeholders.
///
///   - **Opcode nodes** (binary, cast, cmp, select, GEP, concat) carry the
///     arithmetic / logic operation that produced them.  `getValueFlowParents`
///     treats cast and address-arithmetic opcodes as transparent for
///     value-flow traversal.
///
///   - **Region nodes** (`GuardedValueFlowRegionNode`) encode Boolean
///     path conditions as dataflow sub-graphs, supporting AND / OR / NOT
///     composition and satisfiability queries via `ConstraintState`.
///
///   - **Call-summary nodes** (`GuardedValueFlowCallSummaryNode`) model
///     coarse access-path buckets that are intentionally coarser than the
///     direct pseudo-interface channels.
///
/// **Edge direction**: child edges point *from* a consumer *to* its
/// producer.  A parent is data-dependent on the child.  Each edge carries
/// an optional `ConditionRef` guard and a float confidence.

#pragma once

#include "IR/GVFG/ConditionRef.h"
#include "IR/GVFG/GuardedValueFlowObject.h"

#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Error.h>

namespace lotus {
namespace gvfg {

using llvm::ArrayRef;
using llvm::BasicBlock;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::Function;
using llvm::Instruction;
using llvm::path_cond_t;
using llvm::Type;
using llvm::Value;

class GuardedValueFlowGraph;
class GuardedValueFlowOpcodeNode;
class GuardedValueFlowSite;
class GuardedValueFlowReturnSite;
class GuardedValueFlowRegionNode;

/// Abstract field path attached to interface and summary nodes.
/// Offsets are stored from leaf to root so the adapter can append newly
/// discovered outer segments without rebuilding the whole path.
class AccessPath {
public:
  AccessPath() = default;
  AccessPath(Value *base, int64_t offset, bool is_from_return = false) {
    addLevel(base, offset, is_from_return);
  }

  Value *getBase() const { return base_; }
  Value *get_base_ptr() const { return base_; }
  int getDepth() const { return static_cast<int>(offsets_reversed_.size()); }
  int64_t getOffset(int idx) const {
    assert(idx >= 0 && idx < getDepth() && "Invalid access path offset index");
    return offsets_reversed_[offsets_reversed_.size() - idx - 1];
  }
  bool isFromReturn() const { return is_from_return_; }
  bool empty() const { return base_ == nullptr && offsets_reversed_.empty(); }

  void addLevel(Value *base, int64_t offset, bool is_from_return = false) {
    base_ = base;
    offsets_reversed_.push_back(offset);
    is_from_return_ = is_from_return;
  }

  void reset(Value *base = nullptr, bool is_from_return = false) {
    offsets_reversed_.clear();
    base_ = base;
    is_from_return_ = is_from_return;
  }

  void resetCurrentLevel(Value *base, int64_t offset,
                         bool is_from_return = false) {
    if (!offsets_reversed_.empty()) {
      base_ = base;
      offsets_reversed_.back() -= offset;
      is_from_return_ = is_from_return;
    }
  }

  void reset(const AccessPath &other) {
    offsets_reversed_ = other.offsets_reversed_;
    base_ = other.base_;
    is_from_return_ = other.is_from_return_;
  }

private:
  Value *base_{nullptr};
  std::vector<int64_t> offsets_reversed_;
  bool is_from_return_{false};
};

/// Common base for all GVFG nodes.
///
/// Every node carries:
///   - a kind tag for `isa<>` / `dyn_cast<>`,
///   - a parent basic block (for automatic region assignment),
///   - an optional LLVM `Value *` (for operand-like nodes),
///   - an optional debug `Instruction *` (for display).
///
/// **Edges** are stored bidirectionally: `children_` (consumed producers) and
/// `parents_` (consumer back-references).  Each edge carries a `ConditionRef`
/// guard and a float confidence.
///
/// **Matching regions** are populated by the adapter pass to record the path
/// condition under which a load-memory node's producer is valid.
class GuardedValueFlowNode : public GuardedValueFlowObject {
public:
  enum class Kind {
    CommonArgument,
    PseudoArgument,
    VariableArgument,
    CommonReturn,
    PseudoReturn,
    SimpleOperand,
    UndefValue,
    LoadMemory,
    StoreMemory,
    Phi,
    Region,
    CallSiteCommonOutput,
    CallSitePseudoOutput,
    CallSitePseudoInput,
    CallSiteArgumentSummary,
    CallSiteReturnSummary,
    InterfaceCondition,
    SimpleOpcode,
    CastOpcode,
    Unknown,
  };

  struct Edge {
    GuardedValueFlowNode *target{nullptr};
    float confidence{1.0f};
    ConditionRef condition;
  };

  struct MatchingRegion {
    GuardedValueFlowNode *producer{nullptr};
    GuardedValueFlowRegionNode *region{nullptr};
    ConditionRef provenance;
  };

  struct OperandUser {
    const GuardedValueFlowOpcodeNode *user{nullptr};
    unsigned operand_index{0};
  };

  GuardedValueFlowNode(Kind kind, Type *type, GuardedValueFlowGraph *graph,
                       BasicBlock *block, Value *llvm_value = nullptr,
                       Instruction *dbg_inst = nullptr);
  virtual ~GuardedValueFlowNode() = default;

  Kind getKind() const { return kind_; }
  Type *getType() const { return type_; }
  GuardedValueFlowGraph *getGraph() const override { return graph_; }
  BasicBlock *getParentBasicBlock() const override { return block_; }
  Value *getLLVMValue() const { return llvm_value_; }
  Value *getDebugValue() const override { return llvm_value_; }
  Instruction *getDebugInstruction() const override { return dbg_inst_; }
  unsigned getNodeId() const { return node_id_; }

  static bool classof(const GuardedValueFlowObject *object) {
    return object->getDomain() == Domain::Node;
  }

  virtual void addChild(GuardedValueFlowNode *child, float confidence = 1.0f,
                        ConditionRef condition = ConditionRef::none());
  virtual void clearChildren();
  GuardedValueFlowNode *getChild(unsigned idx) const {
    assert(idx < children_.size() && "Invalid child index");
    return children_[idx].target;
  }
  ArrayRef<Edge> children() const { return children_; }
  /// Set-like producer dependencies. Unlike opcode operands, this view is
  /// deduplicated by producer node and carries no positional meaning.
  ArrayRef<Edge> uniqueDataInputs() const { return children_; }
  unsigned getNumChildren() const {
    return static_cast<unsigned>(children_.size());
  }
  ArrayRef<Edge> parents() const { return parents_; }
  /// Occurrence-preserving reverse opcode uses. A producer used twice by one
  /// opcode appears twice with distinct operand indices.
  std::vector<OperandUser> operandUsers() const;
  unsigned getNumParents() const {
    return static_cast<unsigned>(parents_.size());
  }
  bool containsParent(const GuardedValueFlowNode *parent) const;
  /// Confidence attached to a producer or consumer dependency edge.
  float getConfidence(const GuardedValueFlowNode *other) const {
    for (const auto &edge : parents_) if (edge.target == other) return edge.confidence;
    for (const auto &edge : children_) if (edge.target == other) return edge.confidence;
    return 1.0f;
  }
  std::vector<GuardedValueFlowNode *>
  getValueFlowParents(bool enable_arithmetic_flow = false) const;

  void addUseSite(GuardedValueFlowSite *site);
  ArrayRef<GuardedValueFlowSite *> useSites() const { return use_sites_; }

  GuardedValueFlowRegionNode *getRegion() const { return region_; }

  void setDescription(std::string desc) { description_ = std::move(desc); }
  const std::string &getDescription() const { return description_; }

  void setAccessPath(AccessPath path) { access_path_ = path; }
  AccessPath &getAccessPath() { return access_path_; }
  const AccessPath &getAccessPath() const { return access_path_; }

  void setIndex(unsigned idx) { index_ = idx; }
  unsigned getIndex() const { return index_; }

  void addMatchingRegion(GuardedValueFlowNode *producer,
                         GuardedValueFlowRegionNode *region,
                         ConditionRef provenance = ConditionRef::none());
  GuardedValueFlowRegionNode *
  getMatchingRegion(const GuardedValueFlowNode *producer) const;
  ConditionRef getMatchingCondition(const GuardedValueFlowNode *producer) const;
  void clearMatchingRegions() { matching_regions_.clear(); }
  ArrayRef<MatchingRegion> getMatchingRegions() const {
    return matching_regions_;
  }

protected:
  Kind kind_;
  Type *type_;
  GuardedValueFlowGraph *graph_;
  BasicBlock *block_;
  Value *llvm_value_;
  Instruction *dbg_inst_;
  unsigned node_id_{0};
  unsigned index_{0};
  std::string description_;
  AccessPath access_path_;
  GuardedValueFlowRegionNode *region_{nullptr};
  std::vector<Edge> children_;
  std::vector<Edge> parents_;
  std::vector<GuardedValueFlowSite *> use_sites_;
  std::vector<MatchingRegion> matching_regions_;

  friend class GuardedValueFlowGraph;
};

/// Argument node covering common arguments, pseudo arguments, and varargs.
class GuardedValueFlowArgumentNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowArgumentNode(Kind kind, Type *type,
                               GuardedValueFlowGraph *graph, BasicBlock *block,
                               Value *llvm_value)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value,
                             dyn_cast<Instruction>(llvm_value)) {}

  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CommonArgument ||
           node->getKind() == Kind::PseudoArgument ||
           node->getKind() == Kind::VariableArgument;
  }
};

/// Region nodes encode **Boolean path conditions** as dataflow sub-graphs.
///
/// **Forms**:
///   - `AlwaysTrue` / `AlwaysFalse` — canonical constants
///   - `Unit` — `(condition_node == sense)`, created from branch/switch guards
///   - `Semantic` — backed by a `path_cond_t` from the constraint system
///   - `ImportedInterface` — cross-function condition imported from a callee
///   - `And` / `Or` / `Not` — Boolean composition of child regions
///
/// Each region carries a `ConstraintState` (assignment map) for satisfiability
/// checks.  AND merges assignments; OR intersects them; NOT complements.
/// Complementary unit regions (same condition node, opposite sense) collapse
/// to AlwaysTrue / AlwaysFalse.
class GuardedValueFlowRegionNode : public GuardedValueFlowNode {
public:
  struct ConstraintState {
    std::map<const GuardedValueFlowNode *, bool> assignments;
  };

  enum class Form {
    AlwaysTrue,
    AlwaysFalse,
    Unit,
    Semantic,
    ImportedInterface,
    And,
    Or,
    Not,
  };

  GuardedValueFlowRegionNode(Type *type, GuardedValueFlowGraph *graph,
                             BasicBlock *block, Form form,
                             GuardedValueFlowNode *condition_node,
                             bool condition_sense, ConditionRef condition)
      : GuardedValueFlowNode(Kind::Region, type, graph, block, nullptr,
                             block ? block->getTerminator() : nullptr),
        form_(form), condition_node_(condition_node),
        condition_sense_(condition_sense), region_condition_(condition) {
    if (form_ == Form::AlwaysTrue) {
      setAlwaysTrue();
      return;
    }
    if (form_ == Form::AlwaysFalse) {
      setAlwaysFalse();
      return;
    }

    if (form_ == Form::Unit) {
      if (!condition_node_) {
        if (condition_sense_)
          setAlwaysTrue();
        else
          setAlwaysFalse();
        return;
      }

      if (auto *condition_region =
              dyn_cast<GuardedValueFlowRegionNode>(condition_node_)) {
        if (condition_region->isAlwaysTrue()) {
          if (condition_sense_)
            setAlwaysTrue();
          else
            setAlwaysFalse();
          return;
        }
        if (condition_region->isAlwaysFalse()) {
          if (condition_sense_)
            setAlwaysFalse();
          else
            setAlwaysTrue();
          return;
        }
      }

      constraint_state_.assignments[this] = true;
      constraint_state_.assignments[condition_node_] = condition_sense_;
      is_satisfiable_ = true;
    }
  }

  Form getForm() const { return form_; }
  bool isAlwaysTrue() const { return form_ == Form::AlwaysTrue; }
  bool isAlwaysFalse() const { return form_ == Form::AlwaysFalse; }
  bool isInterfaceRegion() const { return form_ == Form::ImportedInterface; }
  bool isSemantic() const {
    return form_ == Form::Semantic || form_ == Form::ImportedInterface;
  }
  bool isLocalSemanticRegion() const { return form_ == Form::Semantic; }
  bool isImportedSemanticRegion() const {
    return form_ == Form::ImportedInterface;
  }
  bool isCompound() const {
    return form_ == Form::And || form_ == Form::Or || form_ == Form::Not;
  }
  GuardedValueFlowNode *getConditionNode() const { return condition_node_; }
  bool getConditionSense() const { return condition_sense_; }
  const ConditionRef &getRegionCondition() const { return region_condition_; }
  Function *getInterfaceOwnerFunction() const {
    return interface_owner_function_;
  }
  Function *getInterfaceOriginFunction() const {
    return interface_origin_function_;
  }
  path_cond_t getInterfacePathCondition() const {
    return interface_path_condition_;
  }
  path_cond_t getImportedSourceCondition() const {
    return imported_source_condition_;
  }
  bool isSatisfiable() const { return is_satisfiable_; }
  const ConstraintState &getConstraintState() const {
    return constraint_state_;
  }
  void setInterfaceMetadata(Function *owner_function, Function *origin_function,
                            path_cond_t interface_path_condition,
                            path_cond_t imported_source_condition) {
    interface_owner_function_ = owner_function;
    interface_origin_function_ = origin_function;
    interface_path_condition_ = interface_path_condition;
    imported_source_condition_ = imported_source_condition;
  }

private:
  Form form_;
  GuardedValueFlowNode *condition_node_{nullptr};
  bool condition_sense_{true};
  ConditionRef region_condition_;
  bool is_satisfiable_{true};
  ConstraintState constraint_state_;
  Function *interface_owner_function_{nullptr};
  Function *interface_origin_function_{nullptr};
  path_cond_t interface_path_condition_{nullptr};
  path_cond_t imported_source_condition_{nullptr};

public:
  void setAlwaysFalse() {
    form_ = Form::AlwaysFalse;
    is_satisfiable_ = false;
    constraint_state_.assignments.clear();
  }

  void setAlwaysTrue() {
    form_ = Form::AlwaysTrue;
    is_satisfiable_ = true;
    constraint_state_.assignments.clear();
  }

  void setConstraintState(ConstraintState state) {
    constraint_state_ = std::move(state);
    is_satisfiable_ = true;
  }

  void markUnsatisfiable() {
    is_satisfiable_ = false;
    constraint_state_.assignments.clear();
  }

public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::Region;
  }
};

/// Opcode node encoding arithmetic, logic, cast, GEP, select, and cmp
/// operations.
///
/// Opcode operands are occurrences: they preserve LLVM operand order and
/// multiplicity. The inherited `children()` / `uniqueDataInputs()` relation is
/// deliberately set-like and remains suitable for dependency traversal. Thus
/// `add x, x` has two operand uses but one unique data input.
///
/// When `hasIntConstant()` is true, the node carries one explicit operand and
/// one implicit integer constant (used for address arithmetic such as
/// `gep.dynamic.offset = idx * sizeof(elem)`).
class GuardedValueFlowOpcodeNode : public GuardedValueFlowNode {
public:
  struct OperandUse {
    GuardedValueFlowNode *producer{nullptr};
    unsigned operand_index{0};
    float confidence{1.0f};
    ConditionRef condition;
  };

  enum class OpcodeKind {
    Invalid,
    URem,
    FRem,
    SRem,
    UDiv,
    SDiv,
    FDiv,
    And,
    Or,
    Xor,
    Shl,
    LShr,
    AShr,
    Mul,
    FMul,
    FAdd,
    FSub,
    Add,
    Sub,
    AddrSpaceCast,
    IntToPtr,
    PtrToInt,
    BitCast,
    ZExt,
    SExt,
    Trunc,
    FPTrunc,
    FPExt,
    SIToFP,
    FPToSI,
    UIToFP,
    FPToUI,
    ExtractElement,
    InsertElement,
    GetElementPtr,
    Select,
    ICmp,
    FCmp,
    Concat,
  };

  GuardedValueFlowOpcodeNode(Kind kind, Type *type,
                             GuardedValueFlowGraph *graph, BasicBlock *block,
                             OpcodeKind opcode_kind)
      : GuardedValueFlowNode(kind, type, graph, block, nullptr, nullptr),
        opcode_kind_(opcode_kind) {}

  OpcodeKind getOpcodeKind() const { return opcode_kind_; }
  void addOperand(GuardedValueFlowNode *producer, float confidence = 1.0f,
                  ConditionRef condition = ConditionRef::none());
  llvm::Error setOperand(unsigned index, GuardedValueFlowNode *producer,
                         float confidence = 1.0f,
                         ConditionRef condition = ConditionRef::none());

  /// Compatibility spelling. On opcode nodes a child is an operand
  /// occurrence, so repeated calls with the same producer do not collapse.
  void addChild(GuardedValueFlowNode *producer, float confidence = 1.0f,
                ConditionRef condition = ConditionRef::none()) override {
    addOperand(producer, confidence, condition);
  }
  void clearChildren() override;

  ArrayRef<OperandUse> operands() const { return operands_; }
  unsigned getNumOperands() const {
    return static_cast<unsigned>(operands_.size());
  }
  GuardedValueFlowNode *getOperand(unsigned index) const {
    assert(index < operands_.size() && "Invalid opcode operand index");
    return operands_[index].producer;
  }

  // Typed positional accessors for the opcode families most prone to operand
  // order mistakes.
  GuardedValueFlowNode *conditionOperand() const { return getOperand(0); }
  GuardedValueFlowNode *trueValueOperand() const { return getOperand(1); }
  GuardedValueFlowNode *falseValueOperand() const { return getOperand(2); }
  GuardedValueFlowNode *vectorOperand() const { return getOperand(0); }
  GuardedValueFlowNode *insertedValue() const { return getOperand(1); }
  GuardedValueFlowNode *indexOperand() const {
    return getOperand(opcode_kind_ == OpcodeKind::InsertElement ? 2 : 1);
  }
  void setCmpPredicate(int predicate) { cmp_predicate_ = predicate; }
  int getCmpPredicate() const { return cmp_predicate_; }
  void setCastWidths(uint64_t src_bits, uint64_t dst_bits) {
    cast_src_bits_ = src_bits;
    cast_dst_bits_ = dst_bits;
  }
  uint64_t getCastSrcBits() const { return cast_src_bits_; }
  uint64_t getCastDstBits() const { return cast_dst_bits_; }
  void setIntConstant(int64_t value) {
    has_int_constant_ = true;
    int_constant_ = value;
  }
  bool hasIntConstant() const { return has_int_constant_; }
  int64_t getIntConstant() const { return int_constant_; }

private:
  OpcodeKind opcode_kind_;
  int cmp_predicate_{-1};
  uint64_t cast_src_bits_{0};
  uint64_t cast_dst_bits_{0};
  bool has_int_constant_{false};
  int64_t int_constant_{0};
  std::vector<OperandUse> operands_;

public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::SimpleOpcode ||
           node->getKind() == Kind::CastOpcode;
  }
};

/// PHI node with incoming-edge-local guards.
///
/// Each incoming edge carries the value node, incoming basic block, and an
/// optional condition node derived from the incoming block's terminator.
/// The condition is the edge-local guard (branch direction, switch case)
/// that must hold for this particular incoming value to flow into the phi.
class GuardedValueFlowPhiNode : public GuardedValueFlowNode {
public:
  struct Incoming {
    GuardedValueFlowNode *value_node{nullptr};
    BasicBlock *incoming_block{nullptr};
    GuardedValueFlowNode *condition_node{nullptr};
    bool condition_sense{true};
    ConditionRef condition;
  };

  GuardedValueFlowPhiNode(Type *type, GuardedValueFlowGraph *graph,
                          BasicBlock *block, Value *llvm_value,
                          Instruction *dbg_inst)
      : GuardedValueFlowNode(Kind::Phi, type, graph, block, llvm_value,
                             dbg_inst) {}

  void addIncoming(GuardedValueFlowNode *value_node, BasicBlock *incoming_block,
                   GuardedValueFlowNode *condition_node, bool condition_sense,
                   ConditionRef condition);
  ArrayRef<Incoming> incoming() const { return incoming_; }
  auto begin() const { return incoming_.begin(); }
  auto end() const { return incoming_.end(); }

private:
  std::vector<Incoming> incoming_;

public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::Phi;
  }
};

/// Return node (common or pseudo) with occurrence-preserving return tracking.
class GuardedValueFlowReturnNode : public GuardedValueFlowNode {
public:
  struct ReturnIncoming {
    GuardedValueFlowNode *value{nullptr};
    GuardedValueFlowReturnSite *site{nullptr};
    ConditionRef guard;
  };

  GuardedValueFlowReturnNode(Kind kind, Type *type,
                             GuardedValueFlowGraph *graph, BasicBlock *block,
                             Value *llvm_value = nullptr)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value,
                             dyn_cast_or_null<Instruction>(llvm_value)) {}

  void addReturnValueSitePair(GuardedValueFlowNode *value_node,
                              GuardedValueFlowReturnSite *site,
                              ConditionRef guard = ConditionRef::none());
  ArrayRef<ReturnIncoming> incomingReturns() const { return incoming_returns_; }
  void clearChildren() override;
  GuardedValueFlowReturnSite *
  getReturnSite(const GuardedValueFlowNode *value_node) const;

private:
  std::vector<ReturnIncoming> incoming_returns_;

public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CommonReturn ||
           node->getKind() == Kind::PseudoReturn;
  }
};

/// Call output node covering three interface roles:
///   - **CommonOutput** — direct non-void call result (the function return value
///     at the call site)
///   - **PseudoInput** — per-callee incoming side-effect channel
///   - **PseudoOutput** — per-callee outgoing side-effect channel
class GuardedValueFlowCallOutputNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowCallOutputNode(Kind kind, Type *type,
                                 GuardedValueFlowGraph *graph,
                                 BasicBlock *block, Value *llvm_value,
                                 Instruction *call_site,
                                 Function *callee = nullptr)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value, call_site),
        call_site_(call_site), callee_(callee) {}

  Instruction *getCallSite() const { return call_site_; }
  Function *getCallee() const { return callee_; }

private:
  Instruction *call_site_;
  Function *callee_;

public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CallSiteCommonOutput ||
           node->getKind() == Kind::CallSitePseudoOutput ||
           node->getKind() == Kind::CallSitePseudoInput;
  }
};

/// Summary nodes model access-path buckets that are intentionally coarser than
/// direct pseudo interfaces, used when an interface channel exceeds the
/// configured access-path depth limit and must be merged into a summary.
class GuardedValueFlowCallSummaryNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowCallSummaryNode(Kind kind, Type *type,
                                  GuardedValueFlowGraph *graph,
                                  BasicBlock *block, Instruction *call_site,
                                  Function *callee, unsigned summary_index)
      : GuardedValueFlowNode(kind, type, graph, block, nullptr, call_site),
        call_site_(call_site), callee_(callee), summary_index_(summary_index) {}

  Instruction *getCallSite() const { return call_site_; }
  Function *getCallee() const { return callee_; }
  unsigned getSummaryIndex() const { return summary_index_; }

  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowNode::classof(object) &&
           classof(static_cast<const GuardedValueFlowNode *>(object));
  }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CallSiteArgumentSummary ||
           node->getKind() == Kind::CallSiteReturnSummary;
  }

private:
  Instruction *call_site_;
  Function *callee_;
  unsigned summary_index_{0};
};

} // namespace gvfg
} // namespace lotus
