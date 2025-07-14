/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: Alias analysis adapter of Canary
 */

#include "Dataflow/NullPointer/AliasAnalysisAdapter.h"
#include <llvm/IR/Value.h>
#include <llvm/IR/Constants.h>
#include <llvm/Pass.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;

static cl::opt<unsigned> DyckAAOpt("dyck-aa", cl::init(0), cl::Hidden,
                        cl::desc("Adapt DyckAA. (0: None, 1: DyckAA, 2: CFL-Dyck-AA)"));

static cl::opt<unsigned> CFLAAOpt("cfl-aa", cl::init(0), cl::Hidden,
                        cl::desc("Adapt CFLAA. (0: None, 1: Steensgaard, 2: Andersen)"));

// Note: We'll remove our own SimpleCaptureInfo and use LLVM's version instead

// Implementation of DyckAAAdapter
// Implementation of interface defined in AliasAnalysisAdapter.h
DyckAAAdapter::DyckAAAdapter(Function *F) : Fn(F) {
    // Initialize DyckAA-related member variables
}

DyckAAAdapter::~DyckAAAdapter() {
    // Clean up DyckAA-related resources
}

bool DyckAAAdapter::mayAlias(Value *V1, Value *V2, Instruction *InstPoint, 
                             bool IncludeI) {
    // Simple implementation just returns true (may alias)
    // A more sophisticated implementation would use DyckAA
    return true;
}

bool DyckAAAdapter::mayNull(Value *V, Instruction *InstPoint, bool BeforeInstruction) {
    // Simple implementation just returns true (may be null)
    // A more sophisticated implementation would use DyckAA
    return true;
}

// Implementation of CFLAAAdapter
CFLAAAdapter::CFLAAAdapter(Module *M, llvm::CFLSteensAAResult *SteensAA, 
                          llvm::CFLAndersAAResult *AndersAA, bool UseSteens) 
    : ModuleRef(M), SteensAAResult(SteensAA), AndersAAResult(AndersAA), 
      UseSteensgaard(UseSteens) {
    // Initialize CFLAA-related member variables
}

CFLAAAdapter::~CFLAAAdapter() {
    // Clean up CFLAA-related resources
}

bool CFLAAAdapter::mayAlias(Value *V1, Value *V2, Instruction *InstPoint, 
                           bool IncludeI) {
    // Since we can't access CFLSteensAAResult and CFLAndersAAResult directly,
    // we'll provide a simpler implementation that's always conservative
    
    // For now, we'll simply return true (may alias) for all pointer pairs
    // A more complete implementation would require linking against actual CFLAA
    return true;
}

bool CFLAAAdapter::mayNull(Value *V, Instruction *InstPoint, bool BeforeInstruction) {
    // This is a conservative implementation since CFLAA doesn't directly provide
    // nullness information. A more sophisticated implementation would use the
    // alias information to infer potential nullness.
    
    // For ConstantPointerNull, we know it's null
    if (isa<llvm::ConstantPointerNull>(V)) {
        return true;
    }
    
    // For GlobalValue, function, or other known non-null values
    if (isa<llvm::GlobalValue>(V) || isa<llvm::Function>(V)) {
        return false;
    }
    
    // For other values, we conservatively return true (may be null)
    return true;
}

// Factory method to create the appropriate adapter
// The caller takes ownership of the returned object
AliasAnalysisAdapter* AliasAnalysisAdapter::createAdapter(Module *M, Function *F) {
    if (DyckAAOpt > 0) {
        return new DyckAAAdapter(F);
    } else if (CFLAAOpt > 0) {
        // In a simplified implementation, just return a DyckAA-based adapter
        // that provides conservative answers
        return new DyckAAAdapter(F);
    }
    
    // Default: return a simple adapter that always returns conservative results
    return new DyckAAAdapter(F);
} 