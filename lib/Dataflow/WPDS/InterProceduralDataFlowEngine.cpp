#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/SaturationProcess.h"
#include <llvm/IR/CFG.h>

namespace dataflow {

using namespace wpds;
using namespace llvm;

// Helper functor for copying CA transitions
template<typename T>
struct CopyTransitionsFunctor : public wpds::util::TransActionFunctor<T> {
    wpds::CA<T>* targetCA;
    
    CopyTransitionsFunctor(wpds::CA<T>* target) : targetCA(target) {}
    
    void operator()(const wpds::CA<T>::catrans_t& t) override {
        targetCA->add(t->from_state(), t->stack(), t->to_state(), 
                     t->semiring_element().get_ptr());
    }
};

InterProceduralDataFlowEngine::InterProceduralDataFlowEngine() = default;

std::unique_ptr<DataFlowResult> InterProceduralDataFlowEngine::runForwardAnalysis(
    Module& m,
    const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
    const std::set<Value*>& initialFacts) {
    
    // Create semiring and WPDS
    Semiring<GenKillTransformer> semiring(GenKillTransformer::one());
    WPDS<GenKillTransformer> wpds(semiring, Query::poststar());
    
    // Build WPDS from LLVM module
    buildWPDS(m, wpds, createTransformer);
    
    // Create initial configuration automaton
    CA<GenKillTransformer> initialCA(semiring);
    buildInitialAutomaton(m, initialCA, initialFacts, true);
    
    // Run post* algorithm
    CA<GenKillTransformer> resultCA(semiring);
    
    // Copy initial CA transitions to result CA using functor
    CopyTransitionsFunctor<GenKillTransformer> copier(&resultCA);
    initialCA.for_each(copier);
    
    wpds::SaturationProcess<GenKillTransformer> satProcess(wpds, resultCA, semiring, Query::poststar());
    satProcess.poststar();
    
    // Extract results
    currentResult = std::make_unique<DataFlowResult>();
    extractResults(m, resultCA, currentResult, true);
    
    return std::move(currentResult);
}

std::unique_ptr<DataFlowResult> InterProceduralDataFlowEngine::runBackwardAnalysis(
    Module& m,
    const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
    const std::set<Value*>& initialFacts) {
    
    // Create semiring and WPDS
    Semiring<GenKillTransformer> semiring(GenKillTransformer::one(), false); // backward direction
    WPDS<GenKillTransformer> wpds(semiring, Query::prestar());
    
    // Build WPDS from LLVM module
    buildWPDS(m, wpds, createTransformer);
    
    // Create initial configuration automaton
    CA<GenKillTransformer> initialCA(semiring);
    buildInitialAutomaton(m, initialCA, initialFacts, false);
    
    // Run pre* algorithm
    CA<GenKillTransformer> resultCA(semiring);
    
    // Copy initial CA transitions to result CA using functor
    CopyTransitionsFunctor<GenKillTransformer> copier(&resultCA);
    initialCA.for_each(copier);
    
    wpds::SaturationProcess<GenKillTransformer> satProcess(wpds, resultCA, semiring, Query::prestar());
    satProcess.prestar();
    
    // Extract results
    currentResult = std::make_unique<DataFlowResult>();
    extractResults(m, resultCA, currentResult, false);
    
    return std::move(currentResult);
}

const std::set<Value*>& InterProceduralDataFlowEngine::getInSet(Instruction* inst) const {
    if (!currentResult) {
        static std::set<Value*> emptySet;
        return emptySet;
    }
    return currentResult->IN(inst);
}

const std::set<Value*>& InterProceduralDataFlowEngine::getOutSet(Instruction* inst) const {
    if (!currentResult) {
        static std::set<Value*> emptySet;
        return emptySet;
    }
    return currentResult->OUT(inst);
}

void InterProceduralDataFlowEngine::buildWPDS(
    Module& m, 
    WPDS<GenKillTransformer>& wpds,
    const std::function<GenKillTransformer*(Instruction*)>& createTransformer) {
    
    // Clear previous mappings
    functionToKey.clear();
    functionExitToKey.clear();
    instToKey.clear();
    bbToKey.clear();
    keyToInst.clear();

    // PDS control state (single state for whole program)
    wpds_key_t controlState = str2key("q");
    
    // First pass: Create function entry and exit keys for all functions
    for (auto& F : m) {
        if (F.isDeclaration()) continue;
        
        std::string fname = F.getName().str();
        if (fname.empty()) {
            fname = "func_" + std::to_string((uintptr_t)&F);
        }
        
        wpds_key_t funcEntry = new_str2key(("entry_" + fname).c_str());
        wpds_key_t funcExit = new_str2key(("exit_" + fname).c_str());
        functionToKey[&F] = funcEntry;
        functionExitToKey[&F] = funcExit;
        
        wpds.add_element_to_P(controlState);
    }
    
    // Second pass: Create rules for each function
    for (auto& F : m) {
        if (F.isDeclaration()) continue;
        
        wpds_key_t funcEntry = functionToKey[&F];
        wpds_key_t funcExit = functionExitToKey[&F];
        
        // Map basic blocks to keys
        for (auto& BB : F) {
            std::string bbName = BB.getName().str();
            if (bbName.empty()) {
                bbName = "bb_" + std::to_string((uintptr_t)&BB);
            }
            wpds_key_t bbKey = new_str2key(bbName.c_str());
            bbToKey[&BB] = bbKey;
        }
        
        // Rule from function entry to first basic block
        BasicBlock& entryBB = F.getEntryBlock();
        wpds_key_t entryBBKey = bbToKey[&entryBB];
        wpds.add_rule(controlState, funcEntry, controlState, entryBBKey, 
                     GenKillTransformer::one());
        
        // Process each basic block
        for (auto& BB : F) {
            wpds_key_t bbKey = bbToKey[&BB];
            
            Instruction* prevInst = nullptr;
            wpds_key_t prevKey = bbKey;
            
            // Process instructions in the basic block
            for (auto& I : BB) {
                // Create instruction key
                std::string instName = I.getName().str();
                if (instName.empty()) {
                    instName = "inst_" + std::to_string((uintptr_t)&I);
                }
                wpds_key_t instKey = new_str2key(instName.c_str());
                instToKey[&I] = instKey;
                keyToInst[instKey] = &I;
                
                // Create transformer for this instruction
                GenKillTransformer* transformer = createTransformer(&I);
                
                // Add rule from previous location to this instruction
                wpds.add_rule(controlState, prevKey, controlState, instKey, transformer);
                
                // Handle different instruction types
                if (auto* callInst = dyn_cast<CallInst>(&I)) {
                    Function* calledFunc = callInst->getCalledFunction();
                    if (calledFunc && !calledFunc->isDeclaration() && 
                        functionToKey.find(calledFunc) != functionToKey.end()) {
                        
                        // Interprocedural call: <q, instKey> -> <q, calledEntry, returnKey>
                        wpds_key_t calledEntry = functionToKey[calledFunc];
                        wpds_key_t calledExit = functionExitToKey[calledFunc];
                        
                        std::string returnName = "ret_" + instName;
                        wpds_key_t returnKey = new_str2key(returnName.c_str());
                        
                        // Call rule: push callee and return point
                        wpds.add_rule(controlState, instKey, 
                                    controlState, calledEntry, returnKey,
                                    GenKillTransformer::one());
                        
                        // Return rule: pop from callee exit
                        wpds.add_rule(controlState, calledExit,
                                    controlState,
                                    GenKillTransformer::one());
                        
                        prevKey = returnKey;
                        prevInst = &I;
                        continue;
                    }
                }
                
                if (isa<ReturnInst>(&I)) {
                    // Return: <q, instKey> -> <q, funcExit>
                    wpds.add_rule(controlState, instKey, 
                                controlState, funcExit,
                                GenKillTransformer::one());
                    prevKey = instKey;
                    prevInst = &I;
                    continue;
                }
                
                // Regular instruction - continue to next
                prevKey = instKey;
                prevInst = &I;
            }
            
            // Connect terminator to successor basic blocks
            if (Instruction* terminator = BB.getTerminator()) {
                wpds_key_t termKey = instToKey[terminator];
                
                // If terminator is not a return or call, connect to successors
                if (!isa<ReturnInst>(terminator) && !isa<CallInst>(terminator)) {
                    for (BasicBlock* succBB : successors(&BB)) {
                        wpds_key_t succBBKey = bbToKey[succBB];
                        wpds.add_rule(controlState, termKey,
                                    controlState, succBBKey,
                                    GenKillTransformer::one());
                    }
                }
            }
        }
    }
}

void InterProceduralDataFlowEngine::buildInitialAutomaton(
    Module& m, 
    CA<GenKillTransformer>& ca,
    const std::set<Value*>& initialFacts,
    bool isForward) {
    
    wpds_key_t caState = str2key("caState");
    wpds_key_t acceptState = str2key("accept");
    
    ca.add_initial_state(caState);
    ca.add_final_state(acceptState);
    
    if (isForward) {
        // For forward analysis: start from main function entry
        Function* mainFn = nullptr;
        for (auto& F : m) {
            if (F.isDeclaration()) continue;
            if (F.getName() == "main") { mainFn = &F; break; }
            if (!mainFn) mainFn = &F; // fallback to first defined function
        }
        if (mainFn) {
            wpds_key_t mainEntry = functionToKey[mainFn];
            GenKillTransformer* initTrans = GenKillTransformer::makeGenKillTransformer(
                DataFlowFacts::EmptySet(),
                DataFlowFacts(initialFacts)
            );
            ca.add(caState, mainEntry, acceptState, initTrans);
        }
    } else {
        // For backward analysis: start from all exit points
        for (auto& kv : functionExitToKey) {
            wpds_key_t exitKey = kv.second;
            
            GenKillTransformer* initTrans = GenKillTransformer::makeGenKillTransformer(
                DataFlowFacts::EmptySet(),
                DataFlowFacts(initialFacts)
            );
            
            ca.add(caState, exitKey, acceptState, initTrans);
        }
    }
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForFunction(Function* f) {
    auto it = functionToKey.find(f);
    if (it != functionToKey.end()) {
        return it->second;
    }
    return WPDS_EPSILON;
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForInstruction(Instruction* inst) {
    auto it = instToKey.find(inst);
    if (it != instToKey.end()) {
        return it->second;
    }
    return WPDS_EPSILON;
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForBasicBlock(BasicBlock* bb) {
    auto it = bbToKey.find(bb);
    if (it != bbToKey.end()) {
        return it->second;
    }
    return WPDS_EPSILON;
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForCallSite(CallInst* callInst) {
    std::string instName = callInst->getName().str();
    if (instName.empty()) {
        instName = "inst_" + std::to_string((uintptr_t)callInst);
    }
    return str2key(("callsite_" + instName).c_str());
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForReturnSite(CallInst* callInst) {
    std::string instName = callInst->getName().str();
    if (instName.empty()) {
        instName = "inst_" + std::to_string((uintptr_t)callInst);
    }
    return str2key(("ret_" + instName).c_str());
}

void InterProceduralDataFlowEngine::extractResults(
    Module& m,
    CA<GenKillTransformer>& resultCA,
    std::unique_ptr<DataFlowResult>& result,
    bool isForward) {
    
    wpds_key_t caState = str2key("caState");
    wpds_key_t acceptState = str2key("accept");
    
    // First, compute OUT sets directly from WPDS weights; then derive IN.
    for (auto& kv : instToKey) {
        Instruction* inst = kv.first;
        wpds_key_t instKey = kv.second;

        // Query the transition summarizing paths to this program point
        wpds::CA<GenKillTransformer>::catrans_t trans;
        bool found = resultCA.find(caState, instKey, acceptState, trans);
        if (!found || !trans.get_ptr()) {
            continue;
        }

        GenKillTransformer* pathSummary = trans->semiring_element().get_ptr();
        if (!pathSummary) continue;

        // Store GEN/KILL summary at this point for debugging/inspection
        result->GEN(inst) = pathSummary->getGen().getFacts();
        result->KILL(inst) = pathSummary->getKill().getFacts();

        // OUT is the application of the path summary to the empty set,
        // which carries the seeded initial facts from the initial transition.
        DataFlowFacts outFacts = pathSummary->apply(DataFlowFacts::EmptySet());
        result->OUT(inst) = outFacts.getFacts();
    }

    // Derive IN from OUT using local control-flow where needed
    for (auto& kv : instToKey) {
        Instruction* inst = kv.first;

        if (isForward) {
            // IN of first instruction in a BB is the join of predecessor OUTs;
            // otherwise it is the OUT of the previous instruction.
            if (inst == &inst->getParent()->front()) {
                std::set<Value*> inSet;
                if (&inst->getParent()->getParent()->getEntryBlock() == inst->getParent()) {
                    // function entry; rely on path summary already seeded
                    // with initial facts; IN equals previous OUT of predecessors (none).
                } else {
                    for (auto* predBB : predecessors(inst->getParent())) {
                        if (predBB->empty()) continue;
                        Instruction* predTerm = predBB->getTerminator();
                        auto& predOut = result->OUT(predTerm);
                        inSet.insert(predOut.begin(), predOut.end());
                    }
                }
                result->IN(inst) = inSet;
            } else {
                auto prevIt = inst->getIterator();
                --prevIt;
                result->IN(inst) = result->OUT(&*prevIt);
            }
        } else {
            // Backward: OUT of inst is already computed from WPDS;
            // IN is transfer of local instruction on OUT.
            // Approximate IN as OUT of predecessors/successors relation.
            std::set<Value*> outSet = result->OUT(inst);
            // Apply local transformer if available (via modeling, the rule weight
            // to reach the successor includes local effect; thus IN can be
            // approximated by removing local GEN then adding local KILL inversely).
            // Keep simple: IN = OUT for summarized WPDS results.
            result->IN(inst) = outSet;
        }
    }
}

} // namespace dataflow
