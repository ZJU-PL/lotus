#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Query/QuerySupport.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

using namespace llvm;

namespace pdg::query_detail {

static const Function *getCalledFunctionIgnoringCasts(const CallBase &call) {
  const Value *callee = call.getCalledOperand();
  return callee ? dyn_cast<Function>(callee->stripPointerCasts()) : nullptr;
}

static bool moduleHasAnyCall(const Module &module,
                             const std::vector<std::string> &names) {
  const std::vector<std::string> lowered_names = [&names]() {
    std::vector<std::string> result;
    for (size_t i = 0; i < names.size(); ++i)
      result.push_back(toLower(names[i]));
    return result;
  }();

  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    for (const BasicBlock &block : function) {
      for (const Instruction &inst : block) {
        const CallBase *call = dyn_cast<CallBase>(&inst);
        const Function *callee_function =
            call ? getCalledFunctionIgnoringCasts(*call) : nullptr;
        if (callee_function == nullptr)
          continue;
        const std::string callee = toLower(callee_function->getName().str());
        if (std::find(lowered_names.begin(), lowered_names.end(), callee) !=
            lowered_names.end())
          return true;
      }
    }
  }
  return false;
}

static bool isCallToAny(const Instruction &inst,
                        const std::vector<std::string> &names) {
  const CallBase *call = dyn_cast<CallBase>(&inst);
  const Function *callee_function =
      call ? getCalledFunctionIgnoringCasts(*call) : nullptr;
  if (callee_function == nullptr)
    return false;
  const std::string callee = toLower(callee_function->getName().str());
  for (size_t i = 0; i < names.size(); ++i) {
    if (callee == toLower(names[i]))
      return true;
  }
  return false;
}

static bool isSignedArithInstruction(const Instruction &inst) {
  switch (inst.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::SDiv:
  case Instruction::SRem:
  case Instruction::Shl:
    return inst.getType()->isIntegerTy();
  default:
    return false;
  }
}

static bool isNullDerefInstruction(const Instruction &inst) {
  if (isa<LoadInst>(&inst) || isa<StoreInst>(&inst))
    return true;
  if (const CallBase *call = dyn_cast<CallBase>(&inst)) {
    for (unsigned i = 0; i < call->arg_size(); ++i) {
      if (call->getArgOperand(i)->getType()->isPointerTy())
        return true;
    }
  }
  return false;
}

static bool isMemSafetyInstruction(const Instruction &inst) {
  return isa<LoadInst>(&inst) || isa<StoreInst>(&inst) ||
         isa<MemIntrinsic>(&inst) || isa<AllocaInst>(&inst) ||
         isa<CallBase>(&inst);
}

static bool isDefBehaviorInstruction(const Instruction &inst) {
  return isSignedArithInstruction(inst) || isa<LoadInst>(&inst) ||
         isa<StoreInst>(&inst) || isa<CallBase>(&inst);
}

NodeSet resolvePropertyCriteria(ProgramGraph &pdg, const Module &module,
                                const PropertySpec &spec) {
  NodeSet criteria;

  auto addInstructionNode = [&](const Instruction &inst) {
    Node *node = pdg.getNode(const_cast<Instruction &>(inst));
    if (node != nullptr)
      criteria.insert(node);
  };

  for (size_t rule_index = 0; rule_index < spec.rules().size(); ++rule_index) {
    const PropertyRule &rule = spec.rules()[rule_index];
    if (rule.kind == PropertyKind::UnreachCall ||
        rule.kind == PropertyKind::Assertions ||
        rule.kind == PropertyKind::CoverageErrorCall) {
      std::vector<std::string> targets;
      if (rule.kind == PropertyKind::Assertions) {
        targets.push_back("__assert_fail");
        targets.push_back("__verifier_error");
      } else if (rule.target.empty()) {
        targets.push_back("reach_error");
        if (rule.kind == PropertyKind::UnreachCall)
          targets.push_back("__verifier_error");
      } else {
        const std::string target = toLower(rule.target);
        targets.push_back(target);
        if (target == "reach_error")
          targets.push_back("__verifier_error");
      }

      for (const Function &function : module) {
        if (function.isDeclaration())
          continue;
        for (const BasicBlock &block : function) {
          for (const Instruction &inst : block) {
            if (isCallToAny(inst, targets))
              addInstructionNode(inst);
          }
        }
      }
      continue;
    }

    const bool has_mem_markers =
        (rule.kind == PropertyKind::MemSafety ||
         rule.kind == PropertyKind::MemCleanup) &&
        moduleHasAnyCall(module,
                         std::vector<std::string>{
                             "__INSTR_mark_pointer", "__INSTR_mark_free",
                             "__INSTR_mark_allocation", "__INSTR_mark_exit"});

    const bool has_overflow_markers =
        (rule.kind == PropertyKind::NoOverflow ||
         rule.kind == PropertyKind::DefBehavior) &&
        moduleHasAnyCall(
            module, std::vector<std::string>{"__VERIFIER_error",
                                             "__symbiotic_check_overflow"});

    const bool has_null_markers =
        rule.kind == PropertyKind::NullDeref &&
        moduleHasAnyCall(module,
                         std::vector<std::string>{"__INSTR_mark_pointer"});

    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      for (const BasicBlock &block : function) {
        for (const Instruction &inst : block) {
          switch (rule.kind) {
          case PropertyKind::MemSafety:
          case PropertyKind::MemCleanup:
            if (has_mem_markers) {
              if (isCallToAny(
                      inst, std::vector<std::string>{"__INSTR_mark_pointer",
                                                     "__INSTR_mark_free",
                                                     "__INSTR_mark_allocation",
                                                     "__INSTR_mark_exit"}))
                addInstructionNode(inst);
            } else if (isMemSafetyInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          case PropertyKind::NoOverflow:
            if (has_overflow_markers) {
              if (isCallToAny(inst, std::vector<std::string>{
                                        "__VERIFIER_error",
                                        "__symbiotic_check_overflow"}))
                addInstructionNode(inst);
            } else if (isSignedArithInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          case PropertyKind::DefBehavior:
            if (has_overflow_markers) {
              if (isCallToAny(inst, std::vector<std::string>{
                                        "__VERIFIER_error",
                                        "__symbiotic_check_overflow"}))
                addInstructionNode(inst);
            } else if (isDefBehaviorInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          case PropertyKind::Termination:
            if (isCallToAny(inst,
                            std::vector<std::string>{
                                "__INSTR_fail", "__assert_fail",
                                "__VERIFIER_silent_exit", "__VERIFIER_exit",
                                "__INSTR_check_assume"}))
              addInstructionNode(inst);
            break;
          case PropertyKind::NullDeref:
            if (has_null_markers) {
              if (isCallToAny(
                      inst, std::vector<std::string>{"__INSTR_mark_pointer"})) {
                if (const Instruction *next = inst.getNextNode())
                  addInstructionNode(*next);
                else
                  addInstructionNode(inst);
              }
            } else if (isNullDerefInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          default:
            break;
          }
        }
      }
    }
  }

  return criteria;
}

} // namespace pdg::query_detail
