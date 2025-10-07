

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include "LLVMUtils/Statistics.h"

// Analyzes and prints statistics about the module's instructions.
void Statistics::run(Module &M) {
    unsigned NumInstructions = 0;
    unsigned NumPointerInstructions = 0;
    unsigned NumDerefInstructions = 0;
    for (auto &F: M) {
        if (F.empty()) continue;
        for (auto &I: instructions(F)) {
            if (I.isDebugOrPseudoInst()) continue;
            ++NumInstructions;
            for (unsigned K = 0; K < I.getNumOperands(); ++K) {
                if (I.getOperand(K)->getType()->isPointerTy()) {
                    ++NumPointerInstructions;
                    break;
                }
            }

            if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<AtomicCmpXchgInst>(I)
                || isa<AtomicRMWInst>(I) || isa<ExtractValueInst>(I) || isa<InsertValueInst>(I)) {
                ++NumDerefInstructions;
            } else if (auto *CI = dyn_cast<CallInst>(&I)) {
                if (auto *Callee = CI->getCalledFunction()) {
                    if (Callee->empty()) {
#if defined(LLVM12)
                        for (unsigned K = 0; K < CI->getNumArgOperands(); ++K) {
#elif defined(LLVM14)
                        // refer to llvm-12/include/llvm/IR/InstrTypes.h:1321
                        for (unsigned K = 0; K < CI->arg_size(); ++K) {
#else
    #error "Unsupported LLVM version"
#endif
                            if (CI->getArgOperand(K)->getType()->isPointerTy()) {
                                ++NumDerefInstructions;
                                break;
                            }
                        }
                    }
                } else {
                    ++NumDerefInstructions;
                }
            }
        }
    }
    outs() << "# total instructions: " << NumInstructions << ", "
           << "# ptr instructions: " << NumPointerInstructions << ", "
           << "# deref instructions: " << NumDerefInstructions << ".\n";
}

