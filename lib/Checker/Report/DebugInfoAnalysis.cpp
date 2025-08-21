// Use debug info of LLVM to better report bugs, e.g., line number, position, function name, variable name, etc.

#include "Checker/Report/DebugInfoAnalysis.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

DebugInfoAnalysis::DebugInfoAnalysis() = default;

std::string DebugInfoAnalysis::getSourceLocation(const Instruction *I) {
    if (!I) return "unknown:0";

    const DebugLoc &DL = I->getDebugLoc();
    if (!DL) return "unknown:0";

    unsigned Line = DL.getLine();
    unsigned Col = DL.getCol();

    if (auto *Scope = DL.getScope()) {
        if (isa<DIScope>(Scope)) {
            if (auto *File = cast<DIScope>(Scope)->getFile()) {
                return File->getFilename().str() + ":" + std::to_string(Line) +
                       ":" + std::to_string(Col);
            }
        }
    }

    return "unknown:" + std::to_string(Line);
}

std::string DebugInfoAnalysis::getFunctionName(const Instruction *I) {
    if (!I) return "unknown_function";

    const Function *F = I->getFunction();
    if (!F) return "unknown_function";

    // Try to get name from debug info first
    if (DISubprogram *Subprogram = F->getSubprogram()) {
        return Subprogram->getName().str();
    }

    return F->getName().str();
}

std::string DebugInfoAnalysis::getVariableName(const Value *V) {
    if (!V) return "unknown_var";

    // Try to get debug info for local variables
    if (auto *I = dyn_cast<Instruction>(V)) {
        // Look for local variable debug info
        for (const auto &BB : *I->getFunction()) {
            for (const auto &Inst : BB) {
                if (auto *DbgVar = dyn_cast<DbgVariableIntrinsic>(&Inst)) {
                    if (DbgVar->getVariableLocationOp(0) == V) {
                        return DbgVar->getVariable()->getName().str();
                    }
                }
            }
        }
    }

    // Fallback to LLVM IR name
    if (V->hasName()) {
        return V->getName().str();
    }

    return "temp_var";
}

std::string DebugInfoAnalysis::getTypeName(const Value *V) {
    if (!V) return "unknown_type";

    Type *Ty = V->getType();
    if (!Ty) return "unknown_type";

    std::string TypeStr;
    raw_string_ostream RSO(TypeStr);
    Ty->print(RSO);
    return TypeStr;
}

void DebugInfoAnalysis::printBugReport(const Instruction *BugInst,
                                     const std::string &BugType,
                                     const Value *RelatedValue) {
    printf("[BUG REPORT] %s\n", BugType.c_str());
    printf("  Location: %s\n", getSourceLocation(BugInst).c_str());
    printf("  Function: %s\n", getFunctionName(BugInst).c_str());
    if (RelatedValue) {
        printf("  Variable: %s\n", getVariableName(RelatedValue).c_str());
        printf("  Type: %s\n", getTypeName(RelatedValue).c_str());
    }
    printf("\n");
}

