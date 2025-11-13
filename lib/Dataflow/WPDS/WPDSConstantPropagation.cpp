/**
 * @file WPDSConstantPropagation.cpp
 * @brief Constant propagation analysis using WPDS-based dataflow engine
 */

#include "Dataflow/WPDS/WPDSConstantPropagation.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "Dataflow/Mono/DataFlowResult.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using dataflow::DataFlowFacts;
using dataflow::GenKillTransformer;
using dataflow::InterProceduralDataFlowEngine;

// For constant propagation, we track (variable, constant) pairs
// A variable is in the set if it has been assigned a constant value
// This is a simplified version - a full implementation would track the actual constant values

static GenKillTransformer* createConstantPropagationTransformer(Instruction* I) {
    std::set<Value*> genSet;
    std::set<Value*> killSet;
    
    if (auto* SI = dyn_cast<StoreInst>(I)) {
        Value* storedVal = SI->getValueOperand();
        Value* ptr = SI->getPointerOperand();
        
        // If storing a constant, the pointer now holds a constant
        if (isa<ConstantInt>(storedVal) || isa<ConstantFP>(storedVal)) {
            genSet.insert(ptr);
        } else {
            // Storing a non-constant kills the constant property
            killSet.insert(ptr);
        }
    } else if (auto* LI = dyn_cast<LoadInst>(I)) {
        // Load itself doesn't generate/kill unless we track the result
        // For simplicity, we don't track load results as constants
    } else if (auto* BI = dyn_cast<BinaryOperator>(I)) {
        // If both operands are constants, result is constant
        if ((isa<ConstantInt>(BI->getOperand(0)) || isa<ConstantFP>(BI->getOperand(0))) &&
            (isa<ConstantInt>(BI->getOperand(1)) || isa<ConstantFP>(BI->getOperand(1)))) {
            genSet.insert(BI);
        } else {
            // Result is not a constant
            killSet.insert(BI);
        }
    }
    
    DataFlowFacts gen(genSet);
    DataFlowFacts kill(killSet);
    return GenKillTransformer::makeGenKillTransformer(kill, gen);
}

std::unique_ptr<DataFlowResult> runConstantPropagationAnalysis(Module& module) {
    InterProceduralDataFlowEngine engine;
    std::set<Value*> initial; // Start with empty set
    return engine.runForwardAnalysis(module, createConstantPropagationTransformer, initial);
}

void demoConstantPropagationAnalysis(Module& module) {
    auto result = runConstantPropagationAnalysis(module);
    
    errs() << "[WPDS][ConstantProp] Analysis Results:\n";
    for (auto& F : module) {
        if (F.isDeclaration()) continue;
        errs() << "Function: " << F.getName() << "\n";
        
        for (auto& BB : F) {
            for (auto& I : BB) {
                const std::set<Value*>& in = result->IN(&I);
                if (!in.empty()) {
                    errs() << "  Instruction: ";
                    I.print(errs());
                    errs() << "\n    Constants available: " << in.size() << " values\n";
                }
            }
        }
    }
}

