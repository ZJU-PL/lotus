/// @file GuardedValueFlowGraph.cpp
/// @brief Implementation of GuardedValueFlowGraph — graph container and region
///        management
///
/// **Responsibilities:**
///   - Owns per-function node and site storage via `std::unique_ptr` vectors.
///   - Provides canonical constructor accessors for nodes of every kind
///     (arguments, operands, opcodes, regions, phi, memory, call/output,
///     summary, return).
///   - Manages value→node lookups (ordinary `Value *` and synthetic
///     interface `Value *` in separate namespaces).
///   - Manages site registries (call sites, return sites, synthetic guard
///     nodes for switch/branch conditions).
///   - Implements region-node composition: unit regions (from branch/switch
///     guards), AND / OR / NOT regions with constraint-state propagation
///     and complementary-region detection.
///   - Implements semantic region creation from `path_cond_t` pointers
///     (imported interface regions from callees, local semantic regions).
///   - Manages block-condition annotations and node→region assignment.
///   - Manages load-memory and store-memory node registries keyed by
///     instruction / (value, instruction) pairs.
///   - Tracks pseudo-argument, pseudo-return, and summary-node indices.
///   - Emits diagnostics for partial or degraded graphs.
///   - Provides query helpers for downstream consumers:
///     `getDirectDataDependencies`, `getDirectControlDependencies`,
///     `getEffectiveControlRegion`, `getMemoryProducers`, and
///     `getResolvedCallTargets`.
///
/// **Region complementarity**: Two unit regions with the same condition node
/// and opposite sense (true/false) are recognised as complementary; their
/// AND collapses to AlwaysFalse and their OR to AlwaysTrue.

#include "IR/GVFG/GuardedValueFlowGraph.h"

llvm::Function *
lotus::gvfg::GuardedValueFlowObject::getParentFunction() const {
  return getGraph()->getBaseFunction();
}

llvm::raw_ostream &lotus::gvfg::operator<<(
    llvm::raw_ostream &out, const GuardedValueFlowObject &object) {
  if (auto *value = object.getDebugValue())
    return out << *value;
  if (auto *inst = object.getDebugInstruction())
    return out << *inst;
  if (auto *node = llvm::dyn_cast<GuardedValueFlowNode>(&object))
    return out << node->getDescription();
  return out << "site " << object.getObjectId();
}

#include <algorithm>

#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::gvfg;

namespace {

static path_cond_t getImportedSource(path_cond_t cond) {
  if (!cond)
    return nullptr;
  if (cond->getKind() == PathCond::Kind::ImportedAtom)
    return cond->getImportedSource();
  return nullptr;
}

static Function *getInterfaceOriginFunction(path_cond_t cond) {
  if (!cond)
    return nullptr;

  if (path_cond_t imported = getImportedSource(cond))
    return imported->getOwnerFunc();

  if (cond->getKind() == PathCond::Kind::Not)
    return getInterfaceOriginFunction(cond->getLhs());

  if (cond->getKind() == PathCond::Kind::And ||
      cond->getKind() == PathCond::Kind::Or) {
    Function *lhs_origin = getInterfaceOriginFunction(cond->getLhs());
    Function *rhs_origin = getInterfaceOriginFunction(cond->getRhs());
    return lhs_origin == rhs_origin ? lhs_origin : nullptr;
  }

  return nullptr;
}

static std::string renderPathCond(path_cond_t cond) {
  if (!cond)
    return "<null>";
  std::string buffer;
  raw_string_ostream os(buffer);
  cond->print(os);
  return os.str();
}

static bool mergeConstraintStatesForAnd(
    const GuardedValueFlowRegionNode::ConstraintState &lhs,
    const GuardedValueFlowRegionNode::ConstraintState &rhs,
    GuardedValueFlowRegionNode::ConstraintState &out) {
  out.assignments = lhs.assignments;
  for (const auto &entry : rhs.assignments) {
    auto it = out.assignments.find(entry.first);
    if (it != out.assignments.end() && it->second != entry.second)
      return false;
    out.assignments[entry.first] = entry.second;
  }
  return true;
}

static GuardedValueFlowRegionNode::ConstraintState
intersectConstraintStatesForOr(
    const GuardedValueFlowRegionNode::ConstraintState &lhs,
    const GuardedValueFlowRegionNode::ConstraintState &rhs) {
  GuardedValueFlowRegionNode::ConstraintState out;
  for (const auto &entry : lhs.assignments) {
    auto it = rhs.assignments.find(entry.first);
    if (it != rhs.assignments.end() && it->second == entry.second)
      out.assignments[entry.first] = entry.second;
  }
  return out;
}

static bool areComplementaryRegions(const GuardedValueFlowRegionNode *lhs,
                                    const GuardedValueFlowRegionNode *rhs) {
  if (!lhs || !rhs)
    return false;

  if (lhs->getForm() == GuardedValueFlowRegionNode::Form::Unit &&
      rhs->getForm() == GuardedValueFlowRegionNode::Form::Unit &&
      lhs->getConditionNode() == rhs->getConditionNode() &&
      lhs->getConditionSense() != rhs->getConditionSense())
    return true;

  if (lhs->getForm() == GuardedValueFlowRegionNode::Form::Unit &&
      lhs->getConditionNode() == rhs && !lhs->getConditionSense())
    return true;

  if (rhs->getForm() == GuardedValueFlowRegionNode::Form::Unit &&
      rhs->getConditionNode() == lhs && !rhs->getConditionSense())
    return true;

  return false;
}

static BasicBlock *getRegionHelperBlock(GuardedValueFlowGraph &graph,
                                        BasicBlock *block) {
  if (block)
    return block;
  Function *base = graph.getBaseFunction();
  return (base && !base->empty()) ? &base->getEntryBlock() : nullptr;
}

static GuardedValueFlowNode *
findOrCreateBooleanLiteralNode(GuardedValueFlowGraph &graph, bool value,
                               BasicBlock *block) {
  auto *literal = ConstantInt::get(
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), value);
  if (auto *existing = graph.findNode(literal))
    return existing;

  auto *node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, literal->getType(), &graph,
      getRegionHelperBlock(graph, block), literal, nullptr);
  node->setDescription(value ? "true" : "false");
  graph.mapValueNode(literal, node);
  return node;
}

static GuardedValueFlowOpcodeNode *
createRegionBinaryOpcode(GuardedValueFlowGraph &graph,
                         GuardedValueFlowOpcodeNode::OpcodeKind opcode_kind,
                         BasicBlock *block, GuardedValueFlowNode *lhs,
                         GuardedValueFlowNode *rhs, StringRef description) {
  auto *opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::SimpleOpcode,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
      getRegionHelperBlock(graph, block), opcode_kind);
  opcode->setDescription(description.str());
  opcode->addChild(lhs);
  opcode->addChild(rhs);
  return opcode;
}

static GuardedValueFlowOpcodeNode *
createRegionNotOpcode(GuardedValueFlowGraph &graph, BasicBlock *block,
                      GuardedValueFlowNode *input) {
  auto *opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::SimpleOpcode,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
      getRegionHelperBlock(graph, block),
      GuardedValueFlowOpcodeNode::OpcodeKind::Xor);
  opcode->setDescription("region.not");
  opcode->setIntConstant(-1);
  opcode->addChild(input);
  return opcode;
}

} // namespace

GuardedValueFlowGraph::GuardedValueFlowGraph(Function *base_function)
    : base_function_(base_function) {}

GuardedValueFlowGraph::~GuardedValueFlowGraph() {
  for (Value *value : owned_synthetic_values_) {
    if (value)
      value->deleteValue();
  }
}

void GuardedValueFlowGraph::assignNodeRegion(GuardedValueFlowNode *node) {
  if (!node || node->getKind() == GuardedValueFlowNode::Kind::Region)
    return;

  BasicBlock *block = node->getParentBasicBlock();
  if (!block)
    return;

  GuardedValueFlowRegionNode *region = findRegion(block);
  if (!region && base_function_ && !base_function_->empty() &&
      block == &base_function_->getEntryBlock()) {
    region = getAlwaysTrueRegion();
  }

  if (region)
    node->region_ = region;
}

GuardedValueFlowNode *GuardedValueFlowGraph::findNode(Value *value) const {
  auto it = value_nodes_.find(value);
  return it == value_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapValueNode(Value *value,
                                         GuardedValueFlowNode *node) {
  if (value)
    value_nodes_[value] = node;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findInterfaceNode(Value *value) const {
  auto it = interface_nodes_.find(value);
  return it == interface_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapInterfaceNode(Value *value,
                                             GuardedValueFlowNode *node) {
  if (value)
    interface_nodes_[value] = node;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findPseudoArgumentBySource(Value *value) const {
  auto it = pseudo_argument_sources_.find(value);
  return it == pseudo_argument_sources_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapPseudoArgumentSource(
    Value *value, GuardedValueFlowNode *node) {
  if (value)
    pseudo_argument_sources_[value] = node;
}

Argument *GuardedValueFlowGraph::createSyntheticInterfaceValue(Type *type,
                                                               StringRef name) {
  auto *value = new Argument(type, name);
  owned_synthetic_values_.push_back(value);
  return value;
}

GuardedValueFlowCallSite *
GuardedValueFlowGraph::findCallSite(Instruction *inst) const {
  auto it = call_sites_.find(inst);
  return it == call_sites_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapCallSite(Instruction *inst,
                                        GuardedValueFlowCallSite *site) {
  if (inst)
    call_sites_[inst] = site;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findSyntheticGuardNode(Instruction *inst,
                                              BasicBlock *successor) const {
  auto it = synthetic_guard_nodes_.find(std::make_pair(inst, successor));
  return it == synthetic_guard_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapSyntheticGuardNode(Instruction *inst,
                                                  BasicBlock *successor,
                                                  GuardedValueFlowNode *node) {
  if (inst && successor)
    synthetic_guard_nodes_[std::make_pair(inst, successor)] = node;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findRegion(BasicBlock *block) const {
  auto it = regions_.find(block);
  return it == regions_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapRegion(BasicBlock *block,
                                      GuardedValueFlowRegionNode *node) {
  if (block)
    regions_[block] = node;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findUnitRegion(GuardedValueFlowNode *condition,
                                      bool sense) const {
  auto it = unit_regions_.find(std::make_pair(condition, sense));
  return it == unit_regions_.end() ? nullptr : it->second;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findOrCreateUnitRegion(GuardedValueFlowNode *condition,
                                              bool sense, BasicBlock *block,
                                              ConditionRef condition_ref) {
  if (!condition && sense)
    return getAlwaysTrueRegion();
  if (!condition && !sense)
    return getAlwaysFalseRegion();
  if (auto *condition_region =
          dyn_cast_or_null<GuardedValueFlowRegionNode>(condition)) {
    if (condition_region->isAlwaysTrue())
      return sense ? getAlwaysTrueRegion() : getAlwaysFalseRegion();
    if (condition_region->isAlwaysFalse())
      return sense ? getAlwaysFalseRegion() : getAlwaysTrueRegion();
  }

  if (auto *existing = findUnitRegion(condition, sense))
    return existing;

  const bool is_foreign_condition =
      condition && condition->getGraph() && condition->getGraph() != this;
  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      is_foreign_condition ? GuardedValueFlowRegionNode::Form::ImportedInterface
                           : GuardedValueFlowRegionNode::Form::Unit,
      condition, sense, condition_ref);

  if (is_foreign_condition) {
    GuardedValueFlowRegionNode::ConstraintState state;
    state.assignments[region] = true;
    state.assignments[condition] = sense;
    region->setConstraintState(std::move(state));

    path_cond_t path_cond = condition_ref.getPathCond();
    region->setInterfaceMetadata(
        path_cond ? path_cond->getOwnerFunc() : nullptr,
        condition->getGraph() ? condition->getGraph()->getBaseFunction()
                              : nullptr,
        path_cond, getImportedSource(path_cond));
  }

  if (!is_foreign_condition && condition) {
    if (sense) {
      region->addChild(condition, 1.0f, condition_ref);
    } else {
      auto *not_opcode = createRegionNotOpcode(*this, block, condition);
      region->addChild(not_opcode, 1.0f, condition_ref);
    }
  }
  unit_regions_[std::make_pair(condition, sense)] = region;
  return region;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findOrCreateAndRegion(GuardedValueFlowRegionNode *lhs,
                                             GuardedValueFlowRegionNode *rhs,
                                             BasicBlock *block) {
  if (!lhs)
    return rhs ? rhs : getAlwaysTrueRegion();
  if (!rhs)
    return lhs;
  if (lhs == rhs)
    return lhs;
  if (lhs->isAlwaysTrue())
    return rhs;
  if (rhs->isAlwaysTrue())
    return lhs;
  if (lhs->isAlwaysFalse())
    return lhs;
  if (rhs->isAlwaysFalse())
    return rhs;
  if (areComplementaryRegions(lhs, rhs))
    return getAlwaysFalseRegion();

  auto key = std::minmax(lhs, rhs);
  auto it = and_regions_.find(key);
  if (it != and_regions_.end())
    return it->second;

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::And, nullptr, true,
      ConditionRef::none());
  GuardedValueFlowRegionNode::ConstraintState merged_state;
  if (!mergeConstraintStatesForAnd(lhs->getConstraintState(),
                                   rhs->getConstraintState(), merged_state)) {
    and_regions_[key] = getAlwaysFalseRegion();
    return getAlwaysFalseRegion();
  }
  merged_state.assignments[region] = true;
  region->setConstraintState(std::move(merged_state));
  auto *and_opcode = createRegionBinaryOpcode(
      *this, GuardedValueFlowOpcodeNode::OpcodeKind::And, block, lhs, rhs,
      "region.and");
  region->addChild(and_opcode);
  and_regions_[key] = region;
  return region;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findOrCreateOrRegion(GuardedValueFlowRegionNode *lhs,
                                            GuardedValueFlowRegionNode *rhs,
                                            BasicBlock *block) {
  if (!lhs)
    return rhs ? rhs : getAlwaysTrueRegion();
  if (!rhs)
    return lhs;
  if (lhs == rhs)
    return lhs;
  if (lhs->isAlwaysTrue())
    return lhs;
  if (rhs->isAlwaysTrue())
    return rhs;
  if (lhs->isAlwaysFalse())
    return rhs;
  if (rhs->isAlwaysFalse())
    return lhs;
  if (areComplementaryRegions(lhs, rhs))
    return getAlwaysTrueRegion();

  auto key = std::minmax(lhs, rhs);
  auto it = or_regions_.find(key);
  if (it != or_regions_.end())
    return it->second;

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::Or, nullptr, true,
      ConditionRef::none());
  auto merged_state = intersectConstraintStatesForOr(lhs->getConstraintState(),
                                                     rhs->getConstraintState());
  merged_state.assignments[region] = true;
  region->setConstraintState(std::move(merged_state));
  auto *or_opcode = createRegionBinaryOpcode(
      *this, GuardedValueFlowOpcodeNode::OpcodeKind::Or, block, lhs, rhs,
      "region.or");
  region->addChild(or_opcode);
  or_regions_[key] = region;
  return region;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findOrCreateNotRegion(GuardedValueFlowRegionNode *input,
                                             BasicBlock *block) {
  if (!input)
    return getAlwaysFalseRegion();
  if (input->isAlwaysTrue())
    return getAlwaysFalseRegion();
  if (input->isAlwaysFalse())
    return getAlwaysTrueRegion();
  if (input->getForm() == GuardedValueFlowRegionNode::Form::Unit)
    return findOrCreateUnitRegion(input->getConditionNode(),
                                  !input->getConditionSense(), block,
                                  input->getRegionCondition());
  return findOrCreateUnitRegion(input, false, block, ConditionRef::none());
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::getAlwaysTrueRegion() {
  if (!always_true_region_) {
    always_true_region_ = createNode<GuardedValueFlowRegionNode>(
        Type::getInt1Ty(base_function_->getContext()), this, nullptr,
        GuardedValueFlowRegionNode::Form::AlwaysTrue, nullptr, true,
        ConditionRef::none());
    always_true_region_->addChild(
        findOrCreateBooleanLiteralNode(*this, true, nullptr));
  }
  return always_true_region_;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::getAlwaysFalseRegion() {
  if (!always_false_region_) {
    always_false_region_ = createNode<GuardedValueFlowRegionNode>(
        Type::getInt1Ty(base_function_->getContext()), this, nullptr,
        GuardedValueFlowRegionNode::Form::AlwaysFalse, nullptr, false,
        ConditionRef::none());
    always_false_region_->addChild(
        findOrCreateBooleanLiteralNode(*this, false, nullptr));
  }
  return always_false_region_;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findSemanticRegion(path_cond_t path_cond) const {
  auto it = semantic_regions_.find(path_cond);
  return it == semantic_regions_.end() ? nullptr : it->second;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findSemanticConditionNode(path_cond_t path_cond) const {
  auto it = semantic_condition_nodes_.find(path_cond);
  return it == semantic_condition_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapSemanticConditionNode(path_cond_t path_cond,
                                                     GuardedValueFlowNode *node) {
  if (path_cond && node)
    semantic_condition_nodes_[path_cond] = node;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::findOrCreateSemanticRegion(
    path_cond_t path_cond, BasicBlock *block,
    GuardedValueFlowNode *origin_condition_node) {
  if (!path_cond)
    return getAlwaysTrueRegion();

  if (auto *existing = findSemanticRegion(path_cond))
    return existing;

  auto condition = ConditionRef::fromPathCond(path_cond);
  Function *origin_function = getInterfaceOriginFunction(path_cond);
  bool is_imported_region =
      origin_function && origin_function != path_cond->getOwnerFunc();
  auto *condition_node = findSemanticConditionNode(path_cond);
  if (!condition_node) {
    if (is_imported_region && origin_condition_node) {
      condition_node = origin_condition_node;
    } else if (!is_imported_region && origin_condition_node &&
               origin_condition_node->getGraph() == this) {
      condition_node = origin_condition_node;
    } else {
      condition_node = createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::InterfaceCondition,
          Type::getInt1Ty(base_function_->getContext()), this, nullptr, nullptr,
          nullptr);
      condition_node->setDescription("semantic.cond:" +
                                     renderPathCond(path_cond));
    }
    semantic_condition_nodes_[path_cond] = condition_node;
  }

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      is_imported_region ? GuardedValueFlowRegionNode::Form::ImportedInterface
                         : GuardedValueFlowRegionNode::Form::Semantic,
      condition_node, true, condition);
  region->setDescription(is_imported_region ? "region.interface"
                                            : "region.semantic");
  region->setInterfaceMetadata(path_cond->getOwnerFunc(), origin_function,
                               path_cond, getImportedSource(path_cond));
  GuardedValueFlowRegionNode::ConstraintState state;
  state.assignments[region] = true;
  state.assignments[condition_node] = true;
  region->setConstraintState(std::move(state));
  if (!is_imported_region && condition_node->getGraph() == this) {
    region->addChild(condition_node, 1.0f, condition);
    condition_node->region_ = region;
  }
  semantic_regions_[path_cond] = region;
  return region;
}

void GuardedValueFlowGraph::addBlockCondition(BasicBlock *block,
                                              BlockCondition condition) {
  if (!block)
    return;
  block_conditions_[block].push_back(condition);
}

ArrayRef<GuardedValueFlowGraph::BlockCondition>
GuardedValueFlowGraph::getBlockConditions(BasicBlock *block) const {
  auto it = block_conditions_.find(block);
  if (it == block_conditions_.end())
    return {};
  return it->second;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findLoadMemoryNode(Instruction *inst) const {
  auto it = load_memory_nodes_.find(inst);
  return it == load_memory_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapLoadMemoryNode(Instruction *inst,
                                              GuardedValueFlowNode *node) {
  if (inst)
    load_memory_nodes_[inst] = node;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findStoreMemoryNode(Value *value,
                                           Instruction *inst) const {
  auto it = store_memory_nodes_.find(std::make_pair(value, inst));
  return it == store_memory_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapStoreMemoryNode(Value *value, Instruction *inst,
                                               GuardedValueFlowNode *node) {
  store_memory_nodes_[std::make_pair(value, inst)] = node;
}

GuardedValueFlowNode *GuardedValueFlowGraph::findOrCreateStoreMemoryNode(
    Value *value, Instruction *inst, Type *type, BasicBlock *block,
    StringRef description) {
  if (auto *existing = findStoreMemoryNode(value, inst))
    return existing;

  auto *node =
      createNode<GuardedValueFlowNode>(GuardedValueFlowNode::Kind::StoreMemory,
                                       type, this, block, nullptr, inst);
  node->setDescription(description.str());
  mapStoreMemoryNode(value, inst, node);
  return node;
}

GuardedValueFlowNode *GuardedValueFlowGraph::createAnonymousStoreMemoryNode(
    Type *type, BasicBlock *block, Instruction *inst, StringRef description) {
  auto *node =
      createNode<GuardedValueFlowNode>(GuardedValueFlowNode::Kind::StoreMemory,
                                       type, this, block, nullptr, inst);
  node->setDescription(description.str());
  return node;
}

GuardedValueFlowReturnSite *
GuardedValueFlowGraph::findReturnSite(Instruction *inst) const {
  auto it = return_sites_.find(inst);
  return it == return_sites_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapReturnSite(Instruction *inst,
                                          GuardedValueFlowReturnSite *site) {
  if (inst)
    return_sites_[inst] = site;
}

void GuardedValueFlowGraph::registerPseudoArgument(GuardedValueFlowNode *node) {
  if (!node)
    return;
  if (pseudo_arguments_.size() <= node->getIndex())
    pseudo_arguments_.resize(node->getIndex() + 1, nullptr);
  pseudo_arguments_[node->getIndex()] = node;
  compat_caches_dirty_ = true;
}

void GuardedValueFlowGraph::registerPseudoReturn(
    GuardedValueFlowReturnNode *node) {
  if (!node)
    return;
  if (pseudo_returns_.size() <= node->getIndex())
    pseudo_returns_.resize(node->getIndex() + 1, nullptr);
  pseudo_returns_[node->getIndex()] = node;
  compat_caches_dirty_ = true;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::getPseudoArgument(unsigned idx) const {
  return idx < pseudo_arguments_.size() ? pseudo_arguments_[idx] : nullptr;
}

GuardedValueFlowReturnNode *
GuardedValueFlowGraph::getPseudoReturn(unsigned idx) const {
  return idx < pseudo_returns_.size() ? pseudo_returns_[idx] : nullptr;
}

const GuardedValueFlowReturnNode *GuardedValueFlowGraph::getCommonReturn() const {
  for (const auto &node : nodes_) {
    if (node->getKind() == GuardedValueFlowNode::Kind::CommonReturn)
      return dyn_cast<GuardedValueFlowReturnNode>(node.get());
  }
  return nullptr;
}

unsigned GuardedValueFlowGraph::getNumCommonArgument() const {
  unsigned count = 0;
  for (const auto &node : nodes_) {
    if (node->getKind() == GuardedValueFlowNode::Kind::CommonArgument)
      ++count;
  }
  return count;
}

unsigned GuardedValueFlowGraph::getNumVarArgument() const {
  unsigned count = 0;
  for (const auto &node : nodes_) {
    if (node->getKind() == GuardedValueFlowNode::Kind::VariableArgument)
      ++count;
  }
  return count;
}

GuardedValueFlowNode *GuardedValueFlowGraph::getCommonArgument(unsigned idx) const {
  unsigned current = 0;
  for (const auto &node : nodes_) {
    if (node->getKind() != GuardedValueFlowNode::Kind::CommonArgument)
      continue;
    if (current == idx)
      return node.get();
    ++current;
  }
  return nullptr;
}

GuardedValueFlowNode *GuardedValueFlowGraph::getVarArgument(unsigned idx) const {
  unsigned current = 0;
  for (const auto &node : nodes_) {
    if (node->getKind() != GuardedValueFlowNode::Kind::VariableArgument)
      continue;
    if (current == idx)
      return node.get();
    ++current;
  }
  return nullptr;
}

std::vector<GuardedValueFlowNode *> GuardedValueFlowGraph::getCommonArguments() const {
  std::vector<GuardedValueFlowNode *> result;
  for (const auto &node : nodes_) {
    if (node->getKind() == GuardedValueFlowNode::Kind::CommonArgument)
      result.push_back(node.get());
  }
  return result;
}

std::vector<GuardedValueFlowReturnNode *> GuardedValueFlowGraph::getReturnNodes() const {
  std::vector<GuardedValueFlowReturnNode *> result;
  if (auto *common = const_cast<GuardedValueFlowReturnNode *>(getCommonReturn()))
    result.push_back(common);
  for (auto *node : pseudo_returns_)
    result.push_back(node);
  return result;
}

std::vector<GuardedValueFlowNode *> GuardedValueFlowGraph::arguments() const {
  std::vector<GuardedValueFlowNode *> result;
  for (const auto &node : nodes_) {
    if (node->getKind() == GuardedValueFlowNode::Kind::CommonArgument)
      result.push_back(node.get());
  }
  result.insert(result.end(), pseudo_arguments_.begin(),
                pseudo_arguments_.end());
  return result;
}

std::vector<GuardedValueFlowReturnNode *>
GuardedValueFlowGraph::returns() const {
  return getReturnNodes();
}

void GuardedValueFlowGraph::refreshCompatCaches() const {
  compat_arg_nodes_.clear();
  compat_return_nodes_.clear();

  for (const auto &node : nodes_) {
    if (node->getKind() == GuardedValueFlowNode::Kind::CommonArgument)
      compat_arg_nodes_.push_back(node.get());
  }
  for (auto *node : pseudo_arguments_)
    compat_arg_nodes_.push_back(node);

  if (auto *common = const_cast<GuardedValueFlowReturnNode *>(getCommonReturn()))
    compat_return_nodes_.push_back(common);
  for (auto *node : pseudo_returns_)
    compat_return_nodes_.push_back(node);
  compat_caches_dirty_ = false;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findFunctionSummaryArgumentNode(unsigned ap_depth,
                                                       Value *source) const {
  auto it =
      function_summary_argument_nodes_.find(std::make_pair(ap_depth, source));
  return it == function_summary_argument_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapFunctionSummaryArgumentNode(
    unsigned ap_depth, Value *source, GuardedValueFlowNode *node) {
  function_summary_argument_nodes_[std::make_pair(ap_depth, source)] = node;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findFunctionSummaryReturnNode(unsigned ap_depth) const {
  auto it = function_summary_return_nodes_.find(ap_depth);
  return it == function_summary_return_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapFunctionSummaryReturnNode(
    unsigned ap_depth, GuardedValueFlowNode *node) {
  function_summary_return_nodes_[ap_depth] = node;
}

void GuardedValueFlowGraph::resetFunctionSummaryInterface() {
  for (const auto &entry : function_summary_argument_nodes_) {
    if (entry.second)
      entry.second->clearChildren();
  }
  for (const auto &entry : function_summary_return_nodes_) {
    if (entry.second)
      entry.second->clearChildren();
  }
  summary_argument_nodes_.clear();
  summary_return_nodes_.clear();
}

void GuardedValueFlowGraph::registerSummaryArgumentNode(
    unsigned ap_depth, GuardedValueFlowNode *node) {
  if (!node)
    return;
  auto &nodes = summary_argument_nodes_[ap_depth];
  if (std::find(nodes.begin(), nodes.end(), node) == nodes.end())
    nodes.push_back(node);
}

void GuardedValueFlowGraph::registerSummaryReturnNode(
    unsigned ap_depth, GuardedValueFlowNode *node) {
  if (!node)
    return;
  auto &nodes = summary_return_nodes_[ap_depth];
  if (std::find(nodes.begin(), nodes.end(), node) == nodes.end())
    nodes.push_back(node);
}

ArrayRef<GuardedValueFlowNode *>
GuardedValueFlowGraph::getSummaryArgumentNodes(unsigned ap_depth) const {
  static const std::vector<GuardedValueFlowNode *> empty;
  auto it = summary_argument_nodes_.find(ap_depth);
  return it == summary_argument_nodes_.end()
             ? ArrayRef<GuardedValueFlowNode *>(empty)
             : ArrayRef<GuardedValueFlowNode *>(it->second);
}

ArrayRef<GuardedValueFlowNode *>
GuardedValueFlowGraph::getSummaryReturnNodes(unsigned ap_depth) const {
  static const std::vector<GuardedValueFlowNode *> empty;
  auto it = summary_return_nodes_.find(ap_depth);
  return it == summary_return_nodes_.end()
             ? ArrayRef<GuardedValueFlowNode *>(empty)
             : ArrayRef<GuardedValueFlowNode *>(it->second);
}

void GuardedValueFlowGraph::refreshNodeRegions() {
  for (const auto &node_ptr : nodes_)
    assignNodeRegion(node_ptr.get());
}

void GuardedValueFlowGraph::addDiagnostic(Diagnostic diagnostic) {
  if (!diagnostic.block && diagnostic.instruction)
    diagnostic.block = diagnostic.instruction->getParent();
  diagnostics_.push_back(std::move(diagnostic));
}

bool GuardedValueFlowGraph::isDegraded() const {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [](const Diagnostic &diagnostic) {
                       return diagnostic.severity != Diagnostic::Severity::Note;
                     });
}

std::vector<GuardedValueFlowNode *>
GuardedValueFlowGraph::getDirectDataDependencies(
    const GuardedValueFlowNode *node) const {
  std::vector<GuardedValueFlowNode *> result;
  if (!node)
    return result;

  for (const auto &edge : node->children()) {
    if (edge.target)
      result.push_back(edge.target);
  }

  if (auto *phi_node = dyn_cast<const GuardedValueFlowPhiNode>(node)) {
    for (const auto &incoming : phi_node->incoming()) {
      if (incoming.condition_node)
        result.push_back(incoming.condition_node);
    }
  }

  return result;
}

std::vector<GuardedValueFlowGraph::BlockCondition>
GuardedValueFlowGraph::getDirectControlDependencies(
    const GuardedValueFlowNode *node) const {
  if (!node || !node->getParentBasicBlock())
    return {};
  auto conditions = getBlockConditions(node->getParentBasicBlock());
  return std::vector<BlockCondition>(conditions.begin(), conditions.end());
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::getEffectiveControlRegion(
    const GuardedValueFlowNode *node) const {
  return node ? node->getRegion() : nullptr;
}

std::vector<GuardedValueFlowGraph::BlockCondition>
GuardedValueFlowGraph::getEffectiveControlDependencies(
    const GuardedValueFlowNode *node) const {
  return getDirectControlDependencies(node);
}

static GuardedValueFlowNode *
stripAdapterCoercions(GuardedValueFlowNode *node) {
  while (node && node->children().size() == 1) {
    bool is_adapter_cast =
        node->getKind() == GuardedValueFlowNode::Kind::CastOpcode &&
        node->getDescription() == "adapter.cast";
    bool is_adapter_coercion =
        node->getKind() == GuardedValueFlowNode::Kind::Unknown &&
        node->getDescription() == "adapter.coercion";
    if (!is_adapter_cast && !is_adapter_coercion)
      break;
    node = node->children().front().target;
  }
  return node;
}

std::vector<GuardedValueFlowGraph::MemoryProducer>
GuardedValueFlowGraph::getMemoryProducers(
    const GuardedValueFlowNode *node) const {
  std::vector<MemoryProducer> result;
  if (!node)
    return result;

  const GuardedValueFlowNode *memory_node = node;
  if (node->getKind() != GuardedValueFlowNode::Kind::LoadMemory &&
      node->children().size() == 1 && node->children().front().target &&
      node->children().front().target->getKind() ==
          GuardedValueFlowNode::Kind::LoadMemory) {
    memory_node = node->children().front().target;
  }

  if (!memory_node ||
      memory_node->getKind() != GuardedValueFlowNode::Kind::LoadMemory) {
    return result;
  }

  for (const auto &edge : memory_node->children()) {
    GuardedValueFlowNode *linked_producer = edge.target;
    GuardedValueFlowNode *producer_mem = stripAdapterCoercions(linked_producer);
    if (!producer_mem)
      continue;

    GuardedValueFlowNode *producer_value =
        producer_mem->children().empty()
            ? nullptr
            : stripAdapterCoercions(
                  producer_mem->children().front().target);
    bool is_summary =
        producer_value && producer_value->getKind() ==
                              GuardedValueFlowNode::Kind::CallSiteReturnSummary;
    bool is_unknown = producer_value && producer_value->getKind() ==
                                            GuardedValueFlowNode::Kind::Unknown;

    result.push_back({producer_mem, producer_value,
                      memory_node->getMatchingRegion(linked_producer),
                      edge.condition, edge.confidence, is_summary, is_unknown});
  }

  return result;
}

std::vector<GuardedValueFlowGraph::CallTargetInfo>
GuardedValueFlowGraph::getResolvedCallTargets(
    const GuardedValueFlowCallSite *site) const {
  std::vector<CallTargetInfo> result;
  if (!site)
    return result;

  for (Function *callee : site->getCallees()) {
    result.push_back({callee, site->getCalleeCondition(callee),
                      site->getCalleeConditionRegion(callee),
                      site->isBackEdge(callee)});
  }

  return result;
}
