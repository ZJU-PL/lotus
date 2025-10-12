/**
 * @file WPDSUninitializedVariables.cpp
 * @brief Demo implementation of uninitialized variables analysis using WPDS-based dataflow engine
 */

#include "Analysis/WPDS/InterProceduralDataFlow.h"
#include "Analysis/Mono/DataFlowResult.h"
#include <llvm/IR/CFG.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using dataflow::DataFlowFacts;
using dataflow::GenKillTransformer;
using dataflow::InterProceduralDataFlowEngine;

static GenKillTransformer *createUninitTransformer(Instruction *I) {

	std::set<Value *> genSet;
	std::set<Value *> killSet;

	if (auto *AI = dyn_cast<AllocaInst>(I)) {
		// Newly allocated local is uninitialized until stored
		genSet.insert(AI);
	} else if (auto *SI = dyn_cast<StoreInst>(I)) {
		// Store initializes the destination memory
		Value *ptr = SI->getPointerOperand();
		killSet.insert(ptr);
	} else {
		// Other instructions: no effect for the simple demo
	}

	DataFlowFacts gen(genSet);
	DataFlowFacts kill(killSet);
	return GenKillTransformer::makeGenKillTransformer(kill, gen);
}

void demoUninitializedVariablesAnalysis(Module &module) {
	InterProceduralDataFlowEngine engine;
	std::set<Value *> initial; // start with empty fact set
	auto result = engine.runForwardAnalysis(module, createUninitTransformer, initial);

	// Report: a load of a possibly uninitialized location
	for (auto &F : module) {
		if (F.isDeclaration()) continue;
		for (auto &BB : F) {
			for (auto &I : BB) {
				if (auto *LI = dyn_cast<LoadInst>(&I)) {
					Value *ptr = LI->getPointerOperand();
					const std::set<Value *> &in = result->IN(&I);
					if (in.find(ptr) != in.end()) {
						errs() << "[WPDS][Uninit] Potentially uninitialized read at: ";
						if (!I.getFunction()->getName().empty()) {
							errs() << I.getFunction()->getName();
							errs() << ": ";
						}
						if (!I.getName().empty()) errs() << I.getName();
						else errs() << "<unnamed-inst>";
						errs() << "\n";
					}
				}
			}
		}
	}
}

std::unique_ptr<DataFlowResult> runUninitializedVariablesAnalysis(Module &module) {
	InterProceduralDataFlowEngine engine;
	std::set<Value *> initial;
	return engine.runForwardAnalysis(module, createUninitTransformer, initial);
}

static void printValueSet(raw_ostream &OS, const std::set<Value *> &S) {
	OS << "{";
	bool first = true;
	for (auto *V : S) {
		if (!first) OS << ", ";
		first = false;
		if (auto *I = dyn_cast<Instruction>(V)) {
			if (!I->getName().empty()) OS << I->getName();
			else OS << "<inst>";
		} else if (auto *A = dyn_cast<Argument>(V)) {
			if (!A->getName().empty()) OS << A->getName();
			else OS << "<arg>";
		} else if (auto *G = dyn_cast<GlobalValue>(V)) {
			OS << G->getName();
		} else {
			OS << "<val>";
		}
	}
	OS << "}";
}

void queryAnalysisResults(Module &module, const DataFlowResult &result, Instruction *targetInst) {
    (void)module;
    if (!targetInst) return;
	auto itF = targetInst->getFunction();
	(void)itF;
	errs() << "[WPDS][Query] IN  = ";
	printValueSet(errs(), const_cast<DataFlowResult&>(result).IN(targetInst));
	errs() << "\n";
	errs() << "[WPDS][Query] GEN = ";
	printValueSet(errs(), const_cast<DataFlowResult&>(result).GEN(targetInst));
	errs() << "\n";
	errs() << "[WPDS][Query] KILL= ";
	printValueSet(errs(), const_cast<DataFlowResult&>(result).KILL(targetInst));
	errs() << "\n";
	errs() << "[WPDS][Query] OUT = ";
	printValueSet(errs(), const_cast<DataFlowResult&>(result).OUT(targetInst));
	errs() << "\n";
}
