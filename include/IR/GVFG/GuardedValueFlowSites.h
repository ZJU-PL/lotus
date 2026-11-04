/// @file GuardedValueFlowSites.h
/// @brief Site types annotating semantically important instructions in GVFG
///
/// Sites carry the original LLVM instruction and its operand relationships,
/// complementing the value-flow nodes in the graph.
///
/// **Site kinds**:
///   - `CallSite`            — direct / indirect call instructions
///   - `ReturnSite`          — return instructions
///   - `GEPReferenceSite`    — GetElementPtr instructions
///   - `DereferenceSite`     — load / store instructions
///   - `CompareSite`         — icmp / fcmp instructions
///   - `DivSite`             — signed / unsigned / float division and remainder
///   - `AllocSite`           — alloca instructions
///   - `Unknown`             — unsupported / unlowered instructions
///
/// Sites expose operand pointers so downstream passes can walk from a node
/// back to the instruction-level operation that references it.

#pragma once

#include "IR/GVFG/ConditionRef.h"
#include "IR/GVFG/GuardedValueFlowObject.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace lotus {
namespace gvfg {

using llvm::ArrayRef;
using llvm::Function;
using llvm::Instruction;

class GuardedValueFlowGraph;
class GuardedValueFlowNode;
class GuardedValueFlowRegionNode;

/// Compact input record used by compatibility iterators on call sites.
struct GuardedValueFlowCallSiteInput {
  GuardedValueFlowNode *InputNode{nullptr};
  size_t InputIndex{0};
  bool IsCommonInput{false};

  GuardedValueFlowCallSiteInput() = default;
  GuardedValueFlowCallSiteInput(GuardedValueFlowNode *N, size_t I, bool C)
      : InputNode(N), InputIndex(I), IsCommonInput(C) {}
};

/// Base class for instruction-attached sites.
///
/// Sites annotate instructions that are semantically important to later
/// analyses.  Nodes carry the value-flow graph; sites carry the original
/// program operation and the operands that made that operation interesting.
class GuardedValueFlowSite : public GuardedValueFlowObject {
public:
  enum class Kind {
    CallSite,
    ReturnSite,
    DereferenceSite,
    GEP,
    Compare,
    Div,
    Alloc,
    Unknown,
  };

  GuardedValueFlowSite(Kind kind, GuardedValueFlowGraph *graph,
                       Instruction *inst)
      : GuardedValueFlowObject(Domain::Site), kind_(kind), graph_(graph),
        inst_(inst) {}
  virtual ~GuardedValueFlowSite() = default;

  Kind getKind() const { return kind_; }
  GuardedValueFlowGraph *getGraph() const override { return graph_; }
  Instruction *getInstruction() const { return inst_; }
  Instruction *getDebugInstruction() const override { return inst_; }
  llvm::Value *getDebugValue() const override { return inst_; }
  llvm::BasicBlock *getParentBasicBlock() const override {
    return inst_ ? inst_->getParent() : nullptr;
  }

  static bool classof(const GuardedValueFlowObject *object) {
    return object->getDomain() == Domain::Site;
  }

private:
  Kind kind_;
  GuardedValueFlowGraph *graph_;
  Instruction *inst_;
};

/// Alloca instruction site.
class GuardedValueFlowAllocSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::Alloc;
  }

  GuardedValueFlowAllocSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Alloc, graph, inst) {}
};

/// Call instruction site tracking callees, arguments, pseudo-interface
/// channels, summary nodes, and callee-specific path conditions.
class GuardedValueFlowCallSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::CallSite;
  }

  using CallInputSnapshot = std::vector<GuardedValueFlowCallSiteInput>;

  GuardedValueFlowCallSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::CallSite, graph, inst) {}

  void addCallee(Function *callee) {
    if (!callee)
      return;
    if (std::find(callees_.begin(), callees_.end(), callee) == callees_.end())
      callees_.push_back(callee);
  }
  ArrayRef<Function *> getCallees() const { return callees_; }
  Function *getCalledFunction() const;

  void addCommonInput(GuardedValueFlowNode *node);
  ArrayRef<GuardedValueFlowNode *> getCommonInputs() const {
    return common_inputs_;
  }
  GuardedValueFlowNode *getCommonInput(unsigned index) const {
    assert(index < common_inputs_.size());
    return common_inputs_[index];
  }
  bool isCommonInput(const GuardedValueFlowNode *node) const {
    return std::find(common_inputs_.begin(), common_inputs_.end(), node) !=
           common_inputs_.end();
  }
  unsigned getInputIndex(const GuardedValueFlowNode *node) const {
    auto position = std::find(common_inputs_.begin(), common_inputs_.end(), node);
    assert(position != common_inputs_.end());
    return static_cast<unsigned>(position - common_inputs_.begin());
  }

  void setCommonOutput(GuardedValueFlowNode *node) { common_output_ = node; }
  GuardedValueFlowNode *getCommonOutput() const { return common_output_; }

  void addPseudoInput(Function *callee, GuardedValueFlowNode *node);
  void addPseudoOutput(Function *callee, GuardedValueFlowNode *node);
  void setInputSummaryNode(Function *callee, unsigned summary_index,
                           GuardedValueFlowNode *node) {
    input_summary_nodes_[std::make_pair(callee, summary_index)] = node;
  }
  GuardedValueFlowNode *getInputSummaryNode(Function *callee,
                                            unsigned summary_index) const {
    auto it = input_summary_nodes_.find(std::make_pair(callee, summary_index));
    return it == input_summary_nodes_.end() ? nullptr : it->second;
  }
  void setOutputSummaryNode(Function *callee, unsigned summary_index,
                            GuardedValueFlowNode *node) {
    output_summary_nodes_[std::make_pair(callee, summary_index)] = node;
  }
  GuardedValueFlowNode *getOutputSummaryNode(Function *callee,
                                             unsigned summary_index) const {
    auto it = output_summary_nodes_.find(std::make_pair(callee, summary_index));
    return it == output_summary_nodes_.end() ? nullptr : it->second;
  }
  void setBackEdge(Function *callee) {
    if (callee)
      back_edge_callees_.insert(callee);
  }
  bool isBackEdge(Function *callee) const {
    return callee && back_edge_callees_.count(callee) != 0;
  }
  void setCalleeCondition(Function *callee, ConditionRef condition,
                          GuardedValueFlowRegionNode *region = nullptr);
  bool hasCalleeCondition(Function *callee) const {
    return callee &&
           callee_conditions_.find(callee) != callee_conditions_.end();
  }
  ConditionRef getCalleeCondition(Function *callee) const {
    auto it = callee_conditions_.find(callee);
    return it == callee_conditions_.end() ? ConditionRef::none() : it->second;
  }
  GuardedValueFlowRegionNode *getCalleeConditionRegion(Function *callee) const {
    auto it = callee_condition_regions_.find(callee);
    return it == callee_condition_regions_.end() ? nullptr : it->second;
  }

  GuardedValueFlowNode *getPseudoInput(Function *callee, unsigned idx) const;
  GuardedValueFlowNode *getPseudoOutput(Function *callee, unsigned idx) const;
  unsigned getNumPseudoInputs(Function *callee) const;
  unsigned getNumPseudoOutputs(Function *callee) const;

  /// Returns an owning, callee-specific snapshot. Nested iteration and later
  /// calls for another callee cannot invalidate it.
  CallInputSnapshot inputs(Function *callee) const {
    CallInputSnapshot result;
    result.reserve(common_inputs_.size() + getNumPseudoInputs(callee));
    for (size_t idx = 0; idx < common_inputs_.size(); ++idx)
      result.emplace_back(common_inputs_[idx], idx, true);
    auto pseudo_it = pseudo_inputs_.find(callee);
    if (pseudo_it != pseudo_inputs_.end()) {
      for (size_t idx = 0; idx < pseudo_it->second.size(); ++idx)
        result.emplace_back(pseudo_it->second[idx], idx, false);
    }
    return result;
  }

private:
  std::vector<Function *> callees_;
  std::vector<GuardedValueFlowNode *> common_inputs_;
  GuardedValueFlowNode *common_output_{nullptr};
  std::map<Function *, std::vector<GuardedValueFlowNode *>> pseudo_inputs_;
  std::map<Function *, std::vector<GuardedValueFlowNode *>> pseudo_outputs_;
  std::map<std::pair<Function *, unsigned>, GuardedValueFlowNode *>
      input_summary_nodes_;
  std::map<std::pair<Function *, unsigned>, GuardedValueFlowNode *>
      output_summary_nodes_;
  std::set<Function *> back_edge_callees_;
  std::map<Function *, ConditionRef> callee_conditions_;
  std::map<Function *, GuardedValueFlowRegionNode *> callee_condition_regions_;
  mutable std::map<Function *, CallInputSnapshot> compat_inputs_caches_;

public:
  auto input_begin(Function *callee) const {
    auto [it, inserted] = compat_inputs_caches_.try_emplace(callee);
    if (inserted)
      it->second = inputs(callee);
    return it->second.begin();
  }

  auto input_end(Function *callee) const {
    auto [it, inserted] = compat_inputs_caches_.try_emplace(callee);
    if (inserted)
      it->second = inputs(callee);
    return it->second.end();
  }
};

/// Load / store instruction site.  For loads only the pointer operand is
/// populated; for stores both pointer and value operands are recorded.
class GuardedValueFlowDereferenceSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::DereferenceSite;
  }

  GuardedValueFlowDereferenceSite(GuardedValueFlowGraph *graph,
                                  Instruction *inst)
      : GuardedValueFlowSite(Kind::DereferenceSite, graph, inst) {}

  void setPointerOperand(GuardedValueFlowNode *node) {
    pointer_operand_ = node;
  }
  void setValueOperand(GuardedValueFlowNode *node) { value_operand_ = node; }
  GuardedValueFlowNode *getPointerOperand() const { return pointer_operand_; }
  GuardedValueFlowNode *getValueOperand() const { return value_operand_; }

private:
  GuardedValueFlowNode *pointer_operand_{nullptr};
  GuardedValueFlowNode *value_operand_{nullptr};
};

/// Return instruction site.
class GuardedValueFlowReturnSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::ReturnSite;
  }

  GuardedValueFlowReturnSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::ReturnSite, graph, inst) {}
};

/// GEP instruction site recording the pointer operand, offset operands, and
/// result node.  Offset operands stay aligned with the original IR indices
/// even when internal lowering inserts temporary cast/add nodes.
class GuardedValueFlowGEPReferenceSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::GEP;
  }

  GuardedValueFlowGEPReferenceSite(GuardedValueFlowGraph *graph,
                                   Instruction *inst)
      : GuardedValueFlowSite(Kind::GEP, graph, inst) {}

  void setPointerOperand(GuardedValueFlowNode *node) {
    pointer_operand_ = node;
  }
  GuardedValueFlowNode *getPointerOperand() const { return pointer_operand_; }
  void addOffsetOperand(GuardedValueFlowNode *node) {
    offset_operands_.push_back(node);
  }
  ArrayRef<GuardedValueFlowNode *> getOffsetOperands() const {
    return offset_operands_;
  }
  void setResultNode(GuardedValueFlowNode *node) { result_node_ = node; }
  GuardedValueFlowNode *getResultNode() const { return result_node_; }

private:
  GuardedValueFlowNode *pointer_operand_{nullptr};
  GuardedValueFlowNode *result_node_{nullptr};
  std::vector<GuardedValueFlowNode *> offset_operands_;
};

/// Compare instruction site (icmp / fcmp).
class GuardedValueFlowCompareSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::Compare;
  }

  GuardedValueFlowCompareSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Compare, graph, inst) {}

  void setLhsOperand(GuardedValueFlowNode *node) { lhs_operand_ = node; }
  void setRhsOperand(GuardedValueFlowNode *node) { rhs_operand_ = node; }
  GuardedValueFlowNode *getLhsOperand() const { return lhs_operand_; }
  GuardedValueFlowNode *getRhsOperand() const { return rhs_operand_; }

private:
  GuardedValueFlowNode *lhs_operand_{nullptr};
  GuardedValueFlowNode *rhs_operand_{nullptr};
};

/// Division / remainder site (udiv, sdiv, fdiv, urem, srem, frem).
class GuardedValueFlowDivSite : public GuardedValueFlowSite {
public:
  static bool classof(const GuardedValueFlowObject *object) {
    return GuardedValueFlowSite::classof(object) &&
           classof(static_cast<const GuardedValueFlowSite *>(object));
  }
  static bool classof(const GuardedValueFlowSite *site) {
    return site->getKind() == Kind::Div;
  }

  GuardedValueFlowDivSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Div, graph, inst) {}

  void setLhsOperand(GuardedValueFlowNode *node) { lhs_operand_ = node; }
  void setRhsOperand(GuardedValueFlowNode *node) { rhs_operand_ = node; }
  GuardedValueFlowNode *getLhsOperand() const { return lhs_operand_; }
  GuardedValueFlowNode *getRhsOperand() const { return rhs_operand_; }

private:
  GuardedValueFlowNode *lhs_operand_{nullptr};
  GuardedValueFlowNode *rhs_operand_{nullptr};
};

} // namespace gvfg
} // namespace lotus
