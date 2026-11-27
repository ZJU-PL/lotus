/// @file LotusAAWrapper.cpp
/// @brief LotusAA wrapper — populates a GVFG with pointer-analysis results
///
/// For each function the wrapper:
///   1. **Memory matching**: walks every load-memory node, queries LotusAA's
///      per-function points-to results to find the set of store-memory nodes
///      that may reach it, and wires child edges with path-condition guards
///      and confidence scores.
///   2. **Call-site interface nodes**: creates PseudoInput, PseudoOutput, and
///      argument/return summary nodes per callee.  These nodes live in the
///      interface namespace (`mapInterfaceNode`) to avoid collisions with
///      ordinary SSA values.
///   3. **Escaped objects**: for each callee-escaped `MemObject`, creates a
///      concrete escape object in the caller's graph and maps it via the
///      callee's alloc-site.
///   4. **Output point-to linking**: wires pseudo-output point-to results
///      (from callee summaries) back to the caller's points-to targets.
///   5. **Output value linking**: stores pseudo-output values into the
///      appropriate caller-side object locators, equivalent to callee
///      side-effect propagation.
///   6. **Path-condition import**: creates `ImportedInterface` region nodes
///      for cross-function conditions obtained from LotusAA.
///   7. **Back-edge marking**: records back edges on callsites where LotusAA
///      detected a cycle in the call graph.
///   8. **Summary interface**: populates function-level summary argument and
///      return nodes grouped by access-path depth, enabling coarse-grained
///      inlining when the detailed interface exceeds configured limits.
///
/// The `safeLink` utility inserts a cast-opcode node for legal conversions and
/// an opaque coercion node otherwise, so imported dependencies are never
/// silently erased.

#include "IR/GVFG/LotusAAWrapper.h"

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GVFG/ConditionRef.h"

#include <string>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace lotus::gvfg;

#define DEBUG_TYPE "gvfg-lotus-aa-wrapper"

namespace {

static GuardedValueFlowOpcodeNode::OpcodeKind chooseCastOpcode(Type *src_ty,
                                                               Type *dst_ty) {
  if (!src_ty || !dst_ty)
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;

  if (auto *src_vec_ty = dyn_cast<VectorType>(src_ty)) {
    auto *dst_vec_ty = dyn_cast<VectorType>(dst_ty);
    if (!dst_vec_ty ||
        src_vec_ty->getElementCount() != dst_vec_ty->getElementCount())
      return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
    src_ty = src_vec_ty->getElementType();
    dst_ty = dst_vec_ty->getElementType();
  } else if (dst_ty->isVectorTy()) {
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
  }

  if (!src_ty->isSized() || !dst_ty->isSized())
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;

  const auto &src_bits = src_ty->getPrimitiveSizeInBits();
  const auto &dst_bits = dst_ty->getPrimitiveSizeInBits();

  if (dst_ty->isFloatingPointTy() && src_ty->isFloatingPointTy()) {
    if (dst_bits == src_bits)
      return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;
    return dst_bits > src_bits
               ? GuardedValueFlowOpcodeNode::OpcodeKind::FPExt
               : GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc;
  }
  if (dst_ty->isFloatingPointTy() && src_ty->isIntegerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP;
  if (dst_ty->isIntegerTy() && src_ty->isFloatingPointTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI;
  if (dst_ty->isIntegerTy() && src_ty->isIntegerTy()) {
    if (dst_bits == src_bits)
      return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;
    return dst_bits > src_bits ? GuardedValueFlowOpcodeNode::OpcodeKind::SExt
                               : GuardedValueFlowOpcodeNode::OpcodeKind::Trunc;
  }
  if (dst_ty->isPointerTy() && src_ty->isIntegerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr;
  if (dst_ty->isIntegerTy() && src_ty->isPointerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt;
  if (dst_ty->isPointerTy() && src_ty->isPointerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;

  return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
}

static BasicBlock *getEntryBlockOrNull(GuardedValueFlowGraph &graph) {
  Function *base = graph.getBaseFunction();
  return (base && !base->empty()) ? &base->getEntryBlock() : nullptr;
}

static void recordAdapterWarning(GuardedValueFlowGraph &graph,
                                 const Twine &message,
                                 Instruction *instruction = nullptr,
                                 BasicBlock *block = nullptr) {
  graph.addDiagnostic({GuardedValueFlowGraph::Diagnostic::Origin::Adapter,
                       GuardedValueFlowGraph::Diagnostic::Severity::Warning,
                       message.str(), instruction, block});
}

static BasicBlock *getFunctionEntryBlockOrNull(Function *func) {
  return (func && !func->empty()) ? &func->getEntryBlock() : nullptr;
}

static GuardedValueFlowNode *ensureValueNode(GuardedValueFlowGraph &graph,
                                             Value *value, Type *type,
                                             BasicBlock *bb,
                                             GuardedValueFlowNode::Kind kind,
                                             StringRef desc) {
  if (!value)
    return nullptr;

  if (auto *existing = graph.findNode(value))
    return existing;
  if (auto *existing = graph.findInterfaceNode(value))
    return existing;

  // The adapter may need to materialize values that were never created by the
  // structural builder, for example imported producers and synthetic interface
  // placeholders. Reuse either namespace if the value was already claimed.
  GuardedValueFlowNode *node = nullptr;
  if (kind == GuardedValueFlowNode::Kind::PseudoArgument ||
      kind == GuardedValueFlowNode::Kind::CommonArgument ||
      kind == GuardedValueFlowNode::Kind::VariableArgument) {
    node = graph.createNode<GuardedValueFlowArgumentNode>(
        kind, type ? type : value->getType(), &graph, bb, value);
  } else {
    node = graph.createNode<GuardedValueFlowNode>(
        kind, type ? type : value->getType(), &graph, bb, value,
        dyn_cast<Instruction>(value));
  }
  node->setDescription(desc.str());
  graph.mapValueNode(value, node);
  return node;
}

static GuardedValueFlowNode *
createInterfaceArgumentNode(GuardedValueFlowGraph &graph, Value *value,
                            Type *type, BasicBlock *bb, StringRef desc) {
  if (!value)
    return nullptr;

  auto *node = graph.createNode<GuardedValueFlowArgumentNode>(
      GuardedValueFlowNode::Kind::PseudoArgument,
      type ? type : value->getType(), &graph, bb, value);
  node->setDescription(desc.str());
  return node;
}

static GuardedValueFlowRegionNode *
translatePathCondToRegion(GuardedValueFlowGraphBuilderPass &builder,
                          GuardedValueFlowGraph &graph, path_cond_t cond,
                          BasicBlock *fallback_block);

static GuardedValueFlowNode *
resolveOriginConditionNode(GuardedValueFlowGraphBuilderPass &builder,
                           path_cond_t cond);
static GuardedValueFlowNode *
resolveProducerValueNode(GuardedValueFlowGraph &graph, Value *value, Type *type,
                         BasicBlock *bb);

static void
setAccessPathFromSegments(lotus::gvfg::AccessPath &path,
                          ArrayRef<std::pair<Value *, int64_t>> segments,
                          bool is_from_return) {
  path.reset(nullptr);
  for (size_t idx = 0; idx < segments.size(); ++idx) {
    const auto &segment = segments[idx];
    path.addLevel(segment.first, segment.second,
                  is_from_return && idx + 1 == segments.size());
  }
}

static void setNodeAccessPathFromValue(GuardedValueFlowNode *node,
                                       IntraLotusAA &pta, Value *value) {
  if (!node || !value)
    return;
  std::vector<std::pair<Value *, int64_t>> segments;
  bool is_from_return = false;
  pta.getFullAccessPath(value, segments, &is_from_return);
  setAccessPathFromSegments(node->getAccessPath(), segments, is_from_return);
}

static void setNodeAccessPathFromOutputIndex(GuardedValueFlowNode *node,
                                             IntraLotusAA &pta,
                                             unsigned output_index) {
  if (!node)
    return;
  std::vector<std::pair<Value *, int64_t>> segments;
  bool is_from_return = false;
  pta.getFullOutputAccessPath(static_cast<int>(output_index), segments,
                              &is_from_return);
  setAccessPathFromSegments(node->getAccessPath(), segments, is_from_return);
}

static Type *getStableSummaryNodeType(LLVMContext &ctx) {
  return PTGraph::DEFAULT_NON_POINTER_TYPE ? PTGraph::DEFAULT_NON_POINTER_TYPE
                                           : Type::getInt64Ty(ctx);
}

static mem_value_t makeFallbackValues(Value *value, Instruction *pos = nullptr,
                                      float confidence = 1.0f,
                                      path_cond_t cond = nullptr) {
  mem_value_t fallback;
  fallback.emplace_back(cond, pos, value, confidence);
  return fallback;
}

enum class SummaryDirection {
  Input,
  Output,
};

enum class SummaryValueMode {
  CallsiteProducer,
  OpaqueProducer,
};

struct SummarySentinelProvenance {
  Instruction *callsite{nullptr};
  Function *callee{nullptr};
  unsigned bucket{0};
  SummaryDirection direction{SummaryDirection::Output};
};

static bool setAdapterFailure(std::string &failure, const Twine &message) {
  failure = message.str();
  return false;
}

using StorePointerIndex = DenseMap<const Value *, SmallVector<StoreInst *, 4>>;

static bool instructionReachesInstruction(IntraLotusAA &pta,
                                          const Instruction *src,
                                          const Instruction *dst) {
  if (!src || !dst)
    return false;

  const BasicBlock *src_bb = src->getParent();
  const BasicBlock *dst_bb = dst->getParent();
  if (!src_bb || !dst_bb)
    return false;

  if (src_bb == dst_bb)
    return src != dst && src->comesBefore(dst);

  return pta.blockReaches(src_bb, dst_bb);
}

static void collectExactPointerStoreFallbacks(
    IntraLotusAA &pta, LoadInst *load, mem_value_t &values,
    StorePointerIndex &stores_by_ptr, bool &stores_indexed) {
  if (!load)
    return;

  Value *load_ptr = load->getPointerOperand();
  if (!load_ptr)
    return;

  if (!stores_indexed) {
    stores_indexed = true;
    for (Instruction &inst : instructions(*pta.getFunc())) {
      if (auto *store = dyn_cast<StoreInst>(&inst)) {
        if (Value *store_ptr = store->getPointerOperand())
          stores_by_ptr[store_ptr].push_back(store);
      }
    }
  }

  auto it = stores_by_ptr.find(load_ptr);
  if (it == stores_by_ptr.end())
    return;

  for (StoreInst *store : it->second) {
    if (!instructionReachesInstruction(pta, store, load))
      continue;
    values.emplace_back(nullptr, store, store->getValueOperand(), 1.0f);
  }
}

static GuardedValueFlowReturnSite *
findOrCreateReturnSite(GuardedValueFlowGraph &graph, ReturnInst *ret) {
  if (!ret)
    return nullptr;
  if (auto *existing = graph.findReturnSite(ret))
    return existing;
  auto *site = graph.createSite<GuardedValueFlowReturnSite>(&graph, ret);
  graph.mapReturnSite(ret, site);
  return site;
}

static GuardedValueFlowNode *createSpecialProducerNode(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode::Kind kind, Type *type,
    BasicBlock *bb, Instruction *inst, StringRef description) {
  auto *node = graph.createNode<GuardedValueFlowNode>(kind, type, &graph, bb,
                                                      nullptr, inst);
  node->setDescription(description.str());
  return node;
}

static GuardedValueFlowNode *createSummaryProducerNode(
    GuardedValueFlowGraph &graph, Type *type, BasicBlock *bb,
    Instruction *producer_inst, SummaryValueMode summary_value_mode,
    const SummarySentinelProvenance *provenance = nullptr) {
  Instruction *callsite = producer_inst;
  Function *callee = nullptr;
  unsigned bucket = 0;
  SummaryDirection direction = SummaryDirection::Output;
  if (provenance) {
    if (!callsite)
      callsite = provenance->callsite;
    callee = provenance->callee;
    bucket = provenance->bucket;
    direction = provenance->direction;
  }
  if (!callee) {
    if (auto *call = dyn_cast_or_null<CallBase>(callsite))
      callee = call->getCalledFunction();
  }

  auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary, type, &graph, bb,
      callsite, callee, bucket);
  // Summary sentinels stand in for imported or intentionally opaque producers.
  // They are separate from ordinary value nodes so later consumers can tell
  // exact interface channels from summarized channels.
  summary_node->setDescription(
      direction == SummaryDirection::Input
          ? "summary.input.value"
          : (summary_value_mode == SummaryValueMode::OpaqueProducer
                 ? "summary.value"
                 : "summary.output.value"));
  return summary_node;
}

static GuardedValueFlowNode *
detachReusableLoadMemoryChild(GuardedValueFlowNode *node,
                              StringRef description) {
  if (!node)
    return nullptr;

  GuardedValueFlowNode *load_mem = nullptr;
  if (node->children().size() == 1) {
    auto *candidate = node->children().front().target;
    if (candidate &&
        candidate->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      load_mem = candidate;
    }
  }

  node->clearChildren();
  if (!load_mem)
    return nullptr;

  load_mem->clearChildren();
  load_mem->clearMatchingRegions();
  load_mem->setDescription(description.str());
  return load_mem;
}

static BasicBlock *getValueBlockOrEntry(GuardedValueFlowGraph &graph,
                                        Value *value) {
  if (auto *inst = dyn_cast_or_null<Instruction>(value))
    return inst->getParent();
  return getEntryBlockOrNull(graph);
}

static GuardedValueFlowNode *
findOrCreateConditionNode(GuardedValueFlowGraph &graph, Value *value) {
  if (!value)
    return nullptr;
  if (auto *existing = graph.findNode(value))
    return existing;
  if (auto *existing = graph.findInterfaceNode(value))
    return existing;
  return ensureValueNode(
      graph, value, value->getType(), getValueBlockOrEntry(graph, value),
      GuardedValueFlowNode::Kind::SimpleOperand, "path.cond");
}

static GuardedValueFlowRegionNode *
translateImportedPathCondToRegion(GuardedValueFlowGraphBuilderPass &builder,
                                  GuardedValueFlowGraph &graph,
                                  path_cond_t cond,
                                  BasicBlock *fallback_block);

static GuardedValueFlowNode *
findOrCreateCallTargetPredicateNode(GuardedValueFlowGraph &graph,
                                    path_cond_t cond) {
  if (!cond || cond->getKind() != PathCond::Kind::CallTargetAtom ||
      !cond->getValue() || !cond->getCallee()) {
    return nullptr;
  }

  if (auto *existing = graph.findSemanticConditionNode(cond))
    return existing;

  auto *called_value_node = findOrCreateConditionNode(graph, cond->getValue());
  auto *callee_node =
      ensureValueNode(graph, cond->getCallee(), cond->getCallee()->getType(),
                      getEntryBlockOrNull(graph),
                      GuardedValueFlowNode::Kind::SimpleOperand,
                      "call.target.callee");
  if (!called_value_node || !callee_node)
    return nullptr;

  BasicBlock *block = called_value_node->getParentBasicBlock()
                          ? called_value_node->getParentBasicBlock()
                          : getValueBlockOrEntry(graph, cond->getValue());
  auto *cmp_opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::SimpleOpcode,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph, block,
      GuardedValueFlowOpcodeNode::OpcodeKind::ICmp);
  cmp_opcode->setDescription("call.target.eq");
  cmp_opcode->setCmpPredicate(CmpInst::ICMP_EQ);
  cmp_opcode->addChild(callee_node);
  cmp_opcode->addChild(called_value_node);

  auto *cmp_value = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph, block,
      nullptr, dyn_cast<Instruction>(cond->getValue()));
  cmp_value->setDescription("call.target.guard");
  cmp_value->addChild(cmp_opcode);
  graph.mapSemanticConditionNode(cond, cmp_value);
  return cmp_value;
}

static GuardedValueFlowNode *
createSwitchCaseCompare(GuardedValueFlowGraph &graph, SwitchInst *switch_inst,
                        ConstantInt *case_value) {
  if (!switch_inst || !case_value)
    return nullptr;

  auto *switch_value =
      findOrCreateConditionNode(graph, switch_inst->getCondition());
  auto *case_node = findOrCreateConditionNode(graph, case_value);
  if (!switch_value || !case_node)
    return nullptr;

  auto *cmp_opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::SimpleOpcode,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
      switch_inst->getParent(), GuardedValueFlowOpcodeNode::OpcodeKind::ICmp);
  cmp_opcode->setDescription("switch.case.eq");
  cmp_opcode->setCmpPredicate(CmpInst::ICMP_EQ);
  cmp_opcode->addChild(switch_value);
  cmp_opcode->addChild(case_node);

  auto *cmp_value = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
      switch_inst->getParent(), nullptr, switch_inst);
  cmp_value->setDescription("switch.case.guard");
  cmp_value->addChild(cmp_opcode);
  return cmp_value;
}

static GuardedValueFlowNode *
findOrCreateSwitchPredicateNode(GuardedValueFlowGraph &graph, path_cond_t cond) {
  if (!cond || (cond->getKind() != PathCond::Kind::SwitchCaseAtom &&
                cond->getKind() != PathCond::Kind::SwitchDefaultAtom)) {
    return nullptr;
  }

  auto *switch_inst = cond->getBlock()
                          ? dyn_cast_or_null<SwitchInst>(
                                cond->getBlock()->getTerminator())
                          : nullptr;
  BasicBlock *successor = cond->getSuccessor();
  if (!switch_inst || !successor)
    return nullptr;

  if (auto *existing = graph.findSyntheticGuardNode(switch_inst, successor))
    return existing;

  GuardedValueFlowNode *predicate = nullptr;
  SmallVector<ConstantInt *, 4> matching_cases;
  for (const auto &case_it : switch_inst->cases()) {
    if (case_it.getCaseSuccessor() == successor)
      matching_cases.push_back(case_it.getCaseValue());
  }

  if (!matching_cases.empty()) {
    for (ConstantInt *case_value : matching_cases) {
      auto *case_guard =
          createSwitchCaseCompare(graph, switch_inst, case_value);
      if (!case_guard)
        return nullptr;
      if (!predicate) {
        predicate = case_guard;
        continue;
      }

      auto *or_opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
          GuardedValueFlowNode::Kind::SimpleOpcode,
          Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
          switch_inst->getParent(), GuardedValueFlowOpcodeNode::OpcodeKind::Or);
      or_opcode->setDescription("switch.case.or");
      or_opcode->addChild(predicate);
      or_opcode->addChild(case_guard);

      auto *or_value = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::SimpleOperand,
          Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
          switch_inst->getParent(), nullptr, switch_inst);
      or_value->setDescription("switch.case.guard");
      or_value->addChild(or_opcode);
      predicate = or_value;
    }
  } else if (switch_inst->getDefaultDest() == successor) {
    SmallVector<GuardedValueFlowNode *, 4> case_guards;
    for (const auto &case_it : switch_inst->cases()) {
      if (auto *case_guard =
              createSwitchCaseCompare(graph, switch_inst, case_it.getCaseValue()))
        case_guards.push_back(case_guard);
    }

    if (case_guards.empty()) {
      predicate = findOrCreateConditionNode(
          graph, ConstantInt::getTrue(graph.getBaseFunction()->getContext()));
    } else {
      GuardedValueFlowNode *covered = case_guards.front();
      for (size_t idx = 1; idx < case_guards.size(); ++idx) {
        auto *or_opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
            GuardedValueFlowNode::Kind::SimpleOpcode,
            Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
            switch_inst->getParent(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Or);
        or_opcode->setDescription("switch.default.covered");
        or_opcode->addChild(covered);
        or_opcode->addChild(case_guards[idx]);

        auto *or_value = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::SimpleOperand,
            Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
            switch_inst->getParent(), nullptr, switch_inst);
        or_value->setDescription("switch.default.covered");
        or_value->addChild(or_opcode);
        covered = or_value;
      }

      auto *not_opcode = graph.createNode<GuardedValueFlowOpcodeNode>(
          GuardedValueFlowNode::Kind::SimpleOpcode,
          Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
          switch_inst->getParent(),
          GuardedValueFlowOpcodeNode::OpcodeKind::Xor);
      not_opcode->setDescription("switch.default.not");
      not_opcode->setIntConstant(-1);
      not_opcode->addChild(covered);

      auto *not_value = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::SimpleOperand,
          Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
          switch_inst->getParent(), nullptr, switch_inst);
      not_value->setDescription("switch.default.guard");
      not_value->addChild(not_opcode);
      predicate = not_value;
    }
  }

  if (predicate)
    graph.mapSyntheticGuardNode(switch_inst, successor, predicate);
  return predicate;
}

static GuardedValueFlowNode *
findOrCreateInvokePredicateNode(GuardedValueFlowGraph &graph, path_cond_t cond) {
  if (!cond || (cond->getKind() != PathCond::Kind::InvokeNormalAtom &&
                cond->getKind() != PathCond::Kind::InvokeUnwindAtom)) {
    return nullptr;
  }

  auto *invoke_inst = cond->getBlock()
                          ? dyn_cast_or_null<InvokeInst>(
                                cond->getBlock()->getTerminator())
                          : nullptr;
  BasicBlock *successor = cond->getSuccessor();
  if (!invoke_inst || !successor)
    return nullptr;

  if (auto *existing = graph.findSyntheticGuardNode(invoke_inst, successor))
    return existing;

  auto *node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      Type::getInt1Ty(graph.getBaseFunction()->getContext()), &graph,
      invoke_inst->getParent(), nullptr, invoke_inst);
  node->setDescription(cond->getKind() == PathCond::Kind::InvokeNormalAtom
                           ? "invoke.normal.guard"
                           : "invoke.unwind.guard");
  graph.mapSyntheticGuardNode(invoke_inst, successor, node);
  return node;
}

static GuardedValueFlowRegionNode *
translateLocalPathCondToRegion(GuardedValueFlowGraphBuilderPass &builder,
                               GuardedValueFlowGraph &graph, path_cond_t cond,
                               BasicBlock *fallback_block) {
  if (!cond)
    return graph.getAlwaysTrueRegion();

  Function *owner = cond->getOwnerFunc();
  if (owner && owner != graph.getBaseFunction())
    return nullptr;

  // Rebuild local path conditions out of structural region algebra when
  // possible. This preserves the same region identity that the structural
  // builder would have produced for in-function guards.
  switch (cond->getKind()) {
  case PathCond::Kind::True:
    return graph.getAlwaysTrueRegion();
  case PathCond::Kind::False:
    return graph.getAlwaysFalseRegion();
  case PathCond::Kind::ValueAtom: {
    auto *condition_node = findOrCreateConditionNode(graph, cond->getValue());
    if (!condition_node)
      return nullptr;
    BasicBlock *block = condition_node->getParentBasicBlock()
                            ? condition_node->getParentBasicBlock()
                            : getValueBlockOrEntry(graph, cond->getValue());
    return graph.findOrCreateUnitRegion(condition_node, cond->getSense(), block,
                                        ConditionRef::fromPathCond(cond));
  }
  case PathCond::Kind::BranchAtom: {
    auto *condition_node = findOrCreateConditionNode(graph, cond->getValue());
    if (!condition_node)
      return nullptr;
    BasicBlock *block = cond->getSuccessor();
    if (!block)
      block = cond->getBlock() ? cond->getBlock() : fallback_block;
    return graph.findOrCreateUnitRegion(condition_node, cond->getSense(), block,
                                        ConditionRef::fromPathCond(cond));
  }
  case PathCond::Kind::BlockAtom:
    if (cond->getBlock() == nullptr)
      return graph.getAlwaysTrueRegion();
    if (auto *region = graph.findRegion(cond->getBlock()))
      return region;
    if (graph.getBaseFunction() && !graph.getBaseFunction()->empty() &&
        cond->getBlock() == &graph.getBaseFunction()->getEntryBlock()) {
      return graph.getAlwaysTrueRegion();
    }
    return nullptr;
  case PathCond::Kind::Not: {
    auto *input =
        translatePathCondToRegion(builder, graph, cond->getLhs(), fallback_block);
    return input ? graph.findOrCreateNotRegion(input, fallback_block) : nullptr;
  }
  case PathCond::Kind::And: {
    auto *lhs =
        translatePathCondToRegion(builder, graph, cond->getLhs(), fallback_block);
    auto *rhs =
        translatePathCondToRegion(builder, graph, cond->getRhs(), fallback_block);
    if (!lhs || !rhs)
      return nullptr;
    return graph.findOrCreateAndRegion(lhs, rhs, fallback_block);
  }
  case PathCond::Kind::Or: {
    auto *lhs =
        translatePathCondToRegion(builder, graph, cond->getLhs(), fallback_block);
    auto *rhs =
        translatePathCondToRegion(builder, graph, cond->getRhs(), fallback_block);
    if (!lhs || !rhs)
      return nullptr;
    return graph.findOrCreateOrRegion(lhs, rhs, fallback_block);
  }
  case PathCond::Kind::ImportedAtom:
    return translateImportedPathCondToRegion(builder, graph, cond,
                                             fallback_block);
  case PathCond::Kind::CallTargetAtom: {
    auto *predicate = findOrCreateCallTargetPredicateNode(graph, cond);
    BasicBlock *block =
        fallback_block ? fallback_block : getValueBlockOrEntry(graph, cond->getValue());
    return predicate ? graph.findOrCreateUnitRegion(
                           predicate, true, block, ConditionRef::fromPathCond(cond))
                     : nullptr;
  }
  case PathCond::Kind::SwitchCaseAtom:
  case PathCond::Kind::SwitchDefaultAtom: {
    auto *predicate = findOrCreateSwitchPredicateNode(graph, cond);
    BasicBlock *block = cond->getSuccessor();
    if (!block)
      block = cond->getBlock() ? cond->getBlock() : fallback_block;
    return predicate ? graph.findOrCreateUnitRegion(
                           predicate, true, block, ConditionRef::fromPathCond(cond))
                     : nullptr;
  }
  case PathCond::Kind::InvokeNormalAtom:
  case PathCond::Kind::InvokeUnwindAtom: {
    auto *predicate = findOrCreateInvokePredicateNode(graph, cond);
    BasicBlock *block = cond->getSuccessor();
    if (!block)
      block = cond->getBlock() ? cond->getBlock() : fallback_block;
    return predicate ? graph.findOrCreateUnitRegion(
                           predicate, true, block, ConditionRef::fromPathCond(cond))
                     : nullptr;
  }
  }
  return nullptr;
}

static GuardedValueFlowRegionNode *
translateImportedPathCondToRegion(GuardedValueFlowGraphBuilderPass &builder,
                                  GuardedValueFlowGraph &graph, path_cond_t cond,
                                  BasicBlock *fallback_block) {
  path_cond_t imported_source =
      cond && cond->getKind() == PathCond::Kind::ImportedAtom
          ? cond->getImportedSource()
          : nullptr;
  if (!imported_source)
    return nullptr;

  Function *origin_func = imported_source->getOwnerFunc();
  if (!origin_func || !builder.hasGraphFor(*origin_func))
    return nullptr;

  auto &origin_graph = builder.getGraph(*origin_func);
  auto *origin_region = translatePathCondToRegion(
      builder, origin_graph, imported_source,
      getFunctionEntryBlockOrNull(origin_func));
  if (!origin_region)
    return nullptr;

  BasicBlock *block = fallback_block ? fallback_block : getEntryBlockOrNull(graph);
  return graph.findOrCreateUnitRegion(origin_region, true, block,
                                      ConditionRef::fromPathCond(cond));
}

static GuardedValueFlowRegionNode *
translatePathCondToRegion(GuardedValueFlowGraphBuilderPass &builder,
                          GuardedValueFlowGraph &graph, path_cond_t cond,
                          BasicBlock *fallback_block) {
  if (!cond)
    return graph.getAlwaysTrueRegion();
  if (auto *cached = graph.findTranslatedPathCond(cond))
    return cached;
  if (auto *structural =
          translateLocalPathCondToRegion(builder, graph, cond, fallback_block)) {
    graph.mapTranslatedPathCond(cond, structural);
    return structural;
  }
  // Fall back to a semantic/interface region when the path condition cannot be
  // reconstructed as a purely local structural guard.
  auto *semantic = graph.findOrCreateSemanticRegion(
      cond, fallback_block, resolveOriginConditionNode(builder, cond));
  graph.mapTranslatedPathCond(cond, semantic);
  return semantic;
}

static GuardedValueFlowNode *ensureStoreMemoryNode(
    GuardedValueFlowGraph &graph, Value *value, Instruction *inst,
    Type *memory_type, BasicBlock *bb,
    SummaryValueMode summary_value_mode = SummaryValueMode::CallsiteProducer,
    const SummarySentinelProvenance *summary_provenance = nullptr) {
  if (value == LocValue::FREE_VARIABLE || value == LocValue::NO_VALUE)
    return nullptr;

  const bool is_anonymous_special =
      value == LocValue::UNDEF_VALUE || value == LocValue::SUMMARY_VALUE;
  if (!is_anonymous_special) {
    if (auto *existing = graph.findStoreMemoryNode(value, inst))
      return existing;
  }

  auto *mem_node = is_anonymous_special
                       ? graph.createAnonymousStoreMemoryNode(
                             memory_type, bb, inst, "store.mem.adapter")
                       : graph.findOrCreateStoreMemoryNode(
                             value, inst, memory_type, bb, "store.mem.adapter");

  // Each store-memory node owns the producer chain for one memory fact. Special
  // values use anonymous memory nodes so multiple imported sentinels do not
  // overwrite one another in the keyed store-memory map.
  if (value == LocValue::UNDEF_VALUE) {
    (void)LotusAAWrapper::safeLink(
        graph, mem_node,
        createSpecialProducerNode(graph, GuardedValueFlowNode::Kind::UndefValue,
                                  memory_type, bb, inst, "undef.value"));
  } else if (value == LocValue::SUMMARY_VALUE) {
    auto *summary_node = createSummaryProducerNode(
        graph, memory_type, bb, inst, summary_value_mode, summary_provenance);
    (void)LotusAAWrapper::safeLink(graph, mem_node,
                                                     summary_node);
  } else {
    auto *value_node = resolveProducerValueNode(graph, value, memory_type, bb);
    (void)LotusAAWrapper::safeLink(graph, mem_node,
                                                     value_node);
  }

  return mem_node;
}

static GuardedValueFlowRegionNode *resolveMatchingRegion(
    GuardedValueFlowGraphBuilderPass &builder, GuardedValueFlowGraph &graph,
    GuardedValueFlowNode *producer_mem, const mem_value_item_t &item) {
  if (item.cond)
    return translatePathCondToRegion(
        builder, graph, item.cond,
        producer_mem ? producer_mem->getParentBasicBlock() : nullptr);
  if (producer_mem && producer_mem->getRegion())
    return producer_mem->getRegion();
  return graph.getAlwaysTrueRegion();
}

static void linkMemoryValue(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode *load_mem_node,
    const mem_value_item_t &item, BasicBlock *bb,
    GuardedValueFlowGraphBuilderPass &builder,
    SummaryValueMode summary_value_mode = SummaryValueMode::CallsiteProducer,
    const SummarySentinelProvenance *summary_provenance = nullptr) {
  Value *value = item.val;
  auto cond =
      item.cond ? ConditionRef::fromPathCond(item.cond) : ConditionRef::none();
  Instruction *producer_inst = item.pos;
  auto *producer_mem = ensureStoreMemoryNode(
      graph, value, producer_inst, load_mem_node->getType(), bb,
      summary_value_mode, summary_provenance);
  if (!producer_mem)
    return;
  auto *linked_producer = LotusAAWrapper::safeLink(
      graph, load_mem_node, producer_mem, item.confidence, cond);
  if (!linked_producer)
    return;
  // Matching regions are recorded on the load-memory node even when the actual
  // incoming edge has to pass through an adapter-inserted cast.
  load_mem_node->addMatchingRegion(
      linked_producer,
      resolveMatchingRegion(builder, graph, producer_mem, item), cond);
}

static void populateLoadMemoryNode(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode *load_mem_node,
    const mem_value_t &values, BasicBlock *bb,
    GuardedValueFlowGraphBuilderPass &builder,
    SummaryValueMode summary_value_mode = SummaryValueMode::CallsiteProducer,
    const SummarySentinelProvenance *summary_provenance = nullptr) {
  load_mem_node->clearChildren();
  load_mem_node->clearMatchingRegions();
  for (const auto &item : values)
    linkMemoryValue(graph, load_mem_node, item, bb, builder, summary_value_mode,
                    item.val == LocValue::SUMMARY_VALUE ? summary_provenance
                                                        : nullptr);
}

static void populateUnknownLoadMemoryNode(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode *load_mem_node,
    LoadInst *load) {
  load_mem_node->clearChildren();
  load_mem_node->clearMatchingRegions();

  auto *unknown = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::Unknown, load_mem_node->getType(), &graph,
      load->getParent(), nullptr, load);
  unknown->setDescription("memory.unknown");
  auto *producer_mem = graph.createAnonymousStoreMemoryNode(
      load_mem_node->getType(), load->getParent(), load,
      "store.mem.unknown");
  (void)LotusAAWrapper::safeLink(graph, producer_mem,
                                                   unknown);
  auto *linked = LotusAAWrapper::safeLink(
      graph, load_mem_node, producer_mem);
  if (linked)
    load_mem_node->addMatchingRegion(linked, graph.getAlwaysTrueRegion());

  recordAdapterWarning(
      graph,
      Twine("No reaching producer was available for load; using an unknown ") +
          "memory producer",
      load, load->getParent());
}

static mem_value_t importSummaryValues(IntraLotusAA &pta, Instruction *callsite,
                                       Function *callee,
                                       const mem_value_t &summary_values) {
  mem_value_t imported;
  imported.reserve(summary_values.size());
  for (const auto &item : summary_values) {
    path_cond_t imported_cond =
        item.cond ? pta.importSummaryCond(item.cond, callsite, callee)
                  : nullptr;
    imported.emplace_back(imported_cond, item.pos, item.val, item.confidence);
  }
  return imported;
}

static path_cond_t getImportedSource(path_cond_t cond) {
  if (!cond || cond->getKind() != PathCond::Kind::ImportedAtom)
    return nullptr;
  return cond->getImportedSource();
}

static GuardedValueFlowNode *
resolveOriginConditionNode(GuardedValueFlowGraphBuilderPass &builder,
                           path_cond_t cond) {
  path_cond_t imported_source = getImportedSource(cond);
  if (!imported_source)
    return nullptr;

  Function *origin_func = imported_source->getOwnerFunc();
  if (!origin_func || !builder.hasGraphFor(*origin_func))
    return nullptr;

  auto &origin_graph = builder.getGraph(*origin_func);
  auto *origin_region =
      translatePathCondToRegion(builder, origin_graph, imported_source,
                                getFunctionEntryBlockOrNull(origin_func));
  if (origin_region && origin_region->getConditionNode())
    return origin_region->getConditionNode();

  origin_region = origin_graph.findOrCreateSemanticRegion(
      imported_source, getFunctionEntryBlockOrNull(origin_func));
  return origin_region ? origin_region->getConditionNode() : nullptr;
}

static GuardedValueFlowNode *
resolveProducerValueNode(GuardedValueFlowGraph &graph, Value *value, Type *type,
                         BasicBlock *bb) {
  if (!value)
    return nullptr;
  if (auto *pseudo_arg = graph.findPseudoArgumentBySource(value))
    return pseudo_arg;
  if (auto *existing = graph.findInterfaceNode(value))
    return existing;
  return ensureValueNode(graph, value, type ? type : value->getType(), bb,
                         GuardedValueFlowNode::Kind::SimpleOperand, "producer");
}

static GuardedValueFlowNode *
resolveFunctionSummarySourceNode(GuardedValueFlowGraph &graph, Value *value) {
  if (!value)
    return nullptr;
  if (auto *pseudo_arg = graph.findPseudoArgumentBySource(value))
    return pseudo_arg;
  if (auto *interface_node = graph.findInterfaceNode(value))
    return interface_node;
  return graph.findNode(value);
}

static GuardedValueFlowNode *
findOrCreateFunctionSummaryArgumentNode(GuardedValueFlowGraph &graph,
                                        unsigned ap_depth, Value *source,
                                        Type *type, BasicBlock *entry_block) {
  auto *summary_node = graph.findFunctionSummaryArgumentNode(ap_depth, source);
  if (!summary_node) {
    summary_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::SimpleOperand, type, &graph, entry_block,
        nullptr, nullptr);
    summary_node->setDescription(
        (Twine("summary.arg.") + Twine(ap_depth)).str());
    graph.mapFunctionSummaryArgumentNode(ap_depth, source, summary_node);
  } else {
    summary_node->clearChildren();
  }
  return summary_node;
}

static GuardedValueFlowNode *
findOrCreateFunctionSummaryReturnNode(GuardedValueFlowGraph &graph,
                                      unsigned ap_depth, Type *type,
                                      BasicBlock *entry_block) {
  auto *summary_node = graph.findFunctionSummaryReturnNode(ap_depth);
  if (!summary_node) {
    summary_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::SimpleOperand, type, &graph, entry_block,
        nullptr, nullptr);
    summary_node->setDescription(
        (Twine("summary.ret.") + Twine(ap_depth)).str());
    graph.mapFunctionSummaryReturnNode(ap_depth, summary_node);
  }
  return summary_node;
}

static SmallVector<Function *, 4>
collectCallees(const GuardedValueFlowCallSite &site, CallBase &call,
               const IntraLotusAA &pta) {
  SmallVector<Function *, 4> result;
  for (Function *callee : site.getCallees())
    result.push_back(callee);

  auto resolved_it = pta.getResolvedCallTargets().find(&call);
  if (resolved_it != pta.getResolvedCallTargets().end()) {
    for (const auto &target : resolved_it->second) {
      if (llvm::find(result, target.first) == result.end())
        result.push_back(target.first);
    }
  }

  if (result.empty()) {
    if (Function *direct_callee = call.getCalledFunction())
      result.push_back(direct_callee);
  }

  return result;
}

static void materializeLoadParity(GuardedValueFlowGraph &graph,
                                  IntraLotusAA &pta,
                                  GuardedValueFlowGraphBuilderPass &builder) {
  StorePointerIndex stores_by_ptr;
  bool stores_indexed = false;
  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *load = dyn_cast<LoadInst>(&inst);
    if (!load)
      continue;

    auto *load_value_node = graph.findNode(load);
    auto *load_mem_node = graph.findLoadMemoryNode(load);
    if (!load_value_node || !load_mem_node)
      continue;

    load_value_node->clearChildren();
    load_value_node->addChild(load_mem_node);
    graph.mapLoadMemoryNode(load, load_mem_node);

    mem_value_t load_values;
    pta.collectGuardedValueFlowLoadValues(load, load_values);
    if (load_values.empty())
      collectExactPointerStoreFallbacks(pta, load, load_values, stores_by_ptr,
                                        stores_indexed);
    if (!load_values.empty()) {
      populateLoadMemoryNode(graph, load_mem_node, load_values,
                             load->getParent(), builder);
    }
    if (load_mem_node->children().empty())
      populateUnknownLoadMemoryNode(graph, load_mem_node, load);
  }
}

static void materializeStoreParity(GuardedValueFlowGraph &graph,
                                   IntraLotusAA &pta) {
  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store)
      continue;

    auto *mem_node = graph.findStoreMemoryNode(store->getValueOperand(), store);
    if (!mem_node)
      continue;

    mem_node->clearChildren();
    auto *value_node = resolveProducerValueNode(
        graph, store->getValueOperand(), store->getValueOperand()->getType(),
        store->getParent());
    (void)LotusAAWrapper::safeLink(graph, mem_node,
                                                     value_node);
  }
}

static void
materializeFunctionSummaryInterface(GuardedValueFlowGraph &graph,
                                    IntraLotusAA &pta,
                                    GuardedValueFlowGraphBuilderPass &builder) {
  BasicBlock *entry_block = getEntryBlockOrNull(graph);
  Type *summary_type = getStableSummaryNodeType(pta.getFunc()->getContext());
  graph.resetFunctionSummaryInterface();

  const auto &summary_inputs = pta.getSummaryInputs();
  // Summary arguments are value-only sentinels keyed by access-path depth.
  // Summary returns are value -> load-memory chains so they can still carry
  // imported producer facts and matching regions.
  for (unsigned bucket = 0; bucket < summary_inputs.size(); ++bucket) {
    const auto *summary_bucket = summary_inputs[bucket];
    if (!summary_bucket || summary_bucket->empty())
      continue;

    for (Value *source : *summary_bucket) {
      auto *summary_node = findOrCreateFunctionSummaryArgumentNode(
          graph, bucket, source, summary_type, entry_block);
      auto *source_node = resolveFunctionSummarySourceNode(graph, source);
      if (source_node)
        (void)LotusAAWrapper::safeLink(graph, source_node,
                                                         summary_node);
      graph.registerSummaryArgumentNode(bucket, summary_node);
    }
  }

  const auto &summary_outputs = pta.getSummaryOutputs();
  for (unsigned bucket = 0; bucket < summary_outputs.size(); ++bucket) {
    const mem_value_t *summary_bucket = summary_outputs[bucket];
    if (!summary_bucket || summary_bucket->empty())
      continue;

    auto *summary_node = findOrCreateFunctionSummaryReturnNode(
        graph, bucket, summary_type, entry_block);
    auto *load_mem = detachReusableLoadMemoryChild(
        summary_node, (Twine("summary.ret.mem.") + Twine(bucket)).str());
    if (!load_mem) {
      load_mem = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
          entry_block, nullptr, nullptr);
      load_mem->setDescription(
          (Twine("summary.ret.mem.") + Twine(bucket)).str());
    }

    (void)LotusAAWrapper::safeLink(graph, summary_node,
                                                     load_mem);
    populateLoadMemoryNode(graph, load_mem, *summary_bucket, entry_block,
                           builder, SummaryValueMode::OpaqueProducer);
    graph.registerSummaryReturnNode(bucket, summary_node);
  }
}

static void
materializeFunctionOutputs(GuardedValueFlowGraph &graph, IntraLotusAA &pta,
                           GuardedValueFlowGraphBuilderPass &builder) {
  const auto &outputs = pta.getOutputs();
  // Output index 0 is the direct SSA return channel and stays on CommonReturn.
  // Remaining outputs are modeled as pseudo returns with per-return memory
  // nodes so indirect side effects remain explicit.
  for (size_t idx = 1; idx < outputs.size(); ++idx) {
    const auto *output = outputs[idx];
    auto *pseudo_return = graph.getPseudoReturn(static_cast<unsigned>(idx - 1));
    if (!pseudo_return) {
      Value *pseudo_value = graph.createSyntheticInterfaceValue(
          output->getType(),
          (Twine("pseudo.return.value.") + Twine(idx - 1)).str());
      pseudo_return = graph.createNode<GuardedValueFlowReturnNode>(
          GuardedValueFlowNode::Kind::PseudoReturn, output->getType(), &graph,
          pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock(),
          pseudo_value);
      pseudo_return->setDescription(
          (Twine("pseudo.return.") + Twine(idx)).str());
      pseudo_return->setIndex(static_cast<unsigned>(idx - 1));
      graph.mapInterfaceNode(pseudo_value, pseudo_return);
      graph.registerPseudoReturn(pseudo_return);
    } else if (pseudo_return->getLLVMValue()) {
      graph.mapInterfaceNode(pseudo_return->getLLVMValue(), pseudo_return);
    }
    pseudo_return->clearChildren();
    setNodeAccessPathFromOutputIndex(pseudo_return, pta,
                                     static_cast<unsigned>(idx));

    const auto &ret_val_map = output->getVal();
    if (ret_val_map.empty())
      continue;

    for (const auto &ret_vals : ret_val_map) {
      auto *ret_inst = ret_vals.first;
      auto *site = findOrCreateReturnSite(graph, ret_inst);
      auto *load_mem = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::LoadMemory, output->getType(), &graph,
          ret_inst ? ret_inst->getParent()
                   : pseudo_return->getParentBasicBlock(),
          nullptr, ret_inst);
      load_mem->setDescription(
          (Twine("return.mem.") + Twine(idx) + "." +
           Twine(ret_inst ? ret_inst->getParent()->getName()
                          : StringRef("entry")))
              .str());
      auto *linked_ret = LotusAAWrapper::safeLink(
          graph, pseudo_return, load_mem);
      if (linked_ret)
        pseudo_return->addReturnValueSitePair(linked_ret, site);
      mem_value_t values = ret_vals.second;
      if (values.empty())
        values = makeFallbackValues(LocValue::FREE_VARIABLE, ret_inst);
      populateLoadMemoryNode(graph, load_mem, values,
                             ret_inst ? ret_inst->getParent()
                                      : pseudo_return->getParentBasicBlock(),
                             builder);
    }
  }
}

static bool materializeCallsiteSummaryNodes(
    GuardedValueFlowGraph &graph, IntraLotusAA &pta, LotusAA &lotus,
    GuardedValueFlowGraphBuilderPass &builder, std::string &failure) {
  BasicBlock *entry_block = getEntryBlockOrNull(graph);
  Function *caller = graph.getBaseFunction();

  for (const auto &site_ptr : graph.sites()) {
    auto *site = dynamic_cast<GuardedValueFlowCallSite *>(site_ptr.get());
    if (!site)
      continue;

    auto *call = dyn_cast_or_null<CallBase>(site->getInstruction());
    if (!call)
      continue;

    SmallVector<Function *, 4> callees = collectCallees(*site, *call, pta);
    if (callees.empty())
      continue;

    std::vector<mem_value_t> summary_values;
    pta.collectGuardedValueFlowCallsiteSummaryInputs(call, summary_values);

    int start_ap_depth = 0;
    if (Function *direct_callee = call->getCalledFunction()) {
      if (auto *direct_callee_graph =
              dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(direct_callee))) {
        if (!direct_callee_graph->isConsideredAsLibrary())
          start_ap_depth = direct_callee_graph->getInlineApDepth();
      }
    }

      for (unsigned bucket = 1; bucket < summary_values.size(); ++bucket) {
        Type *summary_type = getStableSummaryNodeType(call->getContext());
        auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
            GuardedValueFlowNode::Kind::CallSiteArgumentSummary, summary_type,
            &graph, call->getParent(), call, nullptr, bucket);
      summary_node->setDescription(
          (Twine("call.input.summary.") + Twine(bucket)).str());

      if (static_cast<int>(bucket) > start_ap_depth) {
        SummarySentinelProvenance summary_provenance{call, nullptr, bucket,
                                                     SummaryDirection::Input};
        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
            entry_block, nullptr, call);
        load_mem->setDescription(
            (Twine("call.input.summary.mem.") + Twine(bucket)).str());
        (void)LotusAAWrapper::safeLink(graph, summary_node,
                                                         load_mem);
        populateLoadMemoryNode(graph, load_mem, summary_values[bucket],
                               entry_block, builder,
                               SummaryValueMode::CallsiteProducer,
                               &summary_provenance);
      }

      for (Function *callee : callees) {
        if (caller && lotus.isBackEdge(caller, callee))
          continue;
        site->setInputSummaryNode(callee, bucket, summary_node);
      }

    }

    for (Function *callee : callees) {
      if (caller && lotus.isBackEdge(caller, callee))
        continue;

      auto *callee_graph = dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(callee));
      if (!callee_graph)
        continue;

      const auto &summary_outputs = callee_graph->getSummaryOutputs();
      for (unsigned bucket = 0; bucket < summary_outputs.size(); ++bucket) {
        const mem_value_t *summary_bucket = summary_outputs[bucket];
        if (!summary_bucket || summary_bucket->empty())
          continue;

        Type *summary_type = getStableSummaryNodeType(call->getContext());
        auto *summary_node = dyn_cast_or_null<GuardedValueFlowCallSummaryNode>(
            site->getOutputSummaryNode(callee, bucket));
        if (!summary_node) {
          summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
              GuardedValueFlowNode::Kind::CallSiteReturnSummary, summary_type,
              &graph, call->getParent(), call, callee, bucket);
          summary_node->setDescription(
              (Twine("call.output.summary.") + Twine(bucket)).str());
          site->setOutputSummaryNode(callee, bucket, summary_node);
        } else {
          summary_node->clearChildren();
        }

        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
            call->getParent(), nullptr, call);
        load_mem->setDescription(
            (Twine("call.output.summary.mem.") + Twine(bucket)).str());
        (void)LotusAAWrapper::safeLink(graph, summary_node,
                                                         load_mem);

        SummarySentinelProvenance summary_provenance{call, callee, bucket,
                                                     SummaryDirection::Output};
        mem_value_t imported_values =
            importSummaryValues(pta, call, callee, *summary_bucket);
        populateLoadMemoryNode(graph, load_mem, imported_values,
                               call->getParent(), builder,
                               SummaryValueMode::CallsiteProducer,
                               &summary_provenance);
      }
    }
  }

  return true;
}

static bool materializeCallsiteInterfaces(
    GuardedValueFlowGraph &graph, IntraLotusAA &pta, LotusAA &lotus,
    GuardedValueFlowGraphBuilderPass &builder, std::string &failure) {
  const auto &call_arg_bindings = pta.getCallArgBindings();
  const auto &call_ret_bindings = pta.getCallReturnBindings();
  BasicBlock *entry_block = getEntryBlockOrNull(graph);
  Function *caller = graph.getBaseFunction();

  for (const auto &site_ptr : graph.sites()) {
    auto *site = dynamic_cast<GuardedValueFlowCallSite *>(site_ptr.get());
    if (!site)
      continue;

    auto *call = dyn_cast_or_null<CallBase>(site->getInstruction());
    if (!call)
      continue;

    SmallVector<Function *, 4> callees = collectCallees(*site, *call, pta);
    for (Function *callee : callees) {
      if (caller && lotus.isBackEdge(caller, callee))
        continue;

      site->addCallee(callee);

      auto *callee_graph =
          dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(callee));
      if (!callee_graph)
        continue;

      auto call_bind_it = call_arg_bindings.find(call);
      const std::map<Value *, mem_value_t, llvm_cmp> *callee_input_bindings =
          nullptr;
      if (call_bind_it != call_arg_bindings.end()) {
        auto callee_it = call_bind_it->second.find(callee);
        if (callee_it != call_bind_it->second.end())
          callee_input_bindings = &callee_it->second;
      }

      if (!callee_graph->getInputs().empty() && !callee_input_bindings) {
        return setAdapterFailure(
            failure, Twine("Missing pseudo-input bindings for caller ") +
                         caller->getName() + " -> " + callee->getName());
      }

      using PseudoInputBinding = std::pair<Value *, const mem_value_t *>;
      std::vector<PseudoInputBinding> ordered_pseudo_inputs(
          callee_graph->getInputs().size(), {nullptr, nullptr});
      for (const auto &input_item : callee_graph->getInputs()) {
        Value *formal_value = input_item.first;
        int raw_pseudo_input_index =
            callee_graph->getPseudoInputIndex(formal_value);
        if (raw_pseudo_input_index < 0 ||
            static_cast<size_t>(raw_pseudo_input_index) >=
                ordered_pseudo_inputs.size() ||
            ordered_pseudo_inputs[raw_pseudo_input_index].first) {
          return setAdapterFailure(
              failure, Twine("Invalid pseudo-input index mapping for caller ") +
                           caller->getName() + " -> " + callee->getName());
        }
        const mem_value_t *binding_values = nullptr;
        if (callee_input_bindings) {
          auto binding_it = callee_input_bindings->find(formal_value);
          if (binding_it != callee_input_bindings->end())
            binding_values = &binding_it->second;
        }
        ordered_pseudo_inputs[raw_pseudo_input_index] = {formal_value,
                                                         binding_values};
      }
      for (const auto &ordered_input : ordered_pseudo_inputs) {
        if (!ordered_input.first)
          return setAdapterFailure(
              failure, Twine("Incomplete pseudo-input mapping for caller ") +
                           caller->getName() + " -> " + callee->getName());
      }

      for (size_t raw_index = 0; raw_index < ordered_pseudo_inputs.size();
           ++raw_index) {
        Value *formal_value = ordered_pseudo_inputs[raw_index].first;
        if (!formal_value)
          continue;
        const mem_value_t *binding_values =
            ordered_pseudo_inputs[raw_index].second;
        if (!binding_values) {
          return setAdapterFailure(
              failure, Twine("Missing pseudo-input binding for caller ") +
                           caller->getName() + " -> " + callee->getName() +
                           ", input index " + Twine(raw_index));
        }

        auto *pseudo_input = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
            site->getPseudoInput(callee, static_cast<unsigned>(raw_index)));
        if (!pseudo_input) {
          Value *pseudo_value = graph.createSyntheticInterfaceValue(
              formal_value->getType(),
              (Twine("pseudo.input.") + callee->getName() + "." +
               Twine(raw_index))
                  .str());
          pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
              GuardedValueFlowNode::Kind::CallSitePseudoInput,
              formal_value->getType(), &graph, call->getParent(), pseudo_value,
              call, callee);
          pseudo_input->setDescription(
              (Twine("call.input.") + Twine(raw_index)).str());
          graph.mapInterfaceNode(pseudo_value, pseudo_input);
          site->addPseudoInput(callee, pseudo_input);
        } else if (pseudo_input->getLLVMValue()) {
          graph.mapInterfaceNode(pseudo_input->getLLVMValue(), pseudo_input);
        }
        pseudo_input->clearChildren();
        setNodeAccessPathFromValue(pseudo_input, *callee_graph, formal_value);

        // Pseudo inputs behave like a callsite-local value whose only child is
        // a load-memory node populated from the caller-to-callee binding.
        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, formal_value->getType(),
            &graph, call->getParent(), nullptr, call);
        load_mem->setDescription(
            (Twine("call.input.mem.") + Twine(raw_index)).str());
        (void)LotusAAWrapper::safeLink(graph, pseudo_input,
                                                         load_mem);
        mem_value_t values = *binding_values;
        populateLoadMemoryNode(graph, load_mem, values, call->getParent(),
                               builder);
      }

      auto ret_bind_it = call_ret_bindings.find(call);
      const std::vector<Value *> *pseudo_outputs = nullptr;
      if (ret_bind_it != call_ret_bindings.end()) {
        auto callee_it = ret_bind_it->second.find(callee);
        if (callee_it != ret_bind_it->second.end())
          pseudo_outputs = &callee_it->second;
      }

      const auto &outputs = callee_graph->getOutputs();
      if (outputs.size() > 1) {
        bool has_complete_pseudo_output_mapping =
            pseudo_outputs && pseudo_outputs->size() == outputs.size();
        if (has_complete_pseudo_output_mapping) {
          for (size_t idx = 1; idx < outputs.size(); ++idx) {
            if (!(*pseudo_outputs)[idx]) {
              has_complete_pseudo_output_mapping = false;
              break;
            }
          }
        }
        if (!has_complete_pseudo_output_mapping) {
          return setAdapterFailure(
              failure, Twine("Incomplete pseudo-output mapping for caller ") +
                           caller->getName() + " -> " + callee->getName());
        }
      }
      for (size_t idx = 1; idx < outputs.size(); ++idx) {
        auto *pseudo_output = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
            site->getPseudoOutput(callee, static_cast<unsigned>(idx - 1)));
        Value *pseudo_value =
            pseudo_output ? pseudo_output->getLLVMValue() : nullptr;
        if (!pseudo_value && pseudo_outputs && idx < pseudo_outputs->size())
          pseudo_value = (*pseudo_outputs)[idx];
        if (!pseudo_value) {
          return setAdapterFailure(
              failure, Twine("Missing pseudo-output binding for caller ") +
                           caller->getName() + " -> " + callee->getName() +
                           ", output index " + Twine(idx - 1));
        }
        if (!pseudo_output) {
          pseudo_output = graph.createNode<GuardedValueFlowCallOutputNode>(
              GuardedValueFlowNode::Kind::CallSitePseudoOutput,
              outputs[idx]->getType(), &graph, entry_block, pseudo_value, call,
              callee);
          pseudo_output->setDescription(
              (Twine("call.output.") + Twine(idx - 1)).str());
          site->addPseudoOutput(callee, pseudo_output);
        }
        graph.mapInterfaceNode(pseudo_value, pseudo_output);
        setNodeAccessPathFromOutputIndex(pseudo_output, *callee_graph,
                                         static_cast<unsigned>(idx));

        // Pseudo outputs mirror the function-output shape: the interface node
        // is anchored at entry, while the backing store-memory node is anchored
        // at the callsite that materializes the effect.
        auto *store_mem =
            ensureStoreMemoryNode(graph, pseudo_value, call,
                                  outputs[idx]->getType(), call->getParent());
        (void)LotusAAWrapper::safeLink(graph, store_mem,
                                                         pseudo_output);
      }
    }
  }

  return true;
}

static void
materializeCallTargetConditions(GuardedValueFlowGraph &graph, IntraLotusAA &pta,
                                GuardedValueFlowGraphBuilderPass &builder) {
  for (const auto &cg_item : pta.getResolvedCallTargets()) {
    auto *call = dyn_cast<CallBase>(cg_item.first);
    auto *site = call ? graph.findCallSite(call) : nullptr;
    if (!site)
      continue;

    for (const auto &target : cg_item.second) {
      site->addCallee(target.first);
      if (target.second)
        site->setCalleeCondition(
            target.first, ConditionRef::fromPathCond(target.second),
            translatePathCondToRegion(builder, graph, target.second,
                                      call->getParent()));
    }
  }
}

static void materializeCallsiteBackEdges(GuardedValueFlowGraph &graph,
                                         LotusAA &lotus) {
  Function *caller = graph.getBaseFunction();
  if (!caller)
    return;

  for (const auto &site_ptr : graph.sites()) {
    auto *site = dynamic_cast<GuardedValueFlowCallSite *>(site_ptr.get());
    if (!site)
      continue;

    for (Function *callee : site->getCallees()) {
      if (lotus.isBackEdge(caller, callee))
        site->setBackEdge(callee);
    }
  }
}

} // namespace

char LotusAAWrapper::ID = 0;
static RegisterPass<LotusAAWrapper>
    Y("gvfg-lotus-aa-wrapper", "LotusAA to GuardedValueFlowGraph wrapper", false,
      true);

LotusAAWrapper::LotusAAWrapper()
    : ModulePass(ID) {}

GuardedValueFlowNode *LotusAAWrapper::safeLink(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode *parent,
    GuardedValueFlowNode *child, float confidence, ConditionRef condition) {
  if (!parent || !child)
    return nullptr;

  Type *src_ty = child->getType();
  Type *dst_ty = parent->getType();

  if (src_ty && dst_ty && src_ty == dst_ty) {
    parent->addChild(child, confidence, condition);
    return child;
  }

  // Preserve the intended dependency even when the adapter has to bridge a
  // type mismatch introduced by imported memory or interface facts.
  auto opcode_kind = chooseCastOpcode(src_ty, dst_ty);
  if (opcode_kind == GuardedValueFlowOpcodeNode::OpcodeKind::Invalid) {
    BasicBlock *coercion_block =
        parent->getParentBasicBlock()
            ? parent->getParentBasicBlock()
            : (child->getParentBasicBlock() ? child->getParentBasicBlock()
                                            : getEntryBlockOrNull(graph));
    auto *coercion = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::Unknown, dst_ty, &graph, coercion_block,
        nullptr, parent->getDebugInstruction());
    coercion->setDescription("adapter.coercion");
    coercion->addChild(child);
    parent->addChild(coercion, confidence, condition);
    recordAdapterWarning(
        graph,
        Twine("Preserved dependency through an opaque adapter coercion: ") +
            parent->getDescription() + " <- " + child->getDescription(),
        parent->getDebugInstruction(), coercion_block);
    return coercion;
  }

  BasicBlock *cast_block =
      parent->getParentBasicBlock()
          ? parent->getParentBasicBlock()
          : (child->getParentBasicBlock() ? child->getParentBasicBlock()
                                          : getEntryBlockOrNull(graph));
  auto *cast_node = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::CastOpcode, dst_ty, &graph, cast_block,
      opcode_kind);
  cast_node->setDescription("adapter.cast");
  cast_node->addChild(child);

  const DataLayout &DL = graph.getBaseFunction()->getParent()->getDataLayout();
  if (src_ty->isSized() && dst_ty->isSized())
    cast_node->setCastWidths(DL.getTypeSizeInBits(src_ty),
                             DL.getTypeSizeInBits(dst_ty));

  parent->addChild(cast_node, confidence, condition);
  return cast_node;
}

void LotusAAWrapper::getAnalysisUsage(
    AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LotusAA>();
  AU.addRequired<GuardedValueFlowGraphBuilderPass>();
}

bool LotusAAWrapper::runOnModule(Module &M) {
  auto &lotus = getAnalysis<LotusAA>();
  auto &builder = getAnalysis<GuardedValueFlowGraphBuilderPass>();

  for (Function &F : M) {
    if (F.isDeclaration() || !builder.hasGraphFor(F))
      continue;
    if (IntraLotusAA *pta = lotus.getPtGraph(&F))
      (void)adaptFunction(builder.getGraph(F), *pta, lotus, builder);
  }

  return false;
}

bool LotusAAWrapper::adaptFunction(
    GuardedValueFlowGraph &graph, IntraLotusAA &pta, LotusAA &lotus,
    GuardedValueFlowGraphBuilderPass &builder) {
  auto fail = [&](const std::string &reason) {
    errs() << "[gvfg-adapter] Partially adapted function "
           << graph.getBaseFunction()->getName() << ": " << reason << "\n";
    GuardedValueFlowGraph::Diagnostic diagnostic;
    diagnostic.origin = GuardedValueFlowGraph::Diagnostic::Origin::Adapter;
    diagnostic.severity = GuardedValueFlowGraph::Diagnostic::Severity::Warning;
    diagnostic.message = reason;
    graph.addDiagnostic(std::move(diagnostic));
    return false;
  };

  unsigned pseudo_arg_index = 0;
  for (const auto &input_item : pta.getInputs()) {
    Value *value = input_item.first;
    auto *node = graph.getPseudoArgument(pseudo_arg_index);
    if (!node) {
      node = createInterfaceArgumentNode(
          graph, value, value->getType(),
          pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock(),
          "pseudo.input");
      node->setIndex(pseudo_arg_index);
      graph.registerPseudoArgument(node);
    }
    graph.mapPseudoArgumentSource(value, node);
    setNodeAccessPathFromValue(node, pta, value);
    ++pseudo_arg_index;
  }

  materializeStoreParity(graph, pta);
  materializeLoadParity(graph, pta, builder);
  materializeFunctionSummaryInterface(graph, pta, builder);
  materializeFunctionOutputs(graph, pta, builder);
  materializeCallTargetConditions(graph, pta, builder);
  std::string failure;
  if (!materializeCallsiteSummaryNodes(graph, pta, lotus, builder, failure))
    return fail(failure);
  if (!materializeCallsiteInterfaces(graph, pta, lotus, builder, failure))
    return fail(failure);
  materializeCallsiteBackEdges(graph, lotus);
  return true;
}

ModulePass *lotus::gvfg::createLotusAAWrapper() {
  return new LotusAAWrapper();
}
