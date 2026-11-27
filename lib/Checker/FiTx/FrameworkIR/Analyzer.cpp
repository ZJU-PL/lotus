// FiTx framework IR builder: converts llvm::Function to fitx::Function
// (basic blocks, ordered block list, call/store/load/ret instructions, return
// assignments). No typestate; that is done by Frontend::Analyzer (paper
// §4.2, 4.3).
#include "Checker/FiTx/FrameworkIR/Analyzer.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/FrameworkIR/Utils.h"

namespace ir_generator {
Analyzer::Analyzer() {}

// Build fitx::Function: create blocks in RPO, iterate instructions to build
// CallInst/StoreInst/LoadInst and collect return assignments; set ordered
// block list for Frontend CFG traversal (paper §4.2).
void Analyzer::analyze(llvm::Function &function, llvm::LoopInfo &loop_info) {
  framework_function_ = fitx::Function::createManagedFunction(
      &function, std::make_unique<llvm::LoopInfo>(std::move(loop_info)));

  std::vector<std::shared_ptr<fitx::BasicBlock>> block;
  llvm::SmallPtrSet<llvm::BasicBlock *, 32> ordered_set;
  if (!function.empty()) {
    llvm::ReversePostOrderTraversal<llvm::Function *> rpot(&function);
    for (llvm::BasicBlock *bb : rpot) {
      ordered_set.insert(bb);
      auto framework_block = framework_function_->getBasicBlock(bb);
      if (framework_block->Id() == fitx::BasicBlock::kNoId) {
        framework_block->setId(block.size());
        block.push_back(framework_block);
      }
    }
  }
  for (auto &basic_block : function) {
    if (!ordered_set.insert(&basic_block).second)
      continue;
    auto framework_block = framework_function_->getBasicBlock(&basic_block);
    if (framework_block->Id() == fitx::BasicBlock::kNoId) {
      framework_block->setId(block.size());
      block.push_back(framework_block);
    }
  }

  for (const auto &framework_block : block) {
    for (auto &instruction : *framework_block->LLVMBasicBlock()) {
      switch (instruction.getOpcode()) {
      case llvm::Instruction::Call:
        analyzeCallInst(&instruction);
        break;
      case llvm::Instruction::Store:
        analyzeStoreInst(&instruction);
        break;
      case llvm::Instruction::Load:
        analyzeLoadInst(&instruction);
        break;
      case llvm::Instruction::Ret:
        analyzeReturnInst(&instruction);
        break;
      default:
        break;
      }
    }
  }
  framework_function_->addOrderedBlock(block);
}

void Analyzer::analyzeCallInst(llvm::Instruction *instruction) {
  auto *call_inst = llvm::cast<llvm::CallInst>(instruction);
  if (llvm::Function *callee = call_inst->getCalledFunction()) {
    llvm::StringRef callee_name = callee->getName();
    // Skip lifetime.end and non-value/non-declare debug intrinsics before
    // constructing framework CallInst and converting all operands.
    if (callee_name.startswith("llvm.lifetime.end") ||
        (callee_name.startswith("llvm.dbg.") &&
         !callee_name.startswith("llvm.dbg.declare") &&
         !callee_name.startswith("llvm.dbg.value")))
      return;
  }
  auto framework_block =
      framework_function_->getBasicBlock(instruction->getParent());
  auto framework_call = fitx::CallInst::Create(call_inst);

  if (framework_call->CalledFunction()) {
    if (fitx::Function::IsDebugValueFunction(
            framework_call->CalledFunction())) {
      auto return_block = framework_function_->ReturnBlock();
      const auto &args = framework_call->Arguments();
      if (return_block && !args.empty() && args[0]) {
        return_block->addDeadValue(args[0]);
      }
    }

    if ((framework_call->CalledFunction()->isDebugFunction() &&
         !fitx::Function::IsDebugDeclareFunction(
             framework_call->CalledFunction())) ||
        fitx::Function::IsLifetimeEndFunction(
            framework_call->CalledFunction()))
      return;

    if (fitx::Function::IsRefcountDecrementFunction(
            framework_call->CalledFunction()) &&
        !framework_call->Arguments().empty()) {
      framework_function_->setProtectedRefcountValue(
          framework_call->Arguments()[0]);
    }

    if (framework_call->CalledFunction()->ProtectedRefcountValue()) {
      framework_function_->addRefcountInstruction(framework_call);
    }
  }

  framework_block->addInstruction(framework_call);
}

void Analyzer::analyzeDebugCallInst(llvm::CallInst *call_inst) {}

void Analyzer::analyzeStoreInst(llvm::Instruction *inst) {
  auto *store_inst = llvm::cast<llvm::StoreInst>(inst);
  auto framework_block = framework_function_->getBasicBlock(inst->getParent());

  // Paper §3.1.1: omit simple arg→alloca copy from state flow (FiT analysis).
  if (llvm::isa<llvm::Argument>(store_inst->getValueOperand()) &&
      llvm::isa<llvm::AllocaInst>(store_inst->getPointerOperand()))
    return;

  auto created_store = fitx::StoreInst::Create(store_inst);
  framework_block->addInstruction(created_store);
  if (auto call_inst = fitx::shared_dyn_cast<fitx::CallInst>(
          created_store->ValueOperand())) {
    auto branch_inst = framework_block->getBranchInst();
    if (!branch_inst || !branch_inst->Condition())
      return;

    if (!call_inst->CalledFunction() ||
        !call_inst->CalledFunction()->ReturnType()->isIntegerTy())
      return;

    if (auto compare_inst = fitx::shared_dyn_cast<fitx::CompareInst>(
            branch_inst->Condition())) {
      compare_inst->replaceOperand(created_store->PointerOperand(),
                                   created_store->ValueOperand());
    }
  }
}

void Analyzer::analyzeLoadInst(llvm::Instruction *inst) {
  auto *load_inst = llvm::cast<llvm::LoadInst>(inst);

  // Check whether this load is filtered out before allocating/converting a
  // framework LoadInst (the majority of loads feed calls, comparisons, stores,
  // or returns and are omitted from framework IR).
  llvm::SmallVector<llvm::User *, 8> users(inst->user_begin(),
                                           inst->user_end());
  llvm::SmallPtrSet<llvm::User *, 16> visited;
  for (size_t idx = 0; idx < users.size(); ++idx) {
    auto *user = users[idx];
    if (!visited.insert(user).second)
      continue;
    if (llvm::isa<llvm::CallInst>(user) || llvm::isa<llvm::CmpInst>(user) ||
        llvm::isa<llvm::ReturnInst>(user))
      return;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
      if (store->getPointerOperand() == inst)
        return;
    }

    if (auto *unary = llvm::dyn_cast<llvm::UnaryInstruction>(user))
      users.append(unary->user_begin(), unary->user_end());
  }

  auto framework_block = framework_function_->getBasicBlock(inst->getParent());
  auto framework_load = fitx::LoadInst::Create(load_inst);
  framework_block->addInstruction(framework_load);
}

void Analyzer::analyzeReturnInst(llvm::Instruction *instruction) {
  auto *return_inst = llvm::cast<llvm::ReturnInst>(instruction);
  auto *return_value = return_inst->getReturnValue();
  framework_function_->setReturnBlock(
      framework_function_->getBasicBlock(return_inst->getParent()));

  if (!return_value || !framework_function_->ReturnType()->isIntOrPtrTy())
    return;

  auto framework_value = fitx::Value::CreateFromDefinition(return_value);
  framework_function_->setReturnValue(framework_value);

  llvm::SmallPtrSet<llvm::Instruction *, 16> visited_inst;
  visited_inst.insert(return_inst);
  collectPossibleReturnValues(return_value, visited_inst, return_inst);
}

bool Analyzer::collectPossibleReturnValues(
    llvm::Value *v, llvm::SmallPtrSetImpl<llvm::Instruction *> &visited_inst,
    llvm::Instruction *last_inst) {
  auto framework_value = fitx::Value::CreateFromDefinition(v);
  bool add_return_value = false;

  if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(v)) {
    if (!visited_inst.insert(instruction).second)
      return true;
    last_inst = instruction;
  }

  framework_value->setReturnValue(true);
  if (llvm::isa<llvm::Constant>(v)) {
    if (last_inst) {
      auto current_block =
          framework_function_->getBasicBlock(last_inst->getParent());
      framework_function_->addPossibleReturnValues(framework_value,
                                                   current_block);
      add_return_value = true;
    }
    return add_return_value;
  }

  if (auto *call_inst = llvm::dyn_cast<llvm::CallInst>(v)) {
    auto current_block =
        framework_function_->getBasicBlock(call_inst->getParent());
    framework_function_->addPossibleReturnValues(framework_value,
                                                 current_block);
    add_return_value = true;
  } else if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(v)) {
    add_return_value = collectPossibleReturnValues(LI->getPointerOperand(),
                                                   visited_inst, last_inst);
  } else if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(v)) {
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); i++) {
      llvm::Value *incoming = PHI->getIncomingValue(i);
      if (incoming != v)
        add_return_value =
            collectPossibleReturnValues(incoming, visited_inst, last_inst) ||
            add_return_value;
    }
  } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(v)) {
    if (v != SI->getValueOperand()) {
      add_return_value = collectPossibleReturnValues(SI->getValueOperand(),
                                                     visited_inst, last_inst);
      if (!add_return_value) {
        auto current_block =
            framework_function_->getBasicBlock(SI->getParent());
        framework_function_->addPossibleReturnValues(framework_value,
                                                     current_block);
        add_return_value = true;
      }
    }
  } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v)) {
    for (auto *usr : AI->users()) {
      add_return_value =
          collectPossibleReturnValues(usr, visited_inst, last_inst) ||
          add_return_value;
    }
  } else if (auto *UnaryInst = llvm::dyn_cast<llvm::UnaryInstruction>(v)) {
    add_return_value = collectPossibleReturnValues(UnaryInst->getOperand(0),
                                                   visited_inst, last_inst);
  } else if (auto *GEI = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
    auto current_block = framework_function_->getBasicBlock(GEI->getParent());
    framework_function_->addPossibleReturnValues(framework_value,
                                                 current_block);
  }
  return add_return_value;
}

} // namespace ir_generator
