#include "Analysis/NullPointer/CastFailureAnalysis.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"

#include <llvm/IR/Constants.h>

namespace lotus {
using namespace llvm;

using namespace std;

#define DEBUG_TYPE "CastFailureAnalysis"

CastFailureAnalysis::CastFailureAnalysis() = default;
CastFailureAnalysis::~CastFailureAnalysis() = default;

bool CastFailureAnalysis::analyze(
    Module &M, DebugInfoAnalysis &debug,
    ir_expression::IRExpressionRenderer &renderer) {
  DIA = &debug;
  cast_fail_phi_oprds.clear();
  inst_resolver = &renderer;

  // Calculate all the cast failure PHI operands
  for (auto &F : M) {
    for (BasicBlock &B : F) {
      for (Instruction &inst : B) {
        PHINode *PN = dyn_cast<PHINode>(&inst);
        if (!PN)
          break;

        for (int i = PN->getNumIncomingValues() - 1; i > -1; --i) {
          if (isCastFailNullForPhi(PN, i))
            cast_fail_phi_oprds.insert(make_pair(PN, i));
        }
      }
    }
  }

  return false;
}

bool CastFailureAnalysis::isCastFailNullForPhi(const PHINode *phi_node,
                                               int kth) {
  Value *k_arg = phi_node->getIncomingValue(kth);
  if (!isa<ConstantPointerNull>(k_arg))
    return false;

  LLVM_DEBUG(dbgs() << "Check for cast failure null: " << *phi_node
                    << ", with arg = " << kth << "\n");

  BasicBlock *BB = phi_node->getIncomingBlock(kth);
  BranchInst *br_inst = dyn_cast<BranchInst>(BB->getTerminator());
  if (!br_inst || !br_inst->isConditional())
    return false;

  // First, nullptr takes the True branch to flow to the phi node
  BasicBlock *true_bb = br_inst->getSuccessor(0);
  if (true_bb != phi_node->getParent())
    return false;

  LLVM_DEBUG(dbgs() << "\t checkpoint 1\n");

  // Get the pointer that's used for nullness checking
  Value *ptr_before_cast = nullptr;

  Value *br_cond = br_inst->getCondition();
  ICmpInst *cmp_inst = dyn_cast<ICmpInst>(br_cond);

  if (!cmp_inst || !cmp_inst->isEquality())
    return false;

  Value *op1 = cmp_inst->getOperand(0);
  Value *op2 = cmp_inst->getOperand(1);

  if (isa<ConstantPointerNull>(op1) && op2->getType()->isPointerTy()) {
    // icmp null, p
    ptr_before_cast = op2;
  } else if (isa<ConstantPointerNull>(op2) && op1->getType()->isPointerTy()) {
    // icmp p, null
    ptr_before_cast = op1;
  }

  if (ptr_before_cast == nullptr)
    return false;

  LLVM_DEBUG(dbgs() << "\t checkpoint 2\n");

  BasicBlock *false_bb = br_inst->getSuccessor(1);

  // Check name as a heuristic
  if (false_bb->hasName() && !false_bb->getName().startswith("cast.notnull"))
    return false;

  if (phi_node->getBasicBlockIndex(false_bb) < 0) {
    return false;
  }

  // Check if the checked non-null pointer eventually goes to the PhiNode
  Value *ptr_cast_final = phi_node->getIncomingValueForBlock(false_bb);

  auto ptr_origin_pair =
      inst_resolver->track_pointer_offset(ptr_cast_final, true);
  if (ptr_origin_pair.first == ptr_before_cast) {
    // The pointer must be used for type cast in the cast.notnull BB
    Value *ptr_after_cast = nullptr;
    for (auto it = ptr_before_cast->use_begin(),
              ie = ptr_before_cast->use_end();
         it != ie; ++it) {
      User *user = it->getUser();
      CastInst *cast_inst = dyn_cast<CastInst>(user);
      if (cast_inst == nullptr)
        continue;

      if (cast_inst->getParent() == false_bb) {
        ptr_after_cast = cast_inst;
        break;
      }
    }

    if (!ptr_after_cast)
      return false;
  }

  LLVM_DEBUG(dbgs() << "\t checkpoint 3\n");

  // Last check: see if the key instructions are on the same source line
  if (DIA->getSourceLine(phi_node) != 0) {
    int line_num = DIA->getSourceLine(phi_node);
    LLVM_DEBUG(dbgs() << "\t PHI line number: " << line_num << "\n");

    if (line_num != DIA->getSourceLine(br_inst) ||
        line_num != DIA->getSourceLine(cmp_inst) ||
        line_num != DIA->getSourceLine(ptr_cast_final))
      return false;
  }

  LLVM_DEBUG(dbgs() << "\t checkpoint 4\n");

  return true;
}

bool CastFailureAnalysis::phiHasCastFailNullArg(const PHINode *phi_node) {
  if (cast_fail_phi_oprds.find(phi_node) == cast_fail_phi_oprds.end())
    return false;

  return true;
}

bool CastFailureAnalysis::isCastNullCheckBranch(const Instruction *term_inst) {
  if (!term_inst)
    return false;

  if (const BranchInst *br_inst = dyn_cast<const BranchInst>(term_inst)) {
    if (!br_inst->isConditional())
      return false;

    const BasicBlock *br_bb = br_inst->getParent();
    const BasicBlock *true_bb = br_inst->getSuccessor(0);
    for (auto &ins_it : *true_bb) {
      const PHINode *PN = dyn_cast<const PHINode>(&ins_it);
      if (!PN)
        break;

      // Check if there's a null operand coming from the given branch
      auto phi_it = cast_fail_phi_oprds.find(PN);
      while (phi_it != cast_fail_phi_oprds.end() && phi_it->first == PN) {
        if (PN->getIncomingBlock(phi_it->second) == br_bb) {
          return true;
        }
        phi_it++;
      }
    }
  }

  return false;
}
} // namespace lotus
