#include "Checker/FiTx/Frontend/Function.h"

#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/BasicBlock.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace fitx {
FunctionInformation::FunctionInformation(
    std::shared_ptr<fitx::Function> function, AnalysisStat stat)
    : framework_function_(std::move(function)), stat_(stat) {}

void FunctionInformation::setAnalysisStat(
    FunctionInformation::AnalysisStat stat) {
  stat_ = stat;
}

void FunctionInformation::setAnayzingBasicBlock(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  current_basicblock_ = std::move(basic_block);
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::getCurrentBasicBlockInformation() {
  return basic_block_info_[current_basicblock_];
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::createBasicBlockInfo(
    std::shared_ptr<fitx::BasicBlock> basic_block,
    const std::set<State> &states) {
  int time_to_live = BasicBlockInformation::kMaxTimeToLive;

  auto existing_it = basic_block_info_.find(basic_block);
  if (existing_it != basic_block_info_.end()) {
    prev_basic_block_info_[basic_block] = existing_it->second;
    time_to_live = existing_it->second->TimeToLive() - 1;
  }

  auto current_block_info =
      std::make_shared<BasicBlockInformation>(basic_block, states);
  basic_block_info_[basic_block] = current_block_info;
  current_block_info->setTimeToLive(time_to_live);

  if (basic_block->isCleanupBlock())
    return current_block_info;

  bool return_value_assigned = !current_block_info->ReturnValues().empty();
  auto mergeStates = [&](const BasicBlockValueStates &pred_val_states,
                         const ArgValueStates &pred_arg_states) {
    current_block_info->ValueStates().mergeFrom(pred_val_states);
    if (!pred_arg_states.empty())
      current_block_info->getArgValueStates().addArgValueState(pred_arg_states);
  };

  for (const auto &pred_reference : basic_block->Predecessors()) {
    auto preds = pred_reference.lock();
    if (!preds)
      continue;
    if (!basicBlockInfoExists(preds)) {
      current_block_info->setPartialStates(true);
      continue;
    }

    const auto &weak_blocks = preds->getPassthroughBlock(basic_block);
    std::vector<std::shared_ptr<fitx::BasicBlock>> passthrough_blocks;
    if (!weak_blocks.empty()) {
      passthrough_blocks.reserve(weak_blocks.size());
      for (const auto &wb : weak_blocks) {
        if (auto locked = wb.lock())
          passthrough_blocks.push_back(std::move(locked));
      }
    }
    if (passthrough_blocks.empty())
      passthrough_blocks.push_back(preds);

    for (const auto &block : passthrough_blocks) {
      auto pred_it = basic_block_info_.find(block);
      if (pred_it == basic_block_info_.end()) {
        current_block_info->setPartialStates(true);
        continue;
      }

      const auto &pred_block_info = pred_it->second;
      if (pred_block_info->PartialStates()) {
        if (block->Line() < basic_block->Line() ||
            pred_block_info->TimeToLive() > 0)
          current_block_info->setPartialStates(true);
        continue;
      }

      BasicBlockInformation::BlockStatus status =
          pred_block_info->getBlockStatus();
      const auto &branch_inst = pred_block_info->BasicBlock()->getBranchInst();
      if (branch_inst && branch_inst->Condition() &&
          branch_inst->returnValueOperandExists()) {
        status = BasicBlockInformation::ERROR;
        if (auto condition =
                shared_dyn_cast<CompareInst>(branch_inst->Condition())) {
          switch (condition->GetPredicate()) {
          case llvm::CmpInst::Predicate::ICMP_EQ:
          case llvm::CmpInst::Predicate::ICMP_SGT:
            status = BasicBlockInformation::SUCCESS;
            break;
          case llvm::CmpInst::Predicate::ICMP_NE:
          case llvm::CmpInst::Predicate::ICMP_SLT:
            status = BasicBlockInformation::ERROR;
            break;
          default:
            break;
          }
        }
      }
      current_block_info->setBlockStatus(status);

      // Zero-copy fast path when predecessor has no pending arg states for
      // `basic_block` (the common case even when return codes are pending).
      if (!pred_block_info->hasPendingArgStatesFor(basic_block)) {
        mergeStates(pred_block_info->ValueStates(),
                    pred_block_info->getArgValueStates());
      } else {
        auto pred_value_states =
            pred_block_info->ValueStatesForSuccessor(basic_block);
        mergeStates(pred_value_states.first, pred_value_states.second);
      }

      if (!return_value_assigned) {
        if (!pred_block_info->hasPendingReturnValuesFor(basic_block)) {
          current_block_info->addReturnValues(pred_block_info->ReturnValues());
        } else {
          current_block_info->addReturnValues(
              pred_block_info->ReturnCodeForSuccessor(basic_block));
        }
      }
    }
  }

  if (current_block_info->getBlockStatus() == BasicBlockInformation::NONE)
    current_block_info->setBlockStatus(BasicBlockInformation::NUTRAL);

  return current_block_info;
}

void FunctionInformation::addValue(std::shared_ptr<Value> value) {
  value_collection_.add(std::move(value));
}

void FunctionInformation::addValues(const ValueCollection &collection) {
  value_collection_.add(collection);
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::getBasicBlockInformation(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  auto it = basic_block_info_.find(basic_block);
  if (it != basic_block_info_.end())
    return it->second;
  return nullptr;
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::getBasicBlockPrevInformation(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  auto it = prev_basic_block_info_.find(basic_block);
  if (it != prev_basic_block_info_.end())
    return it->second;
  return nullptr;
}

bool FunctionInformation::basicBlockInfoExists(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  return basic_block_info_.find(basic_block) != basic_block_info_.end();
}

bool FunctionInformation::basicBlockPrevInfoExists(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  return prev_basic_block_info_.find(basic_block) !=
         prev_basic_block_info_.end();
}

const FunctionInformation::WeakBasicBlockSet &
FunctionInformation::getErrorBlocks(int64_t error_code) {
  return return_info_[error_code];
}

const FunctionInformation::WeakBasicBlockSet &
FunctionInformation::getSuccessBlock() {
  return return_info_[kSuccessCode];
}

bool FunctionInformation::basicBlockInfoChanged(
    std::shared_ptr<fitx::BasicBlock> block) {
  auto current_info = getBasicBlockInformation(block);
  if (!current_info)
    return true;

  // Enforce TTL before PartialStates() so irreducible loops or unreachable
  // predecessors can never cause an infinite worklist loop.
  if (current_info->TimeToLive() <= 0)
    return false;

  if (current_info->PartialStates())
    return true;

  auto prev_info = getBasicBlockPrevInformation(block);
  if (!prev_info)
    return true;

  if (*current_info == *prev_info)
    return false;
  return true;
}

void FunctionInformation::addReturnValueInfo(
    int64_t value, std::weak_ptr<fitx::BasicBlock> block) {
  return_info_[value].insert(std::move(block));
}

void FunctionInformation::addReturnValueInfo(int64_t value,
                                             WeakBasicBlockSet block_info) {
  auto it = return_info_.find(value);
  if (it == return_info_.end()) {
    return_info_.emplace(value, std::move(block_info));
    return;
  }
  it->second.insert(block_info.begin(), block_info.end());
}

bool FunctionInformation::existsInRefcountFunctions(
    std::shared_ptr<fitx::Function> function) {
  return std::find(called_refcount_functions_.begin(),
                   called_refcount_functions_.end(),
                   function) != called_refcount_functions_.end();
}

void FunctionInformation::addRefcountFunction(
    std::shared_ptr<fitx::Function> function) {
  called_refcount_functions_.push_back(std::move(function));
}

void FunctionInformation::releaseAnalysisMemory(bool keep_summary) {
  prev_basic_block_info_.clear();
  current_basicblock_.reset();
  alias_info_.clear();

  if (!keep_summary) {
    basic_block_info_.clear();
    value_collection_.clear();
    return;
  }

  std::set<std::shared_ptr<fitx::BasicBlock>> needed_blocks;
  for (const auto &entry : return_info_) {
    for (const auto &wb : entry.second) {
      if (auto locked = wb.lock())
        needed_blocks.insert(std::move(locked));
    }
  }

  for (auto it = basic_block_info_.begin(); it != basic_block_info_.end();) {
    if (needed_blocks.find(it->first) == needed_blocks.end()) {
      it = basic_block_info_.erase(it);
    } else {
      it->second->clearNonSummaryData();
      ++it;
    }
  }
}

}; // namespace fitx
