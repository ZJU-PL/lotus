/**
 * @file UseOfUninitializedVariableChecker.cpp
 * @brief Implementation of uninitialized variable use checker
 */

#include "Checker/GVFA/UseOfUninitializedVariableChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugTypes.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void UseOfUninitializedVariableChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find uninitialized value sources
    for (auto &F : *M) {
        if (F.isDeclaration()) continue;
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                // Uninitialized allocas (stack variables without initialization)
                if (auto *AI = dyn_cast<AllocaInst>(&I)) {
                    // Check if there's a store before first use
                    bool hasInitialStore = false;
                    for (auto *U : AI->users()) {
                        if (auto *SI = dyn_cast<StoreInst>(U)) {
                            if (SI->getPointerOperand() == AI && 
                                SI->getParent() == &BB) {
                                hasInitialStore = true;
                                break;
                            }
                        }
                    }
                    if (!hasInitialStore) {
                        Sources[{AI, 1}] = 1;
                    }
                }
                // Explicit undef constants
                else if (isa<UndefValue>(&I)) {
                    Sources[{&I, 1}] = 1;
                }
                // Load from potentially uninitialized memory
                else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    if (auto *AI = dyn_cast<AllocaInst>(LI->getPointerOperand())) {
                        // This load might read uninitialized memory
                        Sources[{LI, 1}] = 1;
                    }
                }
            }
        }
    }
}

void UseOfUninitializedVariableChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    // Find uses of values that should not be undefined
    for (auto &F : *M) {
        if (F.isDeclaration()) continue;
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                const Value *UncheckedOp = nullptr;
                
                // Arithmetic operations
                if (isa<BinaryOperator>(&I) || isa<UnaryOperator>(&I)) {
                    // Use of undef in arithmetic is problematic
                    if (I.getNumOperands() > 0) {
                        UncheckedOp = I.getOperand(0);
                    }
                }
                // Comparisons
                else if (auto *Cmp = dyn_cast<CmpInst>(&I)) {
                    UncheckedOp = Cmp->getOperand(0);
                }
                // Return statements
                else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
                    if (RI->getReturnValue()) {
                        UncheckedOp = RI->getReturnValue();
                    }
                }
                // Function arguments
                else if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (CI->getNumOperands() > 0) {
                        // Check first non-function operand
                        for (unsigned i = 0; i < CI->arg_size(); ++i) {
                            Value *Arg = CI->getArgOperand(i);
                            if (!isa<Function>(Arg)) {
                                Sinks[Arg] = new std::set<const Value *>();
                                Sinks[Arg]->insert(CI);
                            }
                        }
                        continue;
                    }
                }
                // Store instructions
                else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    UncheckedOp = SI->getValueOperand();
                }
                
                if (UncheckedOp) {
                    Sinks[UncheckedOp] = new std::set<const Value *>();
                    Sinks[UncheckedOp]->insert(&I);
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Transfer Validation
//===----------------------------------------------------------------------===//

bool UseOfUninitializedVariableChecker::isValidTransfer(const Value * /*From*/, const Value *To) const {
    // Block flow through initialization functions
    if (auto *CI = dyn_cast<CallInst>(To)) {
        if (auto *F = CI->getCalledFunction()) {
            StringRef Name = F->getName();
            if (Name.contains("init") || Name.contains("memset") || 
                Name.contains("bzero") || Name.contains("memcpy")) {
                return false; // These sanitize uninitialized values
            }
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Bug Reporting
//===----------------------------------------------------------------------===//

int UseOfUninitializedVariableChecker::registerBugType() {
    BugReportMgr& mgr = BugReportMgr::get_instance();
    return mgr.register_bug_type(
        "Use of Uninitialized Variable",
        BugDescription::BI_HIGH,
        BugDescription::BC_SECURITY,
        "CWE-457"
    );
}

void UseOfUninitializedVariableChecker::reportVulnerability(
    int bugTypeId, const Value *Source, const Value *Sink, 
    const std::set<const Value *> *SinkInsts) {
    
    BugReport* report = new BugReport(bugTypeId);
    
    // Add source step
    if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
        std::string desc = "Uninitialized value originates here";
        if (isa<AllocaInst>(SourceInst)) {
            desc = "Local variable allocated without initialization";
        } else if (isa<LoadInst>(SourceInst)) {
            desc = "Load from uninitialized memory";
        }
        report->append_step(const_cast<Instruction*>(SourceInst), desc);
    }
    
    // Add intermediate steps if GVFA is available
    if (GVFA && Sink) {
        try {
            std::vector<const Value *> witnessPath = GVFA->getWitnessPath(Source, Sink);
            if (witnessPath.size() > 2) {
                for (size_t i = 1; i + 1 < witnessPath.size(); ++i) {
                    const Value *V = witnessPath[i];
                    if (!V || !isa<Instruction>(V)) continue;
                    
                    const Instruction *I = cast<Instruction>(V);
                    std::string desc = "Potentially uninitialized value propagates";
                    report->append_step(const_cast<Instruction*>(I), desc);
                }
            }
        } catch (...) {
            // Continue without witness path
        }
    }
    
    // Add sink steps
    if (SinkInsts) {
        for (const Value *SI : *SinkInsts) {
            if (auto *SinkInst = dyn_cast<Instruction>(SI)) {
                std::string desc = "Use of potentially uninitialized value";
                if (isa<ReturnInst>(SinkInst)) {
                    desc = "Return of potentially uninitialized value";
                } else if (isa<CallInst>(SinkInst)) {
                    desc = "Potentially uninitialized value passed to function";
                } else if (isa<StoreInst>(SinkInst)) {
                    desc = "Store of potentially uninitialized value";
                }
                report->append_step(const_cast<Instruction*>(SinkInst), desc);
            }
        }
    }
    
    report->set_conf_score(75);
    BugReportMgr::get_instance().insert_report(bugTypeId, report);
}

//===----------------------------------------------------------------------===//
// High-Level Detection
//===----------------------------------------------------------------------===//

int UseOfUninitializedVariableChecker::detectAndReport(
    Module *M, DyckGlobalValueFlowAnalysis *GVFA,
    bool contextSensitive, bool verbose) {
    
    this->GVFA = GVFA;
    int bugTypeId = registerBugType();
    
    VulnerabilitySourcesType Sources;
    VulnerabilitySinksType Sinks;
    getSources(M, Sources);
    getSinks(M, Sinks);
    
    int vulnCount = 0;
    
    for (const auto &SinkPair : Sinks) {
        const Value *SinkValue = SinkPair.first;
        const std::set<const Value *> *SinkInsts = SinkPair.second;
        
        for (const auto &SourceEntry : Sources) {
            const Value *SourceValue = SourceEntry.first.first;
            int SourceMask = SourceEntry.second;
            
            bool reachable = contextSensitive 
                ? GVFA->contextSensitiveReachable(SourceValue, SinkValue)
                : GVFA->reachable(SinkValue, SourceMask);
            
            if (reachable) {
                vulnCount++;
                reportVulnerability(bugTypeId, SourceValue, SinkValue, SinkInsts);
                
                if (verbose) {
                    llvm::outs() << "VULNERABILITY: " << getCategory() << "\n  Source: ";
                    SourceValue->print(llvm::outs());
                    llvm::outs() << "\n  Sink: ";
                    SinkValue->print(llvm::outs());
                    llvm::outs() << "\n";
                }
            }
        }
    }
    
    return vulnCount;
}

