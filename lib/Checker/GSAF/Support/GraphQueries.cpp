#include "Checker/GSAF/Support/GraphQueries.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace lotus::gsaf {

bool ObjectLess::operator()(const gvfg::GuardedValueFlowObject *left,
                            const gvfg::GuardedValueFlowObject *right) const {
  if (left == right)
    return false;
  if (!left || !right)
    return std::less<const gvfg::GuardedValueFlowObject *>()(left, right);
  if (left->getGraph() != right->getGraph())
    return std::less<gvfg::GuardedValueFlowGraph *>()(left->getGraph(),
                                                      right->getGraph());
  if (left->getObjectId() != right->getObjectId())
    return left->getObjectId() < right->getObjectId();
  return std::less<const gvfg::GuardedValueFlowObject *>()(left, right);
}

GuardedValueFlowCallSite *
callSite(const gvfg::GuardedValueFlowCallSummaryNode *node) {
  return node->getGraph()->findCallSite(node->getCallSite());
}

GuardedValueFlowCallSite *
callSite(const GuardedValueFlowCallOutputNode *node) {
  return node->getGraph()->findCallSite(node->getCallSite());
}

llvm::BasicBlock *immediateDominator(const llvm::DominatorTree &tree,
                                     llvm::BasicBlock *block) {
  auto *node = tree.getNode(block);
  return node && node->getIDom() ? node->getIDom()->getBlock() : nullptr;
}

bool hasContradictoryRegions(
    const std::vector<GuardedValueFlowRegionNode *> &regions,
    const GuardedValueFlowRegionNode *extra) {
  std::map<const GuardedValueFlowNode *, bool> assignments;
  auto contradictory = [&](const GuardedValueFlowRegionNode *region) {
    if (!region)
      return false;
    if (!region->isSatisfiable())
      return true;
    for (const auto &entry : region->getConstraintState().assignments) {
      auto inserted = assignments.emplace(entry.first, entry.second);
      if (!inserted.second && inserted.first->second != entry.second)
        return true;
    }
    return false;
  };
  for (auto *region : regions)
    if (contradictory(region))
      return true;
  return contradictory(extra);
}

bool isOperand(const GuardedValueFlowObject *object) {
  return llvm::isa<GuardedValueFlowNode>(object) &&
         !llvm::isa<GuardedValueFlowOpcodeNode>(object);
}

const GuardedValueFlowNode *operandNode(const GuardedValueFlowObject *object) {
  return isOperand(object) ? llvm::cast<GuardedValueFlowNode>(object) : nullptr;
}

const GuardedValueFlowSite *operationalSite(const GuardedValueFlowSite *site) {
  using K = GuardedValueFlowSite::Kind;
  return site && site->getKind() != K::CallSite &&
                 site->getKind() != K::ReturnSite &&
                 site->getKind() != K::Unknown
             ? site
             : nullptr;
}

GuardedValueFlowGraph *
graphFor(GuardedValueFlowGraphBuilderPass *builder, llvm::Function *function) {
  return function && builder->hasGraphFor(*function)
             ? &builder->getGraph(*function)
             : nullptr;
}

std::vector<GuardedValueFlowNode *>
valueFlowParents(const GuardedValueFlowNode *node, bool arithmetic) {
  auto result = node->getValueFlowParents(arithmetic);
  std::sort(result.begin(), result.end(), ObjectLess{});
  return result;
}

std::vector<GuardedValueFlowCallSite *>
callSites(const GuardedValueFlowGraph *graph) {
  std::vector<GuardedValueFlowCallSite *> result;
  for (const auto &site : graph->sites())
    if (auto *call = llvm::dyn_cast<GuardedValueFlowCallSite>(site.get()))
      result.push_back(call);
  return result;
}

std::vector<const GuardedValueFlowCallOutputNode *>
callOutputs(const GuardedValueFlowCallSite *site, llvm::Function *callee) {
  std::vector<const GuardedValueFlowCallOutputNode *> result;
  if (auto *output = site->getCommonOutput())
    result.push_back(llvm::cast<GuardedValueFlowCallOutputNode>(output));
  for (unsigned index = 0; index < site->getNumPseudoOutputs(callee); ++index)
    result.push_back(llvm::cast<GuardedValueFlowCallOutputNode>(
        site->getPseudoOutput(callee, index)));
  return result;
}

bool isGEPOpcode(const GuardedValueFlowNode *node) {
  auto *opcode = llvm::dyn_cast_or_null<GuardedValueFlowOpcodeNode>(node);
  return opcode && opcode->getOpcodeKind() ==
                       GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr;
}

llvm::Instruction *
gepMemoryUse(const GuardedValueFlowGEPReferenceSite *site) {
  auto *node = site->getGraph()->findNode(site->getInstruction());
  if (node)
    for (auto *use : node->useSites())
      if (llvm::isa<llvm::LoadInst>(use->getInstruction()) ||
          llvm::isa<llvm::StoreInst>(use->getInstruction()))
        return use->getInstruction();
  return nullptr;
}

std::vector<GuardedValueFlowNode *> children(const GuardedValueFlowNode *node) {
  std::vector<GuardedValueFlowNode *> result;
  if (auto *opcode = llvm::dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    for (const auto &operand : opcode->operands())
      result.push_back(operand.producer);
  } else {
    for (const auto &edge : node->children())
      result.push_back(edge.target);
  }
  return result;
}

std::string smtText(const SMTExprVec &values) {
  std::ostringstream stream;
  stream << "[";
  for (unsigned index = 0; index < values.size(); ++index) {
    if (index)
      stream << ", ";
    stream << values[index];
  }
  stream << "]";
  return stream.str();
}

std::pair<SMTExprVec, SMTExprVec>
ctrlDepsPair(GuardedValueFlowSolver &solver, llvm::BasicBlock *block,
             const GuardedValueFlowGraph *graph,
             llvm::BasicBlock *previous_block) {
  GuardedValueFlowSolver::QueryContext context;
  context.previous_block = previous_block;
  return solver.getCtrlDepsPair(block, graph, &context);
}

} // namespace lotus::gsaf
