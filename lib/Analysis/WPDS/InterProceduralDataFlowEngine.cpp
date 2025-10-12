#include "Analysis/WPDS/InterProceduralDataFlow.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/SaturationProcess.h"

namespace dataflow {

using namespace wpds;

InterProceduralDataFlowEngine::InterProceduralDataFlowEngine() = default;

std::unique_ptr<DataFlowResult> InterProceduralDataFlowEngine::runForwardAnalysis(
    Module& m,
    const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
    const std::set<Value*>& initialFacts) {
    
    // Create semiring and WPDS
    Semiring<GenKillTransformer> semiring(GenKillTransformer::one());
    WPDS<GenKillTransformer> wpds(semiring);
    
    // Build WPDS from LLVM module
    buildWPDS(m, wpds, createTransformer);
    
    // Create initial configuration automaton
    CA<GenKillTransformer> initialCA(semiring);
    buildInitialAutomaton(m, initialCA, initialFacts, true);
    
    // Run post* algorithm - using the wrapper function
    CA<GenKillTransformer> resultCA = poststar(wpds, initialCA, semiring);
    
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
    Semiring<GenKillTransformer> semiring(GenKillTransformer::one());
    WPDS<GenKillTransformer> wpds(semiring);
    
    // Build WPDS from LLVM module
    buildWPDS(m, wpds, createTransformer);
    
    // Create initial configuration automaton
    CA<GenKillTransformer> initialCA(semiring);
    buildInitialAutomaton(m, initialCA, initialFacts, false);
    
    // Run pre* algorithm - using the wrapper function
    CA<GenKillTransformer> resultCA = prestar(wpds, initialCA, semiring);
    
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
    instToKey.clear();
    bbToKey.clear();
    keyToInst.clear();

    // Create a control state for PDS
    wpds_key_t controlState = str2key("q");
    
    // Create a stack bottom symbol
    wpds_key_t stackBottom = str2key("stack_bottom");
    
    // For each function in the module
    for (auto& F : m) {
        if (F.isDeclaration()) continue;
        
        // Create function entry and exit keys
        std::string fname = F.getName().str();
        wpds_key_t funcEntry = new_str2key(("entry_" + fname).c_str());
        wpds_key_t funcExit = new_str2key(("exit_" + fname).c_str());
        functionToKey[&F] = funcEntry;
        functionExitToKey[&F] = funcExit;
        
        // Connect function entry symbol to the first basic block
        {
            BasicBlock &entryBB = F.getEntryBlock();
            wpds_key_t entryBBKey = WPDS_EPSILON;
            // bbToKey is filled inside loop; ensure a temporary key for entry if needed
            std::string bbName = entryBB.getName().str();
            if (bbName.empty()) {
                bbName = "bb_" + std::to_string(bbToKey.size());
            }
            entryBBKey = new_str2key(bbName.c_str());
            bbToKey[&entryBB] = entryBBKey;
            wpds.add_rule(controlState, funcEntry, controlState, entryBBKey, GenKillTransformer::one());
        }

        // For each basic block in the function
        for (auto& BB : F) {
            // Create basic block key if not already created (e.g., entry BB)
            wpds_key_t bbKey;
            auto bbIt = bbToKey.find(&BB);
            if (bbIt == bbToKey.end()) {
                std::string bbName = BB.getName().str();
                if (bbName.empty()) {
                    bbName = "bb_" + std::to_string(bbToKey.size());
                }
                bbKey = new_str2key(bbName.c_str());
                bbToKey[&BB] = bbKey;
            } else {
                bbKey = bbIt->second;
            }
            
            // For each instruction in the basic block
            for (auto& I : BB) {
                // Create instruction key
                std::string instName = I.getName().str();
                if (instName.empty()) {
                    instName = "inst_" + std::to_string(instToKey.size());
                }
                wpds_key_t instKey = new_str2key(instName.c_str());
                instToKey[&I] = instKey;
                keyToInst[instKey] = &I;
                
                // Create the gen/kill transformer for this instruction
                GenKillTransformer* transformer = createTransformer(&I);
                
                // Add intraprocedural edges
                if (&I == &BB.front()) {
                    // First instruction of BB, connect from BB
                    wpds.add_rule(controlState, bbKey, controlState, instKey, transformer);
                } else {
                    // Connect from previous instruction
                    auto prevIt = I.getIterator();
                    prevIt--;
                    Instruction* prevInst = &(*prevIt);
                    wpds_key_t prevInstKey = instToKey[prevInst];
                    wpds.add_rule(controlState, prevInstKey, controlState, instKey, transformer);
                }
                
                // Handle call instructions
                if (auto* callInst = dyn_cast<CallInst>(&I)) {
                    Function* calledFunc = callInst->getCalledFunction();
                    if (calledFunc && !calledFunc->isDeclaration()) {
                        // Create call site key
                        wpds_key_t callSiteKey = new_str2key(("callsite_" + instName).c_str());
                        
                        // Add interprocedural edges for the call
                        wpds_key_t calledFuncEntry = functionToKey[calledFunc];
                        wpds_key_t calledFuncExit  = functionExitToKey[calledFunc];
                        wpds.add_rule(
                            controlState, instKey, 
                            controlState, calledFuncEntry, callSiteKey, 
                            GenKillTransformer::one()
                        );
                        
                        // Create return site key
                        wpds_key_t returnSiteKey = new_str2key(("returnsite_" + instName).c_str());
                        
                        // If there's a next instruction, connect to it on return
                        auto nextIt = I.getIterator();
                        nextIt++;
                        if (nextIt != BB.end()) {
                            Instruction* nextInst = &(*nextIt);
                            wpds_key_t nextInstKey = instToKey[nextInst];
                            
                            // Connect function exit to return site, then to next instruction
                            wpds.add_rule(
                                controlState, calledFuncExit,
                                controlState,
                                returnSiteKey,
                                GenKillTransformer::one()
                            );

                            // Connect return site to next instruction
                            wpds.add_rule(
                                controlState, returnSiteKey, 
                                controlState, nextInstKey, 
                                GenKillTransformer::one()
                            );
                        }
                    }
                }
                
                // Handle return instructions
                if (isa<ReturnInst>(&I)) {
                    wpds.add_rule(
                        controlState, instKey, 
                        controlState, funcExit, 
                        GenKillTransformer::one()
                    );
                }
            }
            
            // Connect basic blocks via control flow
            for (auto* succBB : successors(&BB)) {
                wpds_key_t succBBKey = bbToKey[succBB];
                Instruction* lastInst = BB.getTerminator();
                wpds_key_t lastInstKey = instToKey[lastInst];
                
                wpds.add_rule(
                    controlState, lastInstKey, 
                    controlState, succBBKey, 
                    GenKillTransformer::one()
                );
            }
        }
    }
    
    // Add initial rule for program entry
    if (!m.empty() && !m.begin()->isDeclaration()) {
        Function* mainFunc = &(*m.begin());  // Assuming first function is main
        wpds_key_t mainEntry = functionToKey[mainFunc];
        
        // Rule to start execution at main
        wpds.add_rule(
            controlState, stackBottom, 
            controlState, mainEntry, stackBottom, 
            GenKillTransformer::one()
        );
    }
}

void InterProceduralDataFlowEngine::buildInitialAutomaton(
    Module& m, 
    CA<GenKillTransformer>& ca,
    const std::set<Value*>& initialFacts,
    bool isForward) {
    
    // Create states for the automaton
    wpds_key_t initialState = str2key("p");
    wpds_key_t acceptingState = str2key("accepting");
    
    // Add transitions for each program point
    DataFlowFacts facts(initialFacts);
    
    if (isForward) {
        // For forward analysis, create transitions from initial state to program entry points
        if (!m.empty() && !m.begin()->isDeclaration()) {
            Function* mainFunc = &(*m.begin());  // Assuming first function is main
            wpds_key_t mainEntry = functionToKey[mainFunc];
            
            // Add transition accepting the main entry state
            ca.add(initialState, mainEntry, acceptingState, 
                  GenKillTransformer::makeGenKillTransformer(
                      DataFlowFacts::EmptySet(), facts));
        }
    } else {
        // For backward analysis, create transitions from all program points
        // to the accepting state with initial facts
        for (auto& kv : instToKey) {
            Instruction* inst = kv.first;
            wpds_key_t instKey = kv.second;
            
            // We're especially interested in return instructions and call sites
            if (isa<ReturnInst>(inst) || isa<CallInst>(inst)) {
                ca.add(initialState, instKey, acceptingState, 
                      GenKillTransformer::makeGenKillTransformer(
                          DataFlowFacts::EmptySet(), facts));
            }
        }
    }
    
    ca.make_state(initialState);
    ca.make_state(acceptingState);
    ca.add_initial_state(initialState);
    ca.add_final_state(acceptingState);
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
        auto it = instToKey.find(callInst);
        if (it != instToKey.end()) {
            // Instead of key2str, just use a generic string
            instName = "callsite_" + std::to_string(reinterpret_cast<std::uintptr_t>(callInst));
        } else {
            instName = "unknown_call";
        }
    }
    return str2key(("callsite_" + instName).c_str());
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForReturnSite(CallInst* callInst) {
    std::string instName = callInst->getName().str();
    if (instName.empty()) {
        auto it = instToKey.find(callInst);
        if (it != instToKey.end()) {
            // Instead of key2str, just use a generic string
            instName = "returnsite_" + std::to_string(reinterpret_cast<std::uintptr_t>(callInst));
        } else {
            instName = "unknown_call";
        }
    }
    return str2key(("returnsite_" + instName).c_str());
}

void InterProceduralDataFlowEngine::extractResults(
    Module& /* m */,
    CA<GenKillTransformer>& resultCA,
    std::unique_ptr<DataFlowResult>& result,
    bool isForward) {
    
    // For each instruction in the program
    for (auto& kv : instToKey) {
        Instruction* inst = kv.first;
        wpds_key_t instKey = kv.second;
        
        // Create a query to find the data flow facts at this program point
        wpds_key_t initialState = str2key("p");
        wpds_key_t acceptingState = str2key("accepting");
        
        // Query the automaton for the instruction key
        // Use find instead of get_trans
        CA<GenKillTransformer>::catrans_t transition;
        bool found = resultCA.find(initialState, instKey, acceptingState, transition);
        
        if (found && transition.get_ptr() != nullptr) {
            // Get transformer from transition
            // We need to cast this properly from sem_elem_t
            auto* transformer = transition->semiring_element().get_ptr();
            
            // Convert to DataFlowResult format
            result->GEN(inst) = transformer->getGen().getFacts();
            result->KILL(inst) = transformer->getKill().getFacts();
            
            // Initialize IN and OUT sets
            if (isForward) {
                // For forward analysis:
                // OUT[inst] = GEN[inst] ∪ (IN[inst] - KILL[inst])
                // For entry points, IN is empty or some user-specified set
                if (auto* firstInst = dyn_cast<Instruction>(&inst->getParent()->front())) {
                    if (inst == firstInst) {
                        // This is a basic block entry - IN depends on predecessors
                        if (inst->getParent()->hasNPredecessorsOrMore(1)) {
                            // Take the union of out sets from predecessors
                            for (auto* predBB : predecessors(inst->getParent())) {
                                Instruction* predTerminator = predBB->getTerminator();
                                result->IN(inst).insert(
                                    result->OUT(predTerminator).begin(),
                                    result->OUT(predTerminator).end()
                                );
                            }
                        }
                    } else {
                        // Not a BB entry - get IN from the OUT of previous instruction
                        auto prevIt = inst->getIterator();
                        prevIt--;
                        Instruction* prevInst = &(*prevIt);
                        result->IN(inst) = result->OUT(prevInst);
                    }
                }
                
                // Apply gen/kill to compute OUT
                std::set<Value*> outSet;
                // OUT = (IN - KILL) ∪ GEN
                std::set_difference(
                    result->IN(inst).begin(), result->IN(inst).end(),
                    result->KILL(inst).begin(), result->KILL(inst).end(),
                    std::inserter(outSet, outSet.begin())
                );
                outSet.insert(result->GEN(inst).begin(), result->GEN(inst).end());
                result->OUT(inst) = outSet;
            } else {
                // For backward analysis:
                // IN[inst] = GEN[inst] ∪ (OUT[inst] - KILL[inst])
                // For exit points, OUT is empty or some user-specified set
                if (inst->isTerminator()) {
                    // This is a basic block exit - OUT depends on successors
                    if (auto* br = dyn_cast<BranchInst>(inst)) {
                        // Take the union of in sets from successors
                        for (unsigned i = 0; i < br->getNumSuccessors(); i++) {
                            BasicBlock* succBB = br->getSuccessor(i);
                            Instruction* succInst = &succBB->front();
                            result->OUT(inst).insert(
                                result->IN(succInst).begin(),
                                result->IN(succInst).end()
                            );
                        }
                    }
                } else {
                    // Not a BB exit - get OUT from the IN of next instruction
                    auto nextIt = inst->getIterator();
                    nextIt++;
                    Instruction* nextInst = &(*nextIt);
                    result->OUT(inst) = result->IN(nextInst);
                }
                
                // Apply gen/kill to compute IN
                std::set<Value*> inSet;
                // IN = (OUT - KILL) ∪ GEN
                std::set_difference(
                    result->OUT(inst).begin(), result->OUT(inst).end(),
                    result->KILL(inst).begin(), result->KILL(inst).end(),
                    std::inserter(inSet, inSet.begin())
                );
                inSet.insert(result->GEN(inst).begin(), result->GEN(inst).end());
                result->IN(inst) = inSet;
            }
        }
    }
}

} // namespace dataflow 