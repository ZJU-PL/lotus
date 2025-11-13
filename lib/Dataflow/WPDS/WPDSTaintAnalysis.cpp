/**
 * @file WPDSTaintAnalysis.cpp
 * @brief Taint analysis using WPDS-based dataflow engine
 */

#include "Dataflow/WPDS/WPDSTaintAnalysis.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "Dataflow/Mono/DataFlowResult.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using dataflow::DataFlowFacts;
using dataflow::GenKillTransformer;
using dataflow::InterProceduralDataFlowEngine;

// Taint analysis tracks which values are tainted (derived from untrusted sources)
// GEN: values that become tainted
// KILL: values that are sanitized

static bool isTaintSource(Instruction* I) {
    // For demo purposes, consider CallInst as potential taint sources
    // In practice, you'd check for specific functions like scanf, recv, etc.
    if (auto* CI = dyn_cast<CallInst>(I)) {
        if (Function* F = CI->getCalledFunction()) {
            StringRef name = F->getName();
            // Common taint sources
            if (name.contains("input") || name.contains("read") || 
                name.contains("recv") || name.contains("scanf") ||
                name.contains("getenv") || name.contains("gets")) {
                return true;
            }
        }
    }
    return false;
}

static bool isSanitizer(Instruction* I) {
    // For demo purposes, consider specific functions as sanitizers
    if (auto* CI = dyn_cast<CallInst>(I)) {
        if (Function* F = CI->getCalledFunction()) {
            StringRef name = F->getName();
            // Common sanitizers
            if (name.contains("sanitize") || name.contains("escape") ||
                name.contains("validate") || name.contains("check")) {
                return true;
            }
        }
    }
    return false;
}

static GenKillTransformer* createTaintTransformer(Instruction* I) {
    std::set<Value*> genSet;
    std::set<Value*> killSet;
    
    // Check if this instruction is a taint source
    if (isTaintSource(I)) {
        // The result of this instruction is tainted
        if (!I->getType()->isVoidTy()) {
            genSet.insert(I);
        }
    }
    
    // Check if this is a sanitizer
    if (isSanitizer(I)) {
        // All arguments are sanitized
        for (Use& U : I->operands()) {
            Value* V = U.get();
            if (isa<Instruction>(V) || isa<Argument>(V)) {
                killSet.insert(V);
            }
        }
    }
    
    // Taint propagation: if any operand is tainted, result is tainted
    // This is handled implicitly by the dataflow framework
    // For binary operations, loads, stores, etc., taint propagates
    if (isa<BinaryOperator>(I) || isa<LoadInst>(I) || isa<CastInst>(I) ||
        isa<GetElementPtrInst>(I) || isa<PHINode>(I) || isa<SelectInst>(I)) {
        // If any operand is tainted (checked by framework), result becomes tainted
        // This is handled by the framework's dataflow equations
    }
    
    // Store propagates taint - this is implicit in the framework
    if (isa<StoreInst>(I)) {
        // If storing a tainted value, the memory location becomes tainted
        // This is handled implicitly by the dataflow framework
    }
    
    DataFlowFacts gen(genSet);
    DataFlowFacts kill(killSet);
    return GenKillTransformer::makeGenKillTransformer(kill, gen);
}

std::unique_ptr<DataFlowResult> runTaintAnalysis(Module& module) {
    InterProceduralDataFlowEngine engine;
    std::set<Value*> initial; // Start with no tainted values
    
    // Alternatively, mark function arguments as initially tainted
    for (auto& F : module) {
        if (F.isDeclaration()) continue;
        for (auto& Arg : F.args()) {
            // Consider all arguments as potentially tainted
            initial.insert(&Arg);
        }
    }
    
    return engine.runForwardAnalysis(module, createTaintTransformer, initial);
}

void demoTaintAnalysis(Module& module) {
    auto result = runTaintAnalysis(module);
    
    errs() << "[WPDS][Taint] Analysis Results:\n";
    for (auto& F : module) {
        if (F.isDeclaration()) continue;
        errs() << "Function: " << F.getName() << "\n";
        
        for (auto& BB : F) {
            for (auto& I : BB) {
                const std::set<Value*>& in = result->IN(&I);
                const std::set<Value*>& gen = result->GEN(&I);
                
                if (!in.empty() || !gen.empty()) {
                    errs() << "  Instruction: ";
                    I.print(errs());
                    errs() << "\n";
                    
                    if (!in.empty()) {
                        errs() << "    Tainted inputs: " << in.size() << " values\n";
                    }
                    
                    if (!gen.empty()) {
                        errs() << "    Generates taint: " << gen.size() << " values\n";
                    }
                    
                    // Check for potential security vulnerabilities
                    if (auto* CI = dyn_cast<CallInst>(&I)) {
                        if (Function* F = CI->getCalledFunction()) {
                            StringRef name = F->getName();
                            // Check for dangerous sinks
                            if (name.contains("system") || name.contains("exec") ||
                                name.contains("strcpy") || name.contains("sprintf")) {
                                if (!in.empty()) {
                                    errs() << "    [WARNING] Tainted data flows into dangerous sink: " 
                                           << name << "\n";
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

