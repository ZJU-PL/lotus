/// @file GuardedValueFlowSolver.cpp
/// @brief SMT-based constraint solver over the GuardedValueFlowGraph
///
/// Translates value-flow dependencies and region-based path conditions into
/// SMT bit-vector formulas.  Encodes binary/compare/cast/GEP/concat/extract/
/// insert/select opcodes into the solver's SMTFactory API.  Supports:
///   - getCtrlDeps — per-node / per-block control-dependence constraints
///   - getDataDeps — recursive encoding of child opcode/operand chains
///   - getPhiGated — incoming-edge-local guards from PHI nodes
///   - getDeps / getDepsPair — combined Ctrl+Data for one assignment edge
///   - getOrInsertExpr — SMT variable creation for each GVFG node
///
/// The dominator-aware variant `DTGuardedValueFlowSolver` suppresses
/// control-dependence constraints already implied by the previously visited
/// basic block, reducing redundant formula growth.

#include "IR/GVFG/GuardedValueFlowSolver.h"

#include <climits>

#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace lotus::gvfg;

#define DEBUG_TYPE "gvfg-solver"

static cl::opt<unsigned> GVFGDDMaxDepth(
    "gvfg-solver-dd-max-depth", cl::init(UINT_MAX),
    cl::desc(
        "The max searching depth for guarded value-flow data dependencies."));

static cl::opt<unsigned>
    GVFGCDMaxDepth("gvfg-solver-cd-max-depth", cl::init(UINT_MAX),
                   cl::desc("The max searching depth for guarded value-flow "
                            "control dependencies."));

static cl::opt<bool> GVFGUseRegionAsCtrlDep(
    "gvfg-solver-use-region", cl::init(false),
    cl::desc("Use region nodes directly when computing control dependencies."));

static cl::opt<bool>
    GVFGIgnoreBitVecOverflow("gvfg-solver-ignore-bv-overflow", cl::init(true),
                             cl::desc("Ignore bitvector overflow heuristically "
                                      "for mul/div/shift operations."));

namespace {

static bool isTerminalNode(const GuardedValueFlowNode *node) {
  return node && node->children().empty();
}

static bool isResultLikeNode(const GuardedValueFlowNode *node) {
  if (!node)
    return false;
  return isa<GuardedValueFlowArgumentNode>(node) ||
         isa<GuardedValueFlowPhiNode>(node) ||
         isa<GuardedValueFlowReturnNode>(node) ||
         isa<GuardedValueFlowCallOutputNode>(node) ||
         isa<GuardedValueFlowCallSummaryNode>(node) ||
         node->getKind() == GuardedValueFlowNode::Kind::SimpleOperand ||
         node->getKind() == GuardedValueFlowNode::Kind::UndefValue ||
         node->getKind() == GuardedValueFlowNode::Kind::LoadMemory ||
         node->getKind() == GuardedValueFlowNode::Kind::StoreMemory ||
         node->getKind() == GuardedValueFlowNode::Kind::Region ||
         node->getKind() == GuardedValueFlowNode::Kind::InterfaceCondition;
}

static unsigned getVectorElementCount(Type *type) {
  if (auto *vector_type = dyn_cast_or_null<VectorType>(type))
    return vector_type->getElementCount().getKnownMinValue();
  return 1;
}

static std::string buildNodeSymbol(const GuardedValueFlowNode *node) {
  std::string description = node->getDescription();
  for (char &ch : description) {
    if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
      ch = '_';
  }
  return (Twine("gvfg_") + Twine(node->getNodeId()) + "_" + description).str();
}

static bool shouldExcludeSummaryBackedChild(const GuardedValueFlowNode *current,
                                            const GuardedValueFlowNode *child) {
  if (!current || !child ||
      current->getKind() != GuardedValueFlowNode::Kind::LoadMemory) {
    return false;
  }

  // Use Floyd's cycle detection because single-child value-flow loops
  // (e.g., Load -> Store -> Phi -> Load) can form cycles.
  const GuardedValueFlowNode *slow = child;
  const GuardedValueFlowNode *fast = child;
  while (fast && fast->children().size() == 1) {
    fast = fast->children().front().target;
    if (!fast || fast->children().size() != 1)
      break;
    fast = fast->children().front().target;
    slow = slow->children().front().target;
    if (slow == fast)
      return false;
  }

  return fast &&
         fast->getKind() == GuardedValueFlowNode::Kind::CallSiteReturnSummary;
}

static bool isBinaryOpcodeKind(GuardedValueFlowOpcodeNode::OpcodeKind kind) {
  switch (kind) {
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
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
    return true;
  default:
    return false;
  }
}

static bool isNonNullTerminalValue(const GuardedValueFlowNode *node) {
  if (!node)
    return false;
  Value *value = node->getLLVMValue();
  if (!value)
    return false;
  return isa<AllocaInst>(value) ||
         isa<GlobalValue>(value->stripInBoundsConstantOffsets());
}

static SMTExpr alignBitVecWidth(SMTExpr expr, uint64_t target_width) {
  if (!expr.isBitVector() || target_width == 0)
    return expr;
  uint64_t src_width = expr.getBitVecSize();
  if (src_width == target_width)
    return expr;
  if (src_width < target_width)
    return expr.basic_zext(target_width - src_width);
  return expr.basic_extract(target_width - 1, 0);
}

static SMTExpr toBoolExpr(SMTFactory &factory, SMTExpr expr) {
  if (expr.isBool())
    return expr;
  if (!expr.isBitVector())
    return factory.createBoolVal(true);
  uint64_t width = expr.getBitVecSize();
  if (width == 1)
    return expr.bv12bool();
  return expr != factory.createBitVecVal(0, width);
}

static SMTExpr makeEqualExpr(SMTFactory &factory, SMTExpr lhs, SMTExpr rhs) {
  if (lhs.isBitVector() && rhs.isBitVector()) {
    uint64_t lw = lhs.getBitVecSize();
    uint64_t rw = rhs.getBitVecSize();
    if (lw != rw) {
      uint64_t target = std::max(lw, rw);
      lhs = alignBitVecWidth(lhs, target);
      rhs = alignBitVecWidth(rhs, target);
    }
    return lhs == rhs;
  }
  if (lhs.isBool() && rhs.isBool())
    return lhs == rhs;
  return toBoolExpr(factory, lhs) == toBoolExpr(factory, rhs);
}

static SMTExpr getMatchingRegionExpr(GuardedValueFlowSolver &solver,
                                     const GuardedValueFlowNode *load_node,
                                     const GuardedValueFlowNode *producer) {
  auto *region = load_node ? load_node->getMatchingRegion(producer) : nullptr;
  if (!region)
    return solver.getSMTFactory().createBoolVal(true);
  return toBoolExpr(solver.getSMTFactory(), solver.getOrInsertExpr(region));
}

} // namespace

GuardedValueFlowSolver::GuardedValueFlowSolver(SMTFactory &factory,
                                               const DataLayout &dl)
    : SMTSolver(factory.createSimpleSMTSolver()), DL(dl) {}

GuardedValueFlowSolver::~GuardedValueFlowSolver() {}

SMTExprVec GuardedValueFlowSolver::getCtrlDeps(const GuardedValueFlowNode *node,
                                               const QueryContext *context) {
  assert(node && "Expected a modeled node.");
  BasicBlock *block = node->getParentBasicBlock();
  assert(block && "Expected node to belong to a block.");
  auto *graph = node->getGraph();
  assert(graph && "Expected node to belong to a graph.");
  return getCtrlDeps(block, graph, context);
}

SMTExprVec
GuardedValueFlowSolver::getCtrlDeps(BasicBlock *block,
                                    const GuardedValueFlowGraph *graph,
                                    const QueryContext *context) {
  auto pair = getCtrlDepsPair(block, graph, context);
  return SMTExprVec::merge(pair.first.copy(), pair.second);
}

std::pair<SMTExprVec, SMTExprVec>
GuardedValueFlowSolver::getCtrlDepsPair(BasicBlock *block,
                                        const GuardedValueFlowGraph *graph,
                                        const QueryContext *context) {
  return computeCtrlDepsPair(block, graph, context);
}

SMTExprVec
GuardedValueFlowSolver::getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                                    GuardedValueFlowPhiNode::Incoming incoming,
                                    const QueryContext *context) {
  auto pair = computePhiGatedPair(phi_node, incoming, context);
  return SMTExprVec::merge(pair.first.copy(), pair.second);
}

SMTExprVec GuardedValueFlowSolver::getPhiGated(
    const GuardedValueFlowPhiNode *phi_node, const GuardedValueFlowNode *value,
    BasicBlock *block, const QueryContext *context) {
  for (const auto &incoming : phi_node->incoming()) {
    if (incoming.incoming_block == block && incoming.value_node == value)
      return getPhiGated(phi_node, incoming, context);
  }
  llvm_unreachable("Requested PHI gating for a non-incoming value/block pair");
}

SMTExprVec
GuardedValueFlowSolver::getPhiGated(const GuardedValueFlowPhiNode *phi_node,
                                    const GuardedValueFlowNode *value,
                                    const QueryContext *context) {
  SMTExprVec gated = Factory->createEmptySMTExprVec();
  SMTExprVec ors = Factory->createEmptySMTExprVec();
  for (const auto &incoming : phi_node->incoming()) {
    if (incoming.value_node == value)
      ors.push_back(getPhiGated(phi_node, incoming, context).toAndExpr());
  }
  gated.push_back(ors.toOrExpr());
  return gated;
}

SMTExprVec GuardedValueFlowSolver::getDataDeps(const GuardedValueFlowNode *node,
                                               const QueryContext *context) {
  return computeDataDeps(node, context);
}

SMTExpr
GuardedValueFlowSolver::getOrInsertExpr(const GuardedValueFlowNode *node) {
  assert(node);

  if (isa<GuardedValueFlowArgumentNode>(node) &&
      !FunctionArgumentCache.contains(node)) {
    FunctionArgumentCache.add(node);
  } else if (auto *call_output =
                 dyn_cast<GuardedValueFlowCallOutputNode>(node)) {
    if (trackCallOutput(call_output) &&
        !CallSiteOutputCache.contains(call_output))
      CallSiteOutputCache.add(call_output);
  }

  auto existing = NodeExprMap.find(node);
  if (existing != NodeExprMap.end())
    return existing->second;

  const std::string symbol = getEncodingSymbol(node);
  NodeSymbolNameMap[symbol] = node;

  SMTExpr result = Factory->createEmptySMTExpr();
  if (isTerminalNode(node) && !isa<GuardedValueFlowOpcodeNode>(node)) {
    if (Value *value = node->getLLVMValue()) {
      if (auto *constant_int = dyn_cast<ConstantInt>(value)) {
        result = encodeScalarConstant(constant_int,
                                      getEncodingTypeSize(node->getType()));
        NodeExprMap.insert({node, result});
        return result;
      }
      if (isa<ConstantPointerNull>(value) ||
          isa<ConstantAggregateZero>(value)) {
        result = encodeScalarConstant(cast<Constant>(value),
                                      getEncodingTypeSize(node->getType()));
        NodeExprMap.insert({node, result});
        return result;
      }
      if (auto *constant_fp = dyn_cast<ConstantFP>(value)) {
        result = encodeScalarConstant(constant_fp,
                                      getEncodingTypeSize(node->getType()));
        NodeExprMap.insert({node, result});
        return result;
      }
      if (auto *cds = dyn_cast<ConstantDataSequential>(value)) {
        Type *element_type = cds->getElementType();
        unsigned element_num = cds->getNumElements();
        uint64_t total_size = getEncodingTypeSize(node->getType());
        uint64_t elem_size = element_num ? (total_size / element_num) : 0;
        if (element_num == 0 || elem_size == 0 ||
            (!element_type->isFloatTy() && !element_type->isDoubleTy() &&
             !element_type->isIntegerTy())) {
          result = Factory->createBitVecConst(symbol, total_size);
        } else {
          for (unsigned idx = 0; idx < element_num; ++idx) {
            uint64_t elem = 0;
            if (element_type->isFloatTy())
              elem = static_cast<uint64_t>(cds->getElementAsFloat(idx));
            else if (element_type->isDoubleTy())
              elem = static_cast<uint64_t>(cds->getElementAsDouble(idx));
            else
              elem = cds->getElementAsInteger(idx);

            SMTExpr next = Factory->createBitVecVal(elem, elem_size);
            result = idx == 0 ? next : result.basic_concat(next);
          }
        }
        NodeExprMap.insert({node, result});
        return result;
      }
    }
  }

  result =
      Factory->createBitVecConst(symbol, getEncodingTypeSize(node->getType()));
  NodeExprMap.insert({node, result});
  return result;
}

uint64_t GuardedValueFlowSolver::getEncodingTypeSize(Type *type) const {
  uint64_t width = (type && type->isSized()) ? DL.getTypeSizeInBits(type) : 0;
  return width ? width : DL.getPointerSizeInBits();
}

std::string GuardedValueFlowSolver::getEncodingSymbol(
    const GuardedValueFlowNode *node) const {
  return buildNodeSymbol(node);
}

SMTExpr GuardedValueFlowSolver::encodeScalarConstant(
    const Constant *constant, uint64_t width) {
  if (auto *integer = dyn_cast<ConstantInt>(constant))
    return Factory->createBitVecVal(
        llvm::toString(integer->getValue(), 10, false), width);
  if (auto *floating = dyn_cast<ConstantFP>(constant))
    return Factory->createBitVecVal(
        llvm::toString(floating->getValueAPF().bitcastToAPInt(), 10, false),
        width);
  assert((isa<ConstantPointerNull>(constant) ||
          isa<ConstantAggregateZero>(constant)) &&
         "Unexpected scalar constant kind");
  return Factory->createBitVecVal(0, width);
}

std::pair<uint64_t, uint64_t>
GuardedValueFlowSolver::getEncodingCastWidths(
    const GuardedValueFlowOpcodeNode *node) const {
  return {node->getCastSrcBits(), node->getCastDstBits()};
}

bool GuardedValueFlowSolver::trackCallOutput(
    const GuardedValueFlowCallOutputNode *) const {
  return true;
}

bool GuardedValueFlowSolver::isNonNullTerminal(
    const GuardedValueFlowNode *node) const {
  return isNonNullTerminalValue(node);
}

SMTExpr GuardedValueFlowSolver::encodeOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  auto existing = OpcodeConstraintsCacheMap.find(node);
  if (existing != OpcodeConstraintsCacheMap.end()) {
    for (const auto &edge : node->children()) {
      if (isa<GuardedValueFlowArgumentNode>(edge.target) &&
          !FunctionArgumentCache.contains(edge.target)) {
        FunctionArgumentCache.add(edge.target);
      } else if (auto *call_output =
                     dyn_cast<GuardedValueFlowCallOutputNode>(edge.target)) {
        if (trackCallOutput(call_output) &&
            !CallSiteOutputCache.contains(call_output))
          CallSiteOutputCache.add(call_output);
      }
    }
    return existing->second;
  }

  SMTExpr result = Factory->createEmptySMTExpr();
  switch (node->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::Trunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ZExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::BitCast:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast:
    result = encodeCastOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Select:
    result = encodeSelectOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::ExtractElement:
    result = encodeExtractElementOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::InsertElement:
    result = encodeInsertElementOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::ICmp:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FCmp:
    result = encodeCompareOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr:
    result = encodeGEPOpcodeNode(node);
    break;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Concat:
    result = encodeConcatOpcodeNode(node);
    break;
  default:
    assert(isBinaryOpcodeKind(node->getOpcodeKind()) && "Unsupported opcode");
    result = encodeBinaryOpcodeNode(node);
    break;
  }

  OpcodeConstraintsCacheMap.insert({node, result});
  return result;
}

SMTExpr GuardedValueFlowSolver::encodeBinaryOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  assert(node->getNumOperands() >= 1 && "binary opcode has no lhs operand");
  SMTExpr lhs = getOrInsertExpr(node->getOperand(0));
  SMTExpr rhs = Factory->createEmptySMTExpr();
  SMTExpr self = getOrInsertExpr(node);
  unsigned elem_num = getVectorElementCount(node->getType());

  if (node->getNumOperands() == 2) {
    rhs = getOrInsertExpr(node->getOperand(1));
  } else {
    assert(node->hasIntConstant() &&
           "single-operand binary opcode has no implicit constant");
    rhs = Factory->createBitVecVal(node->getIntConstant(), lhs.getBitVecSize());
  }

  if (lhs.isBitVector() && rhs.isBitVector() &&
      lhs.getBitVecSize() != rhs.getBitVecSize()) {
    uint64_t w = std::max(lhs.getBitVecSize(), rhs.getBitVecSize());
    lhs = alignBitVecWidth(lhs, w);
    rhs = alignBitVecWidth(rhs, w);
  }
  if (elem_num == 0 ||
      (lhs.isBitVector() && lhs.getBitVecSize() % elem_num != 0)) {
    elem_num = 1;
  }

  switch (node->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::FAdd:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
    return makeEqualExpr(*Factory, self, lhs.array_add(rhs, elem_num));
  case GuardedValueFlowOpcodeNode::OpcodeKind::FSub:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
    return makeEqualExpr(*Factory, self, lhs.array_sub(rhs, elem_num));
  case GuardedValueFlowOpcodeNode::OpcodeKind::FMul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Mul: {
    SMTExpr ret = makeEqualExpr(*Factory, self, lhs.array_mul(rhs, elem_num));
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs == 0 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::UDiv: {
    SMTExpr ret = makeEqualExpr(*Factory, self, lhs.array_udiv(rhs, elem_num));
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs == 0 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::FDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SDiv: {
    SMTExpr ret = makeEqualExpr(*Factory, self, lhs.array_sdiv(rhs, elem_num));
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs == 0 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::URem:
    return makeEqualExpr(*Factory, self, lhs.array_urem(rhs, elem_num));
  case GuardedValueFlowOpcodeNode::OpcodeKind::SRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FRem:
    return makeEqualExpr(*Factory, self, lhs.array_srem(rhs, elem_num));
  case GuardedValueFlowOpcodeNode::OpcodeKind::Shl: {
    SMTExpr ret = makeEqualExpr(*Factory, self, lhs.array_shl(rhs, elem_num));
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs >= 10 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::LShr: {
    SMTExpr ret = makeEqualExpr(*Factory, self, lhs.array_lshr(rhs, elem_num));
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs >= 10 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::AShr: {
    SMTExpr ret = makeEqualExpr(*Factory, self, lhs.array_ashr(rhs, elem_num));
    if (GVFGIgnoreBitVecOverflow.getValue())
      ret = ret && (lhs == 0 || rhs >= 10 || self != 0);
    return ret;
  }
  case GuardedValueFlowOpcodeNode::OpcodeKind::And:
    return makeEqualExpr(*Factory, self, lhs.array_and(rhs, elem_num));
  case GuardedValueFlowOpcodeNode::OpcodeKind::Or:
    return makeEqualExpr(*Factory, self, lhs.array_or(rhs, elem_num));
  case GuardedValueFlowOpcodeNode::OpcodeKind::Xor:
    return makeEqualExpr(*Factory, self, lhs.array_xor(rhs, elem_num));
  default:
    llvm_unreachable("Unsupported binary opcode");
  }
}

SMTExpr GuardedValueFlowSolver::encodeCompareOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  assert(node->getNumOperands() == 2 && "compare opcode must be binary");
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr lhs = getOrInsertExpr(node->getOperand(0));
  SMTExpr rhs = getOrInsertExpr(node->getOperand(1));
  unsigned elem_num = getVectorElementCount(node->getType());

  if (lhs.isBitVector() && rhs.isBitVector() &&
      lhs.getBitVecSize() != rhs.getBitVecSize()) {
    uint64_t w = std::max(lhs.getBitVecSize(), rhs.getBitVecSize());
    lhs = alignBitVecWidth(lhs, w);
    rhs = alignBitVecWidth(rhs, w);
  }
  if (elem_num == 0 ||
      (lhs.isBitVector() && lhs.getBitVecSize() % elem_num != 0)) {
    elem_num = 1;
  }

  switch (static_cast<CmpInst::Predicate>(node->getCmpPredicate())) {
  case CmpInst::FCMP_FALSE:
  case CmpInst::FCMP_UNO:
    return self == Factory->createBitVecVal(0, self.getBitVecSize());
  case CmpInst::FCMP_OEQ:
  case CmpInst::FCMP_UEQ:
  case CmpInst::ICMP_EQ:
    return makeEqualExpr(*Factory, self, lhs.array_eq(rhs, elem_num));
  case CmpInst::FCMP_ONE:
  case CmpInst::FCMP_UNE:
  case CmpInst::ICMP_NE:
    return makeEqualExpr(*Factory, self, lhs.array_ne(rhs, elem_num));
  case CmpInst::ICMP_UGT:
    return makeEqualExpr(*Factory, self, lhs.array_ugt(rhs, elem_num));
  case CmpInst::ICMP_UGE:
    return makeEqualExpr(*Factory, self, lhs.array_uge(rhs, elem_num));
  case CmpInst::ICMP_ULT:
    return makeEqualExpr(*Factory, self, lhs.array_ult(rhs, elem_num));
  case CmpInst::ICMP_ULE:
    return makeEqualExpr(*Factory, self, lhs.array_ule(rhs, elem_num));
  case CmpInst::FCMP_OGT:
  case CmpInst::FCMP_UGT:
  case CmpInst::ICMP_SGT:
    return makeEqualExpr(*Factory, self, lhs.array_sgt(rhs, elem_num));
  case CmpInst::FCMP_OGE:
  case CmpInst::FCMP_UGE:
  case CmpInst::ICMP_SGE:
    return makeEqualExpr(*Factory, self, lhs.array_sge(rhs, elem_num));
  case CmpInst::FCMP_OLT:
  case CmpInst::FCMP_ULT:
  case CmpInst::ICMP_SLT:
    return makeEqualExpr(*Factory, self, lhs.array_slt(rhs, elem_num));
  case CmpInst::FCMP_OLE:
  case CmpInst::FCMP_ULE:
  case CmpInst::ICMP_SLE:
    return makeEqualExpr(*Factory, self, lhs.array_sle(rhs, elem_num));
  case CmpInst::FCMP_ORD:
  case CmpInst::FCMP_TRUE:
    return self == Factory->createBitVecVal(static_cast<uint64_t>(-1),
                                            self.getBitVecSize());
  default:
    llvm_unreachable("Unsupported compare predicate");
  }
}

SMTExpr GuardedValueFlowSolver::encodeCastOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  assert(node->getNumOperands() == 1 && "cast opcode must be unary");
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr child = getOrInsertExpr(node->getOperand(0));
  unsigned elem_num = getVectorElementCount(node->getType());
  uint64_t origin_size = child.getBitVecSize();
  uint64_t target_size = self.getBitVecSize();
  if (elem_num == 0 || origin_size % elem_num != 0 ||
      target_size % elem_num != 0) {
    elem_num = 1;
  }

  switch (node->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::Trunc:
    if (origin_size > target_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    if (origin_size < target_size)
      return self == child.array_zext(target_size - origin_size, elem_num);
    return self == child;
  case GuardedValueFlowOpcodeNode::OpcodeKind::ZExt:
    if (target_size > origin_size)
      return self == child.array_zext(target_size - origin_size, elem_num);
    if (target_size < origin_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    return self == child;
  case GuardedValueFlowOpcodeNode::OpcodeKind::SExt:
    if (target_size > origin_size)
      return self == child.array_sext(target_size - origin_size, elem_num);
    if (target_size < origin_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    return self == child;
  case GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr:
    if (origin_size < target_size)
      return self == child.array_zext(target_size - origin_size, elem_num);
    if (origin_size > target_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    return self == child;
  case GuardedValueFlowOpcodeNode::OpcodeKind::BitCast:
    return makeEqualExpr(*Factory, self, child);
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI:
    if (target_size == origin_size)
      return self == child;
    if (target_size < origin_size)
      return self == child.array_trunc(origin_size - target_size, elem_num);
    return self == child.array_sext(target_size - origin_size, elem_num);
  case GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast:
    return Factory->createBoolVal(true);
  default:
    llvm_unreachable("Unsupported cast opcode");
  }
}

SMTExpr GuardedValueFlowSolver::encodeGEPOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  if (node->operands().empty())
    return Factory->createBoolVal(true);

  SMTExpr self = getOrInsertExpr(node);
  SMTExpr base = alignBitVecWidth(getOrInsertExpr(node->getOperand(0)),
                                  self.getBitVecSize());
  SMTExpr computed =
      node->getNumOperands() >= 2
          ? alignBitVecWidth(getOrInsertExpr(node->getOperand(1)),
                             self.getBitVecSize())
          : base;
  SMTExpr null_expr = Factory->createBitVecVal(0, self.getBitVecSize());

  return (self == (base == null_expr).basic_ite(null_expr, computed)) &&
         (base == null_expr || computed != null_expr);
}

SMTExpr GuardedValueFlowSolver::encodeSelectOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  assert(node->getNumOperands() == 3 && "select opcode must have 3 operands");
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr cond = getOrInsertExpr(node->conditionOperand());
  SMTExpr true_value = alignBitVecWidth(
      getOrInsertExpr(node->trueValueOperand()), self.getBitVecSize());
  SMTExpr false_value = alignBitVecWidth(
      getOrInsertExpr(node->falseValueOperand()), self.getBitVecSize());
  unsigned elem_num =
      getVectorElementCount(node->conditionOperand()->getType());
  if (elem_num <= 1 || cond.getBitVecSize() != elem_num ||
      self.getBitVecSize() % elem_num != 0) {
    return self == toBoolExpr(*Factory, cond).basic_ite(true_value, false_value);
  }
  return self == cond.array_ite(true_value, false_value, elem_num);
}

SMTExpr GuardedValueFlowSolver::encodeExtractElementOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  assert(node->getNumOperands() == 2 &&
         "extractelement opcode must have 2 operands");
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr vec = getOrInsertExpr(node->vectorOperand());
  const GuardedValueFlowNode *index_node = node->indexOperand();
  Value *index_value = index_node ? index_node->getLLVMValue() : nullptr;
  unsigned elem_num =
      getVectorElementCount(node->vectorOperand()->getType());

  if (auto *constant_index = dyn_cast_or_null<ConstantInt>(index_value))
    return self == vec.array_elmt(elem_num, constant_index->getZExtValue());

  SMTExpr index_expr = getOrInsertExpr(index_node);
  SMTExpr ret = (index_expr != 0 || self == vec.array_elmt(elem_num, 0));
  for (unsigned idx = 1; idx < elem_num; ++idx)
    ret = ret && (index_expr != static_cast<int>(idx) ||
                  self == vec.array_elmt(elem_num, idx));
  return ret && (index_expr >= 0 && index_expr < static_cast<int>(elem_num));
}

SMTExpr GuardedValueFlowSolver::encodeInsertElementOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  assert(node->getNumOperands() == 3 &&
         "insertelement opcode must have 3 operands");
  SMTExpr self = getOrInsertExpr(node);
  SMTExpr vec = getOrInsertExpr(node->vectorOperand());
  SMTExpr value = getOrInsertExpr(node->insertedValue());
  const GuardedValueFlowNode *index_node = node->indexOperand();
  Value *index_value = index_node ? index_node->getLLVMValue() : nullptr;

  Type *vec_type = node->vectorOperand()->getType();
  unsigned elem_num = getVectorElementCount(vec_type);
  uint64_t vec_size = vec.getBitVecSize();
  uint64_t elem_size = vec_size / std::max(1u, elem_num);

  auto build_case = [&](unsigned idx) {
    if (idx == 0) {
      return self.array_elmt(elem_num, 0) == value &&
             self.basic_extract(vec_size - elem_size - 1, 0) ==
                 vec.basic_extract(vec_size - elem_size - 1, 0);
    }
    if (idx == elem_num - 1) {
      return self.array_elmt(elem_num, elem_num - 1) == value &&
             self.basic_extract(vec_size - 1, elem_size) ==
                 vec.basic_extract(vec_size - 1, elem_size);
    }
    return self.array_elmt(elem_num, idx) == value &&
           self.basic_extract(vec_size - 1, vec_size - idx * elem_size) ==
               vec.basic_extract(vec_size - 1, vec_size - idx * elem_size) &&
           self.basic_extract(vec_size - (idx + 1) * elem_size - 1, 0) ==
               vec.basic_extract(vec_size - (idx + 1) * elem_size - 1, 0);
  };

  if (auto *constant_index = dyn_cast_or_null<ConstantInt>(index_value))
    return build_case(constant_index->getZExtValue());

  SMTExpr index_expr = getOrInsertExpr(index_node);
  SMTExpr ret = (index_expr != 0 || build_case(0));
  for (unsigned idx = 1; idx < elem_num; ++idx)
    ret = ret && (index_expr != static_cast<int>(idx) || build_case(idx));
  return ret && (index_expr >= 0 && index_expr < static_cast<int>(elem_num));
}

SMTExpr GuardedValueFlowSolver::encodeConcatOpcodeNode(
    const GuardedValueFlowOpcodeNode *node) {
  if (node->getNumOperands() == 0)
    return Factory->createBoolVal(true);
  SMTExpr lhs = getOrInsertExpr(node);
  SMTExpr rhs = Factory->createEmptySMTExpr();
  bool has_rhs = false;
  for (unsigned idx = 0; idx < node->getNumOperands(); ++idx) {
    const GuardedValueFlowNode *op = node->getOperand(idx);
    if (!op || !op->getType() || !op->getType()->isSized())
      continue;
    if (DL.getTypeSizeInBits(op->getType()) == 0)
      continue;
    SMTExpr next = getOrInsertExpr(op);
    rhs = has_rhs ? rhs.basic_concat(next) : next;
    has_rhs = true;
  }
  if (!has_rhs)
    return Factory->createBoolVal(true);
  if (lhs.getBitVecSize() != rhs.getBitVecSize()) {
    if (rhs.getBitVecSize() < lhs.getBitVecSize())
      rhs = rhs.basic_zext(lhs.getBitVecSize() - rhs.getBitVecSize());
    else
      rhs = rhs.basic_extract(lhs.getBitVecSize() - 1, 0);
  }
  return lhs == rhs;
}

std::pair<SMTExprVec, SMTExprVec> GuardedValueFlowSolver::_getPhiGated(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming) {
  auto *graph = incoming.value_node->getGraph();
  assert(graph && incoming.incoming_block);

  auto gated = _getCtrlDeps(incoming.incoming_block, graph);
  SMTExprVec gated_ctrl = gated.first.copy();
  SMTExprVec gated_data = gated.second;

  if (incoming.condition_node) {
    SMTExpr cond_expr =
        toBoolExpr(*Factory, getOrInsertExpr(incoming.condition_node));
    gated_ctrl.push_back(incoming.condition_sense ? cond_expr : !cond_expr);
    gated_data =
        SMTExprVec::merge(gated_data, _getDataDeps(incoming.condition_node));
  }
  return {gated_ctrl, gated_data};
}

std::pair<SMTExprVec, SMTExprVec>
GuardedValueFlowSolver::computeCtrlDepsPair(BasicBlock *block,
                                            const GuardedValueFlowGraph *graph,
                                            const QueryContext *context) {
  (void)context;
  return _getCtrlDeps(block, graph);
}

std::pair<SMTExprVec, SMTExprVec> GuardedValueFlowSolver::computePhiGatedPair(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming, const QueryContext *context) {
  (void)context;
  return _getPhiGated(phi_node, incoming);
}

SMTExprVec
GuardedValueFlowSolver::computeDataDeps(const GuardedValueFlowNode *node,
                                        const QueryContext *context) {
  (void)context;
  return _getDataDeps(node);
}

std::pair<SMTExprVec, SMTExprVec> GuardedValueFlowSolver::_getCtrlDeps(
    BasicBlock *block, const GuardedValueFlowGraph *graph, size_t depth) {
  assert(block && graph);

  if (GVFGUseRegionAsCtrlDep.getValue()) {
    auto *region =
        const_cast<GuardedValueFlowGraph *>(graph)->findRegion(block);
    if (!region)
      region =
          const_cast<GuardedValueFlowGraph *>(graph)->getAlwaysTrueRegion();
    SMTExprVec ctrl = Factory->createEmptySMTExprVec();
    SMTExprVec data = _getDataDeps(region);
    ctrl.push_back(toBoolExpr(*Factory, getOrInsertExpr(region)));
    return {ctrl, data};
  }

  // Loop control dependencies can be cyclic. Abstract a recursive revisit in
  // the same way as a depth cutoff, rather than expanding it indefinitely.
  if (depth >= GVFGCDMaxDepth || ActiveCtrlBlocks.count(block)) {
    SMTExprVec ctrl = Factory->createEmptySMTExprVec();
    SMTExprVec data = Factory->createEmptySMTExprVec();
    ctrl.push_back(Factory->createBoolConst(
        (Twine("BB") + Twine(reinterpret_cast<uintptr_t>(block))).str()));
    return {ctrl, data};
  }

  auto cached = CtrlCacheMap.find(block);
  if (cached != CtrlCacheMap.end()) {
    SMTExprVec ctrl = cached->second;
    SMTExprVec data = Factory->createEmptySMTExprVec();

    if (BBCache.contains(block))
      return {ctrl, data};

    std::unordered_set<BasicBlock *> visited;
    SmallVector<
        std::pair<const GuardedValueFlowGraph::BlockCondition *, size_t>, 8>
        data_dep_nodes;
    for (const auto &cond : graph->getBlockConditions(block))
      data_dep_nodes.push_back({&cond, depth});

    while (!data_dep_nodes.empty()) {
      auto entry = data_dep_nodes.pop_back_val();
      const GuardedValueFlowGraph::BlockCondition *data_dep_node = entry.first;
      size_t dfs_depth = entry.second;
      if (dfs_depth >= GVFGCDMaxDepth)
        continue;

      visited.insert(data_dep_node->control_block);
      data =
          SMTExprVec::merge(data, _getDataDeps(data_dep_node->condition_node));

      BasicBlock *control_block = data_dep_node->control_block;
      if (control_block && !BBCache.contains(control_block)) {
        BBCache.add(control_block);
        for (const auto &cond : graph->getBlockConditions(control_block)) {
          if (!visited.count(cond.control_block))
            data_dep_nodes.push_back({&cond, dfs_depth + 1});
        }
      }
    }

    return {ctrl, data};
  }

  SMTExprVec ctrl = Factory->createEmptySMTExprVec();
  SMTExprVec data = Factory->createEmptySMTExprVec();

  ActiveCtrlBlocks.insert(block);
  auto active_guard = make_scope_exit([&] { ActiveCtrlBlocks.erase(block); });

  auto block_conditions = graph->getBlockConditions(block);
  if (!block_conditions.empty()) {
    SMTExprVec conditions = Factory->createEmptySMTExprVec();
    for (const auto &cond : block_conditions) {
      SMTExpr cond_expr =
          toBoolExpr(*Factory, getOrInsertExpr(cond.condition_node));
      auto pair = _getCtrlDeps(cond.control_block, graph, depth + 1);
      conditions.push_back((cond.sense ? cond_expr : !cond_expr) &&
                           pair.first.toAndExpr());
      data = SMTExprVec::merge(data, pair.second);
    }

    ctrl.push_back(conditions.toOrExpr());
    if (!BBCache.contains(block)) {
      BBCache.add(block);
      for (const auto &cond : block_conditions)
        data = SMTExprVec::merge(data, _getDataDeps(cond.condition_node));
    }
  }

  CtrlCacheMap.insert({block, ctrl});
  return {ctrl, data};
}

void GuardedValueFlowSolver::collectDataDeps(const GuardedValueFlowNode *node,
                                             size_t depth, SMTExprVec &out) {
  if (!node)
    return;

  if (depth >= GVFGDDMaxDepth)
    return;

  if (ConstraintCache.contains(node)) {
    if (GVFGDDMaxDepth.getNumOccurrences()) {
      for (const auto &edge : node->children()) {
        if (edge.target && !isTerminalNode(edge.target))
          collectDataDeps(edge.target, depth + 1, out);
      }
    }
    return;
  }
  ConstraintCache.add(node);

  if (isResultLikeNode(node)) {
    SMTExpr self = getOrInsertExpr(node);

    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      for (const auto &incoming : phi_node->incoming()) {
        auto gated = _getPhiGated(phi_node, incoming);
        out.push_back((!gated.first.toAndExpr()) ||
                      makeEqualExpr(*Factory, self,
                                    getOrInsertExpr(incoming.value_node)));
        out.mergeWithAnd(gated.second);
      }
    } else if (auto *return_node = dyn_cast<GuardedValueFlowReturnNode>(node)) {
      for (const auto &incoming : return_node->incomingReturns()) {
        auto *child = incoming.value;
        auto *site = incoming.site;
        BasicBlock *site_block = site && site->getInstruction()
                                     ? site->getInstruction()->getParent()
                                     : return_node->getParentBasicBlock();
        auto gated = _getCtrlDeps(site_block, return_node->getGraph());
        out.push_back(!gated.first.toAndExpr() ||
                      makeEqualExpr(*Factory, self, getOrInsertExpr(child)));
        out.mergeWithAnd(gated.second);
      }
    } else if (node->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      for (const auto &edge : node->children()) {
        auto *child = edge.target;
        if (!child || shouldExcludeSummaryBackedChild(node, child))
          continue;
        auto *region = node->getMatchingRegion(child);
        SMTExpr cond_expr =
            region ? toBoolExpr(*Factory, getOrInsertExpr(region))
                   : Factory->createBoolVal(true);
        if (region)
          collectDataDeps(region, 0, out);
        out.push_back((!cond_expr) ||
                      makeEqualExpr(*Factory, self, getOrInsertExpr(child)));
      }
    }

    if (!isTerminalNode(node)) {
      SMTExprVec cases = Factory->createEmptySMTExprVec();
      for (const auto &edge : node->children()) {
        if (!edge.target || shouldExcludeSummaryBackedChild(node, edge.target))
          continue;
        cases.push_back(
            makeEqualExpr(*Factory, self, getOrInsertExpr(edge.target)));
      }

      if (!cases.empty())
        out.push_back(cases.toOrExpr());

      for (const auto &edge : node->children()) {
        if (edge.target && !shouldExcludeSummaryBackedChild(node, edge.target))
          collectDataDeps(edge.target, depth + 1, out);
      }
    } else if (isNonNullTerminal(node)) {
      out.push_back(self != 0);
    }
  } else if (node->getKind() == GuardedValueFlowNode::Kind::Unknown) {
    // Preserve opaque dependencies for graph traversal without claiming that
    // the unsupported operation's result equals one of its operands.
    for (const auto &edge : node->children()) {
      if (edge.target)
        collectDataDeps(edge.target, depth + 1, out);
    }
  } else if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    out.push_back(encodeOpcodeNode(opcode_node));
    for (const auto &edge : node->children()) {
      if (edge.target)
        collectDataDeps(edge.target, depth + 1, out);
    }
  } else {
    llvm_unreachable("Unhandled guarded value-flow node category");
  }
}

SMTExprVec
GuardedValueFlowSolver::_getDataDeps(const GuardedValueFlowNode *node,
                                     size_t depth) {
  SMTExprVec ret = Factory->createEmptySMTExprVec();
  collectDataDeps(node, depth, ret);
  return ret;
}

SMTExprVec GuardedValueFlowSolver::getDeps(const GuardedValueFlowNode *node,
                                           const GuardedValueFlowNode *child) {
  assert(node && (!child || child->containsParent(node)));

  SMTExprVec ret = Factory->createEmptySMTExprVec();
  if (node->getKind() == GuardedValueFlowNode::Kind::Unknown) {
    ret = SMTExprVec::merge(_getDataDeps(node), ret);
  } else if (isa<GuardedValueFlowOpcodeNode>(node) || !child) {
    if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
      if (opcode_node->getOpcodeKind() ==
              GuardedValueFlowOpcodeNode::OpcodeKind::Select &&
          child) {
        ConstraintCache.add(node);
        SMTExpr node_expr = getOrInsertExpr(node);
        SMTExpr child_expr = getOrInsertExpr(child);
        ret.push_back(makeEqualExpr(*Factory, node_expr, child_expr));
        bool is_true_value = opcode_node->trueValueOperand() == child;
        bool is_false_value = opcode_node->falseValueOperand() == child;
        if (is_true_value != is_false_value) {
          SMTExpr cond_expr = toBoolExpr(
              *Factory, getOrInsertExpr(opcode_node->conditionOperand()));
          ret.push_back(is_true_value ? cond_expr : !cond_expr);
        }
        ret = SMTExprVec::merge(
            _getDataDeps(opcode_node->conditionOperand()), ret);
      } else {
        ret = SMTExprVec::merge(_getDataDeps(node), ret);
      }
    } else {
      ret = SMTExprVec::merge(_getDataDeps(node), ret);
    }
  } else {
    ConstraintCache.add(node);
    SMTExpr node_expr = getOrInsertExpr(node);
    SMTExpr child_expr = getOrInsertExpr(child);
    ret.push_back(makeEqualExpr(*Factory, node_expr, child_expr));

    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      SMTExprVec gated_ctrls = Factory->createEmptySMTExprVec();
      for (const auto &incoming : phi_node->incoming()) {
        if (incoming.value_node == child) {
          auto pair = _getPhiGated(phi_node, incoming);
          gated_ctrls.push_back(pair.first.toAndExpr());
          ret = SMTExprVec::merge(ret, pair.second);
        }
      }
      ret.push_back(gated_ctrls.toOrExpr());
      return ret;
    } else if (node->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      auto *region = node->getMatchingRegion(child);
      if (region) {
        ret = SMTExprVec::merge(ret, _getDataDeps(region));
        ret.push_back(toBoolExpr(*Factory, getOrInsertExpr(region)));
      }
    }
  }

  if (!child || node->getParentBasicBlock() != child->getParentBasicBlock()) {
    auto pair = _getCtrlDeps(node->getParentBasicBlock(), node->getGraph());
    ret.mergeWithAnd(pair.first);
    ret = SMTExprVec::merge(ret, pair.second);
  }
  return ret;
}

std::pair<SMTExprVec, SMTExprVec>
GuardedValueFlowSolver::getDepsPair(const GuardedValueFlowNode *node,
                                    const GuardedValueFlowNode *child) {
  assert(node && (!child || child->containsParent(node)));
  SMTExprVec ctrl = Factory->createEmptySMTExprVec();
  SMTExprVec data = Factory->createEmptySMTExprVec();

  data = SMTExprVec::merge(_getDataDeps(node), data);
  if (!(isa<GuardedValueFlowOpcodeNode>(node) || !child)) {
    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      SMTExprVec gated = Factory->createEmptySMTExprVec();
      for (const auto &incoming : phi_node->incoming()) {
        if (incoming.value_node == child)
          gated.push_back(_getPhiGated(phi_node, incoming).first.toAndExpr());
      }
      ctrl.push_back(gated.toOrExpr());
      return {ctrl, data};
    } else if (node->getKind() == GuardedValueFlowNode::Kind::LoadMemory) {
      if (auto *region = node->getMatchingRegion(child))
        ctrl.push_back(toBoolExpr(*Factory, getOrInsertExpr(region)));
    }
  } else if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    if (opcode_node->getOpcodeKind() ==
            GuardedValueFlowOpcodeNode::OpcodeKind::Select &&
        child) {
      bool is_true_value = opcode_node->trueValueOperand() == child;
      bool is_false_value = opcode_node->falseValueOperand() == child;
      if (is_true_value != is_false_value) {
        SMTExpr cond_expr = toBoolExpr(
            *Factory, getOrInsertExpr(opcode_node->conditionOperand()));
        ctrl.push_back(is_true_value ? cond_expr : !cond_expr);
      }
    }
  }

  if (!child || node->getParentBasicBlock() != child->getParentBasicBlock()) {
    auto pair = _getCtrlDeps(node->getParentBasicBlock(), node->getGraph());
    ctrl = SMTExprVec::merge(ctrl, pair.first);
    data = SMTExprVec::merge(data, pair.second);
  }
  return {ctrl, data};
}

void GuardedValueFlowSolver::push() {
  SMTSolver::push();
  ConstraintCache.push();
  FunctionArgumentCache.push();
  CallSiteOutputCache.push();
  BBCache.push();
}

void GuardedValueFlowSolver::pop(unsigned n) {
  SMTSolver::pop(n);
  ConstraintCache.pop(n);
  FunctionArgumentCache.pop(n);
  CallSiteOutputCache.pop(n);
  BBCache.pop(n);
}

void GuardedValueFlowSolver::reset() {
  SMTSolver::reset();
  ConstraintCache.reset();
  FunctionArgumentCache.reset();
  CallSiteOutputCache.reset();
  BBCache.reset();
  CtrlCacheMap.clear();
  OpcodeConstraintsCacheMap.clear();
  NodeExprMap.clear();
  NodeSymbolNameMap.clear();
}

GuardedValueFlowSolver::SMTResultType GuardedValueFlowSolver::check() {
  return SMTSolver::check();
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::computeCtrlDepsPair(
    BasicBlock *block, const GuardedValueFlowGraph *graph,
    const QueryContext *context) {
  BasicBlock *prev_block = context ? context->previous_block : nullptr;
  return _getCtrlDepsWrapper(block, graph, prev_block);
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::computePhiGatedPair(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming, const QueryContext *context) {
  BasicBlock *prev_block = context ? context->previous_block : nullptr;
  if (!prev_block && phi_node) {
    if (BasicBlock *bb = phi_node->getParentBasicBlock())
      prev_block = &bb->getParent()->getEntryBlock();
  }
  if (!prev_block)
    return {Factory->createEmptySMTExprVec(), Factory->createEmptySMTExprVec()};
  return _getPhiGated(phi_node, incoming, prev_block);
}

SMTExprVec
DTGuardedValueFlowSolver::computeDataDeps(const GuardedValueFlowNode *node,
                                          const QueryContext *context) {
  if (!node)
    return Factory->createEmptySMTExprVec();
  BasicBlock *prev_block = context ? context->previous_block : nullptr;
  if (!prev_block) {
    if (BasicBlock *bb = node->getParentBasicBlock()) {
      prev_block = &bb->getParent()->getEntryBlock();
    } else if (auto *graph = node->getGraph()) {
      if (Function *func = graph->getBaseFunction()) {
        if (!func->empty())
          prev_block = &func->getEntryBlock();
      }
    }
  }
  if (!prev_block)
    return Factory->createEmptySMTExprVec();
  return _getDataDeps(node, prev_block);
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::_getPhiGated(
    const GuardedValueFlowPhiNode *phi_node,
    GuardedValueFlowPhiNode::Incoming incoming, BasicBlock *prev_block) {
  assert(prev_block && incoming.incoming_block);
  auto pair = _getCtrlDepsWrapper(incoming.incoming_block, phi_node->getGraph(),
                                  prev_block);
  SMTExprVec ctrl = pair.first.copy();
  SMTExprVec data = pair.second;
  if (incoming.condition_node) {
    SMTExpr cond_expr =
        toBoolExpr(*Factory, getOrInsertExpr(incoming.condition_node));
    ctrl.push_back(incoming.condition_sense ? cond_expr : !cond_expr);
    data = SMTExprVec::merge(_getDataDeps(incoming.condition_node, prev_block),
                             data);
  }
  return {ctrl, data};
}

void DTGuardedValueFlowSolver::collectDataDeps(const GuardedValueFlowNode *node,
                                               BasicBlock *prev_block,
                                               SMTExprVec &out) {
  assert(prev_block);
  if (!node || ConstraintCache.contains(node))
    return;
  ConstraintCache.add(node);

  if (isResultLikeNode(node)) {
    SMTExpr self = getOrInsertExpr(node);
    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      for (const auto &incoming : phi_node->incoming()) {
        auto pair = _getPhiGated(phi_node, incoming, prev_block);
        out.push_back((!pair.first.toAndExpr()) ||
                      makeEqualExpr(*Factory, self,
                                    getOrInsertExpr(incoming.value_node)));
        out.mergeWithAnd(pair.second);
      }
    }
    if (!isTerminalNode(node)) {
      SMTExprVec cases = Factory->createEmptySMTExprVec();
      for (const auto &edge : node->children()) {
        if (!edge.target || shouldExcludeSummaryBackedChild(node, edge.target))
          continue;
        cases.push_back(
            makeEqualExpr(*Factory, self, getOrInsertExpr(edge.target)));
      }
      if (!cases.empty())
        out.push_back(cases.toOrExpr());
      for (const auto &edge : node->children()) {
        if (edge.target &&
            !shouldExcludeSummaryBackedChild(node, edge.target) &&
            !isTerminalNode(edge.target))
          collectDataDeps(edge.target, prev_block, out);
      }
    }
  } else if (node->getKind() == GuardedValueFlowNode::Kind::Unknown) {
    for (const auto &edge : node->children()) {
      if (edge.target && !isTerminalNode(edge.target))
        collectDataDeps(edge.target, prev_block, out);
    }
  } else if (auto *opcode_node = dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    out.push_back(encodeOpcodeNode(opcode_node));
    for (const auto &edge : node->children()) {
      if (edge.target && !isTerminalNode(edge.target))
        collectDataDeps(edge.target, prev_block, out);
    }
  }
}

SMTExprVec
DTGuardedValueFlowSolver::_getDataDeps(const GuardedValueFlowNode *node,
                                       BasicBlock *prev_block) {
  SMTExprVec ret = Factory->createEmptySMTExprVec();
  collectDataDeps(node, prev_block, ret);
  return ret;
}

std::pair<SMTExprVec, SMTExprVec> DTGuardedValueFlowSolver::_getCtrlDepsWrapper(
    BasicBlock *block, const GuardedValueFlowGraph *graph,
    BasicBlock *prev_block) {
  if (!prev_block)
    prev_block = &block->getParent()->getEntryBlock();

  if (!(DT->dominates(prev_block, block) || DT->dominates(block, prev_block))) {
    prev_block = DT->findNearestCommonDominator(block, prev_block);
    if (!prev_block)
      prev_block = &block->getParent()->getEntryBlock();
  }
  return _getCtrlDeps(block, graph, prev_block);
}

std::pair<SMTExprVec, SMTExprVec>
DTGuardedValueFlowSolver::_getCtrlDeps(BasicBlock *block,
                                       const GuardedValueFlowGraph *graph,
                                       BasicBlock *prev_block) {
  assert(block && graph && prev_block && DT);

  SMTExprVec data = Factory->createEmptySMTExprVec();
  if (!DT->dominates(prev_block, block))
    return {Factory->createEmptySMTExprVec(), data};

  if (ActiveCtrlBlocks.count(block)) {
    SMTExprVec ctrl = Factory->createEmptySMTExprVec();
    ctrl.push_back(Factory->createBoolConst(
        (Twine("BB") + Twine(reinterpret_cast<uintptr_t>(block))).str()));
    return {ctrl, data};
  }

  ActiveCtrlBlocks.insert(block);
  auto active_guard = make_scope_exit([&] { ActiveCtrlBlocks.erase(block); });

  auto cached = CtrlCacheMap.find(block);
  if (cached != CtrlCacheMap.end()) {
    BasicBlock *cached_prev = cached->second.second;

    if (!BBCache.contains(block)) {
      SmallVector<const GuardedValueFlowGraph::BlockCondition *, 8>
          data_dep_nodes;
      DenseSet<BasicBlock *> visited;
      for (const auto &cond : graph->getBlockConditions(block)) {
        if (cond.control_block && DT->dominates(prev_block, cond.control_block))
          data_dep_nodes.push_back(&cond);
      }

      while (!data_dep_nodes.empty()) {
        const auto *data_dep_node = data_dep_nodes.pop_back_val();
        if (data_dep_node->control_block)
          visited.insert(data_dep_node->control_block);
        collectDataDeps(data_dep_node->condition_node, prev_block, data);

        BasicBlock *control_block = data_dep_node->control_block;
        if (control_block && !BBCache.contains(control_block)) {
          BBCache.add(control_block);
          for (const auto &cond : graph->getBlockConditions(control_block)) {
            if (cond.control_block && !visited.count(cond.control_block))
              data_dep_nodes.push_back(&cond);
          }
        }
      }
    }

    if (DT->dominates(cached_prev, prev_block)) {
      return {CtrlCacheMap.at(block).first, data};
    } else if (cached_prev && cached_prev != block &&
               DT->dominates(prev_block, cached_prev)) {
      auto partial = _getCtrlDeps(cached_prev, graph, prev_block);
      auto &updated = CtrlCacheMap.at(block);
      updated.first.mergeWithAnd(partial.first);
      updated.second = prev_block;
      return {updated.first, SMTExprVec::merge(data, partial.second)};
    }
    // Unreachable blocks are dominated by every block in LLVM's DominatorTree,
    // so cached_prev and prev_block can be incomparable.
    return {CtrlCacheMap.at(block).first, data};
  }

  SMTExprVec ctrl = Factory->createEmptySMTExprVec();
  auto block_conditions = graph->getBlockConditions(block);
  if (!block_conditions.empty()) {
    SMTExprVec conditions = Factory->createEmptySMTExprVec();
    for (const auto &cond : block_conditions) {
      if (!cond.control_block || !DT->dominates(prev_block, cond.control_block))
        continue;
      SMTExpr cond_expr =
          toBoolExpr(*Factory, getOrInsertExpr(cond.condition_node));
      auto pair = _getCtrlDeps(cond.control_block, graph, prev_block);
      conditions.push_back((cond.sense ? cond_expr : !cond_expr) &&
                           pair.first.toAndExpr());
      data = SMTExprVec::merge(data, pair.second);
    }

    ctrl.push_back(conditions.toOrExpr());
    if (!BBCache.contains(block)) {
      BBCache.add(block);
      for (const auto &cond : block_conditions) {
        if (cond.control_block && DT->dominates(prev_block, cond.control_block))
          data = SMTExprVec::merge(
              data, _getDataDeps(cond.condition_node, prev_block));
      }
    }
  }

  CtrlCacheMap.insert({block, {ctrl, prev_block}});
  return {ctrl, data};
}

SMTExprVec
DTGuardedValueFlowSolver::getDeps(const GuardedValueFlowNode *node,
                                  const GuardedValueFlowNode *child) {
  BasicBlock *current_block = node->getParentBasicBlock();
  BasicBlock *prev_block = child ? child->getParentBasicBlock()
                                 : &current_block->getParent()->getEntryBlock();

  SMTExprVec ret = Factory->createEmptySMTExprVec();
  if (node->getKind() == GuardedValueFlowNode::Kind::Unknown ||
      isa<GuardedValueFlowOpcodeNode>(node) || !child) {
    ret = SMTExprVec::merge(ret, _getDataDeps(node, prev_block));
  } else {
    ConstraintCache.add(node);
    SMTExpr node_expr = getOrInsertExpr(node);
    SMTExpr child_expr = getOrInsertExpr(child);
    ret.push_back(makeEqualExpr(*Factory, node_expr, child_expr));

    if (auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(node)) {
      SMTExprVec gated_ctrls = Factory->createEmptySMTExprVec();
      for (const auto &incoming : phi_node->incoming()) {
        if (incoming.value_node == child) {
          auto pair = _getPhiGated(phi_node, incoming, prev_block);
          gated_ctrls.push_back(pair.first.toAndExpr());
          ret = SMTExprVec::merge(ret, pair.second);
        }
      }
      ret.push_back(gated_ctrls.toOrExpr());
      return ret;
    }
  }

  auto pair = _getCtrlDepsWrapper(current_block, node->getGraph(), prev_block);
  ret.mergeWithAnd(pair.first);
  ret = SMTExprVec::merge(ret, pair.second);
  return ret;
}

void DTGuardedValueFlowSolver::reset() {
  GuardedValueFlowSolver::reset();
  CtrlCacheMap.clear();
}
