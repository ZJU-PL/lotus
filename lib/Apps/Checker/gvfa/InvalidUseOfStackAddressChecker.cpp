/**
 * @file InvalidUseOfStackAddressChecker.cpp
 * @brief Implementation of invalid-use-of-stack-address checker
 */

#include "Apps/Checker/gvfa/InvalidUseOfStackAddressChecker.h"
#include "Apps/Checker/Report/BugReportMgr.h"
#include "Apps/Checker/Report/BugReport.h"
#include "Apps/Checker/Report/BugTypes.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void InvalidUseOfStackAddressChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find stack allocations (potential sources of escaped stack addresses)
    for (auto &F : *M) {
        if (F.isDeclaration()) continue;
        
        // Skip main function - stack addresses in main often have global lifetime
        if (F.getName() == "main") continue;
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *AI = dyn_cast<AllocaInst>(&I)) {
                    Sources[{AI, 1}] = 1;
                }
            }
        }
    }
}

void InvalidUseOfStackAddressChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    // Find places where stack addresses can escape
    for (auto &F : *M) {
        if (F.isDeclaration()) continue;
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                // Return instructions - stack address returned to caller
                if (auto *RI = dyn_cast<ReturnInst>(&I)) {
                    if (Value *RetVal = RI->getReturnValue()) {
                        if (RetVal->getType()->isPointerTy()) {
                            Sinks[RetVal] = new std::set<const Value *>();
                            Sinks[RetVal]->insert(RI);
                        }
                    }
                }
                // Stores to global variables or escaped memory
                else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *PtrOp = SI->getPointerOperand();
                    if (isa<GlobalVariable>(PtrOp)) {
                        Value *ValOp = SI->getValueOperand();
                        if (ValOp->getType()->isPointerTy()) {
                            Sinks[ValOp] = new std::set<const Value *>();
                            Sinks[ValOp]->insert(SI);
                        }
                    }
                }
                // Function arguments that might store the pointer
                else if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (auto *Callee = CI->getCalledFunction()) {
                        // Skip known safe functions
                        StringRef Name = Callee->getName();
                        if (Name.startswith("llvm.") || Name == "free" || 
                            Name == "printf" || Name == "fprintf") {
                            continue;
                        }
                        
                        // Check pointer arguments that might escape
                        for (unsigned i = 0; i < CI->arg_size(); ++i) {
                            Value *Arg = CI->getArgOperand(i);
                            if (Arg->getType()->isPointerTy()) {
                                // Conservative: assume pointer arguments might escape
                                // unless function is known to be safe
                                if (Callee->isDeclaration()) {
                                    Sinks[Arg] = new std::set<const Value *>();
                                    Sinks[Arg]->insert(CI);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Transfer Validation
//===----------------------------------------------------------------------===//

bool InvalidUseOfStackAddressChecker::isValidTransfer(const Value * /*From*/, const Value * /*To*/) const {
    // Allow most transfers
    // GEP and bitcasts are fine - they don't escape the address
    return true;
}

//===----------------------------------------------------------------------===//
// Bug Reporting
//===----------------------------------------------------------------------===//

int InvalidUseOfStackAddressChecker::registerBugType() {
    BugReportMgr& mgr = BugReportMgr::get_instance();
    return mgr.register_bug_type(
        "Invalid Use of Stack Address",
        BugDescription::BI_HIGH,
        BugDescription::BC_SECURITY,
        "CWE-562"
    );
}

void InvalidUseOfStackAddressChecker::reportVulnerability(
    int bugTypeId, const Value *Source, const Value *Sink, 
    const std::set<const Value *> *SinkInsts) {
    
    BugReport* report = new BugReport(bugTypeId);
    
    // Add source step
    if (auto *AI = dyn_cast<AllocaInst>(Source)) {
        report->append_step(const_cast<AllocaInst*>(AI), 
                          "Stack memory allocated here");
    } else if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
        report->append_step(const_cast<Instruction*>(SourceInst), 
                          "Stack address originates here");
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
                    std::string desc = "Stack address propagates";
                    if (isa<GetElementPtrInst>(I)) {
                        desc = "Pointer arithmetic on stack address";
                    } else if (isa<LoadInst>(I)) {
                        desc = "Stack address loaded from memory";
                    } else if (isa<StoreInst>(I)) {
                        desc = "Stack address stored to memory";
                    }
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
            if (auto *RI = dyn_cast<ReturnInst>(SI)) {
                report->append_step(const_cast<ReturnInst*>(RI), 
                                  "Stack address returned (escapes scope)");
            } else if (auto *StoreI = dyn_cast<StoreInst>(SI)) {
                report->append_step(const_cast<StoreInst*>(StoreI), 
                                  "Stack address stored to global memory");
            } else if (auto *CI = dyn_cast<CallInst>(SI)) {
                report->append_step(const_cast<CallInst*>(CI), 
                                  "Stack address passed to external function (may escape)");
            } else if (auto *SinkInst = dyn_cast<Instruction>(SI)) {
                report->append_step(const_cast<Instruction*>(SinkInst), 
                                  "Stack address escapes here");
            }
        }
    }
    
    report->set_conf_score(85);
    BugReportMgr::get_instance().insert_report(bugTypeId, report);
}

//===----------------------------------------------------------------------===//
// High-Level Detection
//===----------------------------------------------------------------------===//

int InvalidUseOfStackAddressChecker::detectAndReport(
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

