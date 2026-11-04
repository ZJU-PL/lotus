/// @file GuardedValueFlowNodes.cpp
/// @brief Implementation of GVFG node types — edge management, value-flow
///        parent classification, phi-incoming registration, and return-site
///        pairing.
///
/// **Edge semantics**: `addChild(parent, child)` inserts a producer into the
/// parent's `children_` vector and a back-reference into the child's
/// `parents_` vector.  Duplicate edges update confidence and condition.
/// `clearChildren` removes both sides.
///
/// **Value-flow parent classification** (`isValueFlowParent`):
/// Region nodes are never value-flow parents.  Cast-opcode nodes are always
/// value-flow parents.  For simple-opcode nodes, GEP, Add, and Sub are
/// transparent (they represent address arithmetic); the true/false branches
/// of Select are transparent, but the condition operand is not.
/// Arithmetic-opcode nodes (Mul, Div, Rem, And, Or, Xor, etc.) are only
/// transparent when `enable_arithmetic_flow` is true.
///
/// **Use sites**: `addUseSite` records a site in a deduplicated vector so
/// downstream passes can walk from a value node back to every instruction
/// that references it.
///
/// **Matching regions**: the adapter populates per-producer (region,
/// provenance) pairs on load-memory nodes so the solver can attach the
/// correct path condition to each memory producer.

#include "IR/GVFG/GuardedValueFlowNodes.h"

#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/GuardedValueFlowSites.h"

#include <algorithm>

#include <llvm/Support/Errc.h>

using namespace llvm;
using namespace lotus::gvfg;

namespace {

static bool
isArithmeticFlowOpcode(GuardedValueFlowOpcodeNode::OpcodeKind opcode_kind) {
  switch (opcode_kind) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::URem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::And:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Or:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Xor:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Shl:
  case GuardedValueFlowOpcodeNode::OpcodeKind::LShr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AShr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Mul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FMul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FAdd:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FSub:
    return true;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Concat:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Invalid:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast:
  case GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::BitCast:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ZExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Trunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ExtractElement:
  case GuardedValueFlowOpcodeNode::OpcodeKind::InsertElement:
  case GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Select:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ICmp:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FCmp:
    return false;
  }
  return false;
}

static bool isValueFlowParent(const GuardedValueFlowNode *node,
                              const GuardedValueFlowNode *parent,
                              bool enable_arithmetic_flow) {
  if (!node || !parent)
    return false;
  if (isa<GuardedValueFlowRegionNode>(parent))
    return false;

  auto *opcode_parent = dyn_cast<GuardedValueFlowOpcodeNode>(parent);
  if (!opcode_parent)
    return true;

  if (opcode_parent->getKind() == GuardedValueFlowNode::Kind::CastOpcode)
    return true;

  switch (opcode_parent->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
    return true;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Select:
    return opcode_parent->getNumOperands() != 0 &&
           opcode_parent->conditionOperand() != node;
  default:
    return enable_arithmetic_flow &&
           isArithmeticFlowOpcode(opcode_parent->getOpcodeKind());
  }
}

} // namespace

GuardedValueFlowNode::GuardedValueFlowNode(Kind kind, Type *type,
                                           GuardedValueFlowGraph *graph,
                                           BasicBlock *block, Value *llvm_value,
                                           Instruction *dbg_inst)
    : GuardedValueFlowObject(Domain::Node), kind_(kind), type_(type),
      graph_(graph), block_(block),
      llvm_value_(llvm_value), dbg_inst_(dbg_inst) {}

void GuardedValueFlowNode::addChild(GuardedValueFlowNode *child,
                                    float confidence, ConditionRef condition) {
  if (!child)
    return;

  auto child_it =
      std::find_if(children_.begin(), children_.end(),
                   [&](const Edge &edge) { return edge.target == child; });
  auto parent_it =
      std::find_if(child->parents_.begin(), child->parents_.end(),
                   [&](const Edge &edge) { return edge.target == this; });

  if (child_it != children_.end()) {
    child_it->confidence = confidence;
    child_it->condition = condition;
    if (parent_it == child->parents_.end()) {
      child->parents_.push_back({this, confidence, condition});
    } else {
      parent_it->confidence = confidence;
      parent_it->condition = condition;
    }
    return;
  }

  children_.push_back({child, confidence, condition});
  if (parent_it != child->parents_.end()) {
    parent_it->confidence = confidence;
    parent_it->condition = condition;
    return;
  }
  child->parents_.push_back({this, confidence, condition});
}

void GuardedValueFlowNode::clearChildren() {
  for (const Edge &edge : children_) {
    if (!edge.target)
      continue;
    auto &parents = edge.target->parents_;
    parents.erase(std::remove_if(parents.begin(), parents.end(),
                                 [&](const Edge &parent_edge) {
                                   return parent_edge.target == this;
                                 }),
                  parents.end());
  }
  children_.clear();
}

void GuardedValueFlowOpcodeNode::addOperand(GuardedValueFlowNode *producer,
                                            float confidence,
                                            ConditionRef condition) {
  if (!producer)
    return;
  operands_.push_back({producer, static_cast<unsigned>(operands_.size()),
                       confidence, condition});
  GuardedValueFlowNode::addChild(producer, confidence, condition);
}

llvm::Error GuardedValueFlowOpcodeNode::setOperand(
    unsigned index, GuardedValueFlowNode *producer, float confidence,
    ConditionRef condition) {
  if (!producer)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "opcode operand producer is null");
  if (index > operands_.size())
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "opcode operand index %u is not contiguous (operand count %zu)",
        index, operands_.size());
  if (index == operands_.size()) {
    addOperand(producer, confidence, condition);
    return llvm::Error::success();
  }

  operands_[index] = {producer, index, confidence, condition};

  // Rebuild only the set-like dependency projection. This keeps reverse uses
  // consistent when the replaced producer is not referenced by another
  // operand occurrence.
  GuardedValueFlowNode::clearChildren();
  for (const OperandUse &operand : operands_)
    GuardedValueFlowNode::addChild(operand.producer, operand.confidence,
                                   operand.condition);
  return llvm::Error::success();
}

void GuardedValueFlowOpcodeNode::clearChildren() {
  operands_.clear();
  GuardedValueFlowNode::clearChildren();
}

bool GuardedValueFlowNode::containsParent(
    const GuardedValueFlowNode *parent) const {
  return std::any_of(parents_.begin(), parents_.end(),
                     [&](const Edge &edge) { return edge.target == parent; });
}

std::vector<GuardedValueFlowNode::OperandUser>
GuardedValueFlowNode::operandUsers() const {
  std::vector<OperandUser> result;
  for (const Edge &parent_edge : parents_) {
    auto *opcode = dyn_cast_or_null<GuardedValueFlowOpcodeNode>(
        parent_edge.target);
    if (!opcode)
      continue;
    for (const auto &operand : opcode->operands()) {
      if (operand.producer == this)
        result.push_back({opcode, operand.operand_index});
    }
  }
  return result;
}

std::vector<GuardedValueFlowNode *>
GuardedValueFlowNode::getValueFlowParents(bool enable_arithmetic_flow) const {
  std::vector<GuardedValueFlowNode *> result;
  for (const Edge &edge : parents_) {
    if (isValueFlowParent(this, edge.target, enable_arithmetic_flow))
      result.push_back(edge.target);
  }
  return result;
}

void GuardedValueFlowNode::addUseSite(GuardedValueFlowSite *site) {
  for (GuardedValueFlowSite *existing : use_sites_) {
    if (existing == site)
      return;
  }
  use_sites_.push_back(site);
}

void GuardedValueFlowNode::addMatchingRegion(GuardedValueFlowNode *producer,
                                             GuardedValueFlowRegionNode *region,
                                             ConditionRef provenance) {
  for (auto &existing : matching_regions_) {
    if (existing.producer == producer) {
      if (existing.region && region && existing.region != region && graph_) {
        existing.region = graph_->findOrCreateOrRegion(
            existing.region, region, getParentBasicBlock());
      } else if (!existing.region) {
        existing.region = region;
      }
      if (existing.provenance != provenance)
        existing.provenance = ConditionRef::none();
      return;
    }
  }
  matching_regions_.push_back({producer, region, provenance});
}

GuardedValueFlowRegionNode *GuardedValueFlowNode::getMatchingRegion(
    const GuardedValueFlowNode *producer) const {
  for (const auto &entry : matching_regions_) {
    if (entry.producer == producer)
      return entry.region;
  }
  return nullptr;
}

ConditionRef GuardedValueFlowNode::getMatchingCondition(
    const GuardedValueFlowNode *producer) const {
  for (const auto &entry : matching_regions_) {
    if (entry.producer == producer)
      return entry.provenance;
  }
  return ConditionRef::none();
}

void GuardedValueFlowPhiNode::addIncoming(GuardedValueFlowNode *value_node,
                                          BasicBlock *incoming_block,
                                          GuardedValueFlowNode *condition_node,
                                          bool condition_sense,
                                          ConditionRef condition) {
  incoming_.push_back(
      {value_node, incoming_block, condition_node, condition_sense, condition});
  addChild(value_node, 1.0f, condition);
}

void GuardedValueFlowReturnNode::addReturnValueSitePair(
    GuardedValueFlowNode *value_node, GuardedValueFlowReturnSite *site,
    ConditionRef guard) {
  if (!value_node)
    return;
  incoming_returns_.push_back({value_node, site, guard});
  if (std::none_of(children_.begin(), children_.end(), [&](const Edge &edge) {
        return edge.target == value_node;
      })) {
    GuardedValueFlowNode::addChild(value_node, 1.0f, guard);
  }
  if (site)
    addUseSite(site);
}

void GuardedValueFlowReturnNode::clearChildren() {
  incoming_returns_.clear();
  GuardedValueFlowNode::clearChildren();
}

GuardedValueFlowReturnSite *GuardedValueFlowReturnNode::getReturnSite(
    const GuardedValueFlowNode *value_node) const {
  for (auto it = incoming_returns_.rbegin(); it != incoming_returns_.rend();
       ++it) {
    if (it->value == value_node)
      return it->site;
  }
  return nullptr;
}
