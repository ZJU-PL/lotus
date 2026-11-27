#include "Checker/FiTx/Core/Function.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Utils.h"

#include <deque>

namespace fitx {
std::shared_ptr<fitx::Function>
Function::createManagedFunction(llvm::Function *function,
                                std::unique_ptr<llvm::LoopInfo> loop_info) {
  auto it = created_functions_.find(function);
  if (it == created_functions_.end()) {
    auto fn = std::make_shared<fitx::Function>(function, std::move(loop_info));
    created_functions_[function] = fn;
    return fn;
  }

  if (!it->second->hasLoopInfo() && loop_info)
    it->second->setLoopInfo(std::move(loop_info));
  return it->second;
}

bool Function::IsDebugValueFunction(
    std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.dbg.value");
}

bool Function::IsDebugDeclareFunction(
    std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.dbg.declare");
}

bool Function::IsLifetimeEndFunction(
    std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.lifetime.end");
}

bool Function::IsExpectFunction(std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.expect");
}

bool Function::IsRefcountDecrementFunction(
    std::shared_ptr<fitx::Function> function) {
  return std::find(refcount_decrement_functions.begin(),
                   refcount_decrement_functions.end(),
                   function->Name()) != refcount_decrement_functions.end();
}

bool Function::IsMemSetFunction(std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "memset");
}

Function::Function(llvm::Function *function,
                   std::unique_ptr<llvm::LoopInfo> loop_info)
    : llvm_function_(function), return_type_(function->getReturnType()),
      loop_info_(std::move(loop_info)), function_name_(function->getName()),
      is_definition_(function->isDeclaration()),
      arg_size_(function->arg_size()), return_value_(nullptr),
      contains_loop_back_blocks_(false),
      protected_refcount_value_(nullptr) {}

std::shared_ptr<fitx::BasicBlock>
Function::getBasicBlock(llvm::BasicBlock *basic_block) {
  auto it = block_map_.find(basic_block);
  if (it != block_map_.end())
    return it->second;

  auto getOrCreateShell = [&](llvm::BasicBlock *bb) {
    auto found = block_map_.find(bb);
    if (found != block_map_.end())
      return std::make_pair(found->second, false);
    auto fb = std::make_shared<fitx::BasicBlock>(bb);
    block_map_.try_emplace(bb, fb);
    basic_blocks_.insert(fb);
    if (llvm_function_ && !llvm_function_->empty() &&
        bb == &llvm_function_->getEntryBlock())
      init_block_ = fb;
    return std::make_pair(fb, true);
  };

  auto root_pair = getOrCreateShell(basic_block);
  std::deque<llvm::BasicBlock *> worklist;
  if (root_pair.second)
    worklist.push_back(basic_block);

  while (!worklist.empty()) {
    llvm::BasicBlock *curr = worklist.front();
    worklist.pop_front();
    auto framework_block = block_map_[curr];

    llvm::Loop *loop =
        loop_info_.get() ? loop_info_->getLoopFor(curr) : nullptr;

    for (auto *succ_bb : llvm::successors(curr)) {
      auto sp = getOrCreateShell(succ_bb);
      if (sp.second)
        worklist.push_back(succ_bb);
      framework_block->addSuccessor(sp.first);

      if (sp.first->Line() <= framework_block->Line())
        setLoopBackBlock(true);
    }

    for (auto *pred_bb : llvm::predecessors(curr)) {
      if (loop && loop->getHeader() == curr &&
          loop->getBlocksSet().find(pred_bb) != loop->getBlocksSet().end())
        continue;

      auto pp = getOrCreateShell(pred_bb);
      if (pp.second)
        worklist.push_back(pred_bb);
      framework_block->addPredecessor(pp.first);
    }

    framework_block->collectPassthroughBlock();

    if (auto *branch_inst =
            llvm::dyn_cast_or_null<llvm::BranchInst>(curr->getTerminator())) {
      framework_block->setBranchInst(fitx::BranchInst::Create(branch_inst));
    } else if (auto *switch_inst = llvm::dyn_cast_or_null<llvm::SwitchInst>(
                   curr->getTerminator())) {
      framework_block->setBranchInst(fitx::BranchInst::Create(switch_inst));
    }
  }

  return root_pair.first;
}

bool Function::isLoopBlock(std::shared_ptr<fitx::BasicBlock> block) {
  return loop_info_ && loop_info_->getLoopFor(block->LLVMBasicBlock());
}

void Function::setReturnValue(std::shared_ptr<fitx::Value> value) {
  return_value_ = value;
}

std::shared_ptr<fitx::Value> Function::getReturnValue() {
  return return_value_;
}

void Function::setReturnBlock(std::shared_ptr<fitx::BasicBlock> block) {
  return_block_ = block;
}

void Function::addPossibleReturnValues(
    std::shared_ptr<fitx::Value> value,
    std::shared_ptr<fitx::BasicBlock> block) {
  return_assignment_[block] = value;
}

void Function::addCallerFunction(std::shared_ptr<fitx::Function> caller) {
  caller_functions_.insert(caller);
}

const std::set<std::shared_ptr<fitx::Function>> &
Function::CallerFunctions() {
  return caller_functions_;
}

void Function::addRefcountInstruction(
    std::shared_ptr<fitx::Instruction> instruction) {
  if (!last_refcount_call_ || last_refcount_call_ < instruction)
    last_refcount_call_ = instruction;
}

std::shared_ptr<fitx::Instruction> Function::lastRefcountInstruction() {
  return last_refcount_call_;
}

void Function::setLoopBackBlock(bool loop_back) {
  contains_loop_back_blocks_ = loop_back;
}

bool Function::ContainsLoopBackBlock() { return contains_loop_back_blocks_; }

std::map<llvm::Function *, std::shared_ptr<fitx::Function>>
    fitx::Function::created_functions_;

void Function::eraseModuleFunctions(const llvm::Module *M) {
  for (auto it = created_functions_.begin(); it != created_functions_.end();) {
    if (!M || (it->first && it->first->getParent() == M))
      it = created_functions_.erase(it);
    else
      ++it;
  }
}
} // namespace fitx
