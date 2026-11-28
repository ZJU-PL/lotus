#include "Concurrency/CUDA/CUDAParticipantAnalysis.h"

#include "Concurrency/API/ThreadAPI.h"

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>

namespace concurrency::cuda {

namespace {

ParticipationScope classifyByUniformity(UniformityClass uniformity) {
  switch (uniformity) {
  case UniformityClass::WarpUniform:
    return ParticipationScope::Warp;
  case UniformityClass::BlockUniform:
    return ParticipationScope::Block;
  case UniformityClass::ThreadVarying:
    return ParticipationScope::Grid;
  default:
    return ParticipationScope::Unknown;
  }
}

bool isSynchronizationInstruction(const llvm::Instruction *inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!call) {
    return false;
  }
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api) {
    return false;
  }
  auto type = api->getType(call);
  return type == ThreadAPI::TD_CUDA_BARRIER ||
         type == ThreadAPI::TD_CUDA_WARP_BARRIER ||
         type == ThreadAPI::TD_CUDA_MEMORY_BARRIER ||
         type == ThreadAPI::TD_CUDA_DEVICE_SYNC;
}

void fillInstructionMetadata(CUDAParticipantSet &result,
                             const llvm::Function &kernel,
                             const llvm::Instruction *inst) {
  result.kernel = &kernel;
  result.instruction = inst;
}

bool mustReachInstruction(const llvm::Function &kernel,
                          const llvm::Instruction *inst) {
  if (!inst || kernel.empty()) {
    return false;
  }
  llvm::PostDominatorTree post_dom_tree;
  post_dom_tree.recalculate(const_cast<llvm::Function &>(kernel));
  return post_dom_tree.getNode(inst->getParent()) &&
         post_dom_tree.getNode(&kernel.getEntryBlock()) &&
         post_dom_tree.dominates(inst->getParent(), &kernel.getEntryBlock());
}

std::optional<uint32_t> getSimplePredicateMask(const llvm::BranchInst *branch,
                                               bool take_true,
                                               uint32_t warp_size) {
  const auto *compare =
      branch && branch->isConditional()
          ? llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition())
          : nullptr;
  if (!compare || warp_size == 0 || warp_size > 32) {
    return std::nullopt;
  }
  const llvm::Value *builtin = compare->getOperand(0);
  const auto *constant =
      llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1));
  llvm::ICmpInst::Predicate predicate = compare->getPredicate();
  if (!constant) {
    builtin = compare->getOperand(1);
    constant = llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(0));
    predicate = llvm::ICmpInst::getSwappedPredicate(predicate);
  }
  const BuiltinKind kind = CUDASymbolicModel::classifyBuiltin(builtin);
  if (!constant ||
      (kind != BuiltinKind::ThreadIdxX && kind != BuiltinKind::LaneId)) {
    return std::nullopt;
  }
  uint32_t mask = 0;
  const llvm::APInt rhs = constant->getValue();
  for (uint32_t lane = 0; lane < warp_size; ++lane) {
    const llvm::APInt lhs(rhs.getBitWidth(), lane);
    bool value = false;
    switch (predicate) {
    case llvm::ICmpInst::ICMP_EQ:
      value = lhs == rhs;
      break;
    case llvm::ICmpInst::ICMP_NE:
      value = lhs != rhs;
      break;
    case llvm::ICmpInst::ICMP_ULT:
      value = lhs.ult(rhs);
      break;
    case llvm::ICmpInst::ICMP_ULE:
      value = lhs.ule(rhs);
      break;
    case llvm::ICmpInst::ICMP_UGT:
      value = lhs.ugt(rhs);
      break;
    case llvm::ICmpInst::ICMP_UGE:
      value = lhs.uge(rhs);
      break;
    default:
      return std::nullopt;
    }
    if (value == take_true) {
      mask |= uint32_t{1} << lane;
    }
  }
  return mask;
}

} // anonymous namespace

CUDAParticipantAnalysis::CUDAParticipantAnalysis(const llvm::Function &kernel,
                                                 uint32_t warp_size)
    : m_kernel(kernel), m_warp_size(warp_size) {}

CUDAParticipantSet CUDAParticipantAnalysis::getActiveParticipants(
    const llvm::Instruction *inst) const {
  if (!inst) {
    return CUDAParticipantSet{};
  }

  CUDAParticipantSet result;
  fillInstructionMetadata(result, m_kernel, inst);

  if (isSynchronizationInstruction(inst)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
    if (call) {
      ThreadAPI *api = ThreadAPI::getThreadAPI();
      if (api) {
        auto type = api->getType(call);
        if (type == ThreadAPI::TD_CUDA_WARP_BARRIER) {
          result.scopes.push_back(static_cast<int>(ParticipationScope::Warp));
          const bool must_reach = mustReachInstruction(m_kernel, inst);
          const auto *mask =
              call->arg_empty()
                  ? nullptr
                  : llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
          if (!mask) {
            result.min_lane = 0;
            result.max_lane = m_warp_size - 1;
            result.is_symbolic = true;
            result.certainty = ParticipantCertainty::Conditional;
            return result;
          }
          const llvm::APInt &value = mask->getValue();
          const uint32_t effective_width = std::min<uint32_t>(m_warp_size, 32);
          const uint32_t width_mask =
              effective_width == 32 ? 0xffffffffu
                                    : ((uint32_t{1} << effective_width) - 1);
          result.has_lane_mask = true;
          result.lane_mask =
              static_cast<uint32_t>(value.zextOrTrunc(32).getZExtValue()) &
              width_mask;
          if (result.lane_mask == 0) {
            result.min_lane = 0;
            result.max_lane = 0;
          } else {
            result.min_lane =
                static_cast<uint32_t>(__builtin_ctz(result.lane_mask));
            result.max_lane =
                31u - static_cast<uint32_t>(__builtin_clz(result.lane_mask));
          }
          const bool full_mask = value.getBitWidth() >= m_warp_size &&
                                 value == llvm::APInt::getLowBitsSet(
                                              value.getBitWidth(), m_warp_size);
          result.is_exact = must_reach;
          result.certainty = !must_reach
                                 ? ParticipantCertainty::Conditional
                                 : (full_mask ? ParticipantCertainty::Exact
                                              : ParticipantCertainty::Partial);
          return result;
        }
        if (type == ThreadAPI::TD_CUDA_BARRIER) {
          result.scopes.push_back(static_cast<int>(ParticipationScope::Block));
          result.min_lane = 0;
          result.max_lane = m_warp_size - 1;
          result.is_exact = mustReachInstruction(m_kernel, inst);
          result.certainty = result.is_exact
                                 ? ParticipantCertainty::Exact
                                 : ParticipantCertainty::Conditional;
          return result;
        }
      }
    }
    result.scopes.push_back(static_cast<int>(ParticipationScope::Grid));
    result.is_exact = false;
    result.certainty = ParticipantCertainty::Conditional;
    return result;
  }

  for (const auto &op : inst->operands()) {
    if (!op) {
      continue;
    }
    auto uniformity = CUDASymbolicModel::classifyUniformity(op);
    auto scope = classifyByUniformity(uniformity);

    bool already_has_scope = false;
    for (int existing : result.scopes) {
      if (existing == static_cast<int>(scope)) {
        already_has_scope = true;
        break;
      }
    }
    if (!already_has_scope) {
      result.scopes.push_back(static_cast<int>(scope));
    }
  }

  if (result.scopes.empty()) {
    result.scopes.push_back(static_cast<int>(ParticipationScope::Grid));
  }

  llvm::DominatorTree dom_tree(const_cast<llvm::Function &>(m_kernel));
  uint32_t predicate_mask =
      m_warp_size >= 32 ? 0xffffffffu : ((uint32_t{1} << m_warp_size) - 1);
  bool found_predicate = false;
  for (const llvm::BasicBlock &block : m_kernel) {
    const auto *branch =
        llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
    if (!branch || !branch->isConditional() ||
        !dom_tree.dominates(branch, inst)) {
      continue;
    }
    const bool in_true =
        dom_tree.dominates(branch->getSuccessor(0), inst->getParent());
    const bool in_false =
        dom_tree.dominates(branch->getSuccessor(1), inst->getParent());
    if (in_true == in_false) {
      continue;
    }
    if (auto mask = getSimplePredicateMask(branch, in_true, m_warp_size)) {
      predicate_mask &= *mask;
      found_predicate = true;
    }
  }
  if (found_predicate) {
    result.has_lane_mask = true;
    result.lane_mask = predicate_mask;
    if (predicate_mask != 0) {
      result.min_lane = static_cast<uint32_t>(__builtin_ctz(predicate_mask));
      result.max_lane =
          31u - static_cast<uint32_t>(__builtin_clz(predicate_mask));
    }
  }

  // Operand uniformity does not describe the enclosing control predicate.
  // Ordinary instructions therefore remain conditional unless a future path
  // predicate analysis proves their active participant set.
  result.is_symbolic = true;
  result.is_exact = false;
  result.certainty = ParticipantCertainty::Conditional;

  return result;
}

CUDAParticipantSet
CUDAParticipantAnalysis::getActiveSetAt(const llvm::Instruction *inst) const {
  return getActiveParticipants(inst);
}

ParticipantRelation
CUDAParticipantAnalysis::computeRelation(const CUDAParticipantSet &lhs,
                                         const CUDAParticipantSet &rhs) const {
  return computeOverlap(lhs, rhs);
}

CUDAParticipantPredicate CUDAParticipantAnalysis::evaluatePredicate(
    const llvm::Instruction *lhs_inst,
    const llvm::Instruction *rhs_inst) const {
  CUDAParticipantPredicate pred;
  pred.lhs = getActiveParticipants(lhs_inst);
  pred.rhs = getActiveParticipants(rhs_inst);
  pred.relation = computeRelation(pred.lhs, pred.rhs);
  pred.confidence = 1.0;
  return pred;
}

bool CUDAParticipantAnalysis::areUniformWithinWarp(
    const llvm::Value *value) const {
  if (!value) {
    return false;
  }
  return CUDASymbolicModel::classifyUniformity(value) ==
         UniformityClass::WarpUniform;
}

bool CUDAParticipantAnalysis::areUniformWithinBlock(
    const llvm::Value *value) const {
  if (!value) {
    return false;
  }
  return CUDASymbolicModel::classifyUniformity(value) ==
         UniformityClass::BlockUniform;
}

bool CUDAParticipantAnalysis::variesPerLane(const llvm::Value *value) const {
  if (!value) {
    return false;
  }
  return CUDASymbolicModel::classifyUniformity(value) ==
         UniformityClass::ThreadVarying;
}

ParticipantRelation computeOverlap(const CUDAParticipantSet &a,
                                   const CUDAParticipantSet &b) {
  if (a.has_lane_mask && b.has_lane_mask) {
    const uint32_t overlap = a.lane_mask & b.lane_mask;
    if (overlap == 0) {
      return ParticipantRelation::Disjoint;
    }
    if (a.is_exact && b.is_exact && a.lane_mask == b.lane_mask &&
        a.min_block == b.min_block && a.max_block == b.max_block) {
      return ParticipantRelation::Equal;
    }
    return ParticipantRelation::Overlaps;
  }
  if (a.is_exact && b.is_exact) {
    if (a.min_lane == b.min_lane && a.max_lane == b.max_lane &&
        a.min_block == b.min_block && a.max_block == b.max_block) {
      return ParticipantRelation::Equal;
    }
  }

  bool a_in_block = (a.min_block <= b.max_block && a.max_block >= b.min_block);
  bool b_in_block = (b.min_block <= a.max_block && b.max_block >= a.min_block);

  if (a_in_block && b_in_block) {
    return ParticipantRelation::Overlaps;
  }

  return ParticipantRelation::Unknown;
}

} // namespace concurrency::cuda
