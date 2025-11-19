/**
 * @file UseAfterFreeChecker.cpp
 * @brief Implementation of use-after-free vulnerability checker
 */

#include "Checker/GVFA/UseAfterFreeChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugTypes.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

namespace {

/**
 * Helper function to iterate through all instructions in a module.
 */
template<typename Func>
void forEachInstruction(Module *M, Func func) {
    for (auto &F : *M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                func(&I);
            }
        }
    }
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void UseAfterFreeChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find free/delete operations
    forEachInstruction(M, [&Sources](const Instruction *I) {
        if (auto *Call = dyn_cast<CallInst>(I)) {
            if (auto *CalledF = Call->getCalledFunction()) {
                StringRef Name = CalledF->getName();
                // Common deallocation functions
                if (Name == "free" || Name == "delete" || Name == "_ZdlPv" || 
                    Name == "_ZdaPv" || Name == "kfree") {
                    // Mark the freed pointer (first argument) as a source
                    if (Call->arg_size() > 0) {
                        auto *Arg = Call->getArgOperand(0);
                        Sources[{Arg, 1}] = 1;
                    }
                }
            }
        }
    });
}

void UseAfterFreeChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    // Find memory accesses that could dereference freed pointers
    forEachInstruction(M, [&Sinks](const Instruction *I) {
        const Value *PtrOp = nullptr;
        if (auto *LI = dyn_cast<LoadInst>(I)) {
            PtrOp = LI->getPointerOperand();
        } else if (auto *SI = dyn_cast<StoreInst>(I)) {
            PtrOp = SI->getPointerOperand();
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
            PtrOp = GEP->getPointerOperand();
        } else if (auto *Call = dyn_cast<CallInst>(I)) {
            // Also consider function calls that may dereference pointers
            if (auto *CalledF = Call->getCalledFunction()) {
                StringRef Name = CalledF->getName();
                // Common functions that dereference pointers
                if (Name.contains("memcpy") || Name.contains("memset") || 
                    Name.contains("strcpy") || Name.contains("strcmp")) {
                    for (unsigned i = 0; i < Call->arg_size(); ++i) {
                        auto *Arg = Call->getArgOperand(i);
                        if (Arg->getType()->isPointerTy()) {
                            Sinks[Arg] = new std::set<const Value *>();
                            Sinks[Arg]->insert(Call);
                        }
                    }
                }
            }
        }
        
        if (PtrOp) {
            Sinks[PtrOp] = new std::set<const Value *>();
            Sinks[PtrOp]->insert(I);
        }
    });
}

//===----------------------------------------------------------------------===//
// Transfer Validation
//===----------------------------------------------------------------------===//

bool UseAfterFreeChecker::isValidTransfer(const Value * /*From*/, const Value *To) const {
    // Allow flow through most instructions
    // We could potentially block flow through reallocation functions
    if (auto *CI = dyn_cast<CallInst>(To)) {
        if (auto *F = CI->getCalledFunction()) {
            StringRef Name = F->getName();
            // Block flow through reallocation - the pointer becomes valid again
            if (Name == "realloc" || Name == "malloc" || Name == "calloc") {
                return false;
            }
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Bug Reporting
//===----------------------------------------------------------------------===//

int UseAfterFreeChecker::registerBugType() {
    BugReportMgr& mgr = BugReportMgr::get_instance();
    return mgr.register_bug_type(
        "Use After Free",
        BugDescription::BI_HIGH,
        BugDescription::BC_SECURITY,
        "CWE-416"
    );
}

void UseAfterFreeChecker::reportVulnerability(
    int bugTypeId, const Value *Source, const Value *Sink, 
    const std::set<const Value *> *SinkInsts) {
    
    BugReport* report = new BugReport(bugTypeId);
    
    // Add source step (where memory is freed)
    if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
        report->append_step(const_cast<Instruction*>(SourceInst), 
                           "Memory freed here");
    }
    
    // Add intermediate propagation steps if GVFA is available
    if (GVFA && Sink) {
        try {
            std::vector<const Value *> witnessPath = GVFA->getWitnessPath(Source, Sink);
            
            // Add intermediate steps (skip first which is source, and last which is sink)
            if (witnessPath.size() > 2) {
                for (size_t i = 1; i + 1 < witnessPath.size(); ++i) {
                    const Value *V = witnessPath[i];
                    
                    // Check for ellipsis marker (nullptr)
                    if (!V) {
                        continue;
                    }
                    
                    const Instruction *I = dyn_cast<Instruction>(V);
                    if (!I) {
                        continue;
                    }
                    
                    std::string desc;
                    if (isa<StoreInst>(I)) {
                        desc = "Freed pointer stored to memory";
                    } else if (isa<LoadInst>(I)) {
                        desc = "Freed pointer loaded from memory";
                    } else if (isa<CallInst>(I)) {
                        desc = "Freed pointer passed in function call";
                    } else if (isa<ReturnInst>(I)) {
                        desc = "Freed pointer returned";
                    } else if (isa<PHINode>(I)) {
                        desc = "Freed pointer from control flow merge";
                    } else if (isa<GetElementPtrInst>(I)) {
                        desc = "Pointer arithmetic on freed pointer";
                    } else {
                        desc = "Freed pointer propagates through here";
                    }
                    
                    report->append_step(const_cast<Instruction*>(I), desc);
                }
            }
        } catch (...) {
            // If witness path extraction fails, continue without it
        }
    }
    
    // Add sink steps (where freed memory is accessed)
    if (SinkInsts) {
        for (const Value *SI : *SinkInsts) {
            if (auto *SinkInst = dyn_cast<Instruction>(SI)) {
                std::string sinkDesc = "Use of freed memory";
                
                // Make the message more specific based on instruction type
                if (isa<LoadInst>(SinkInst)) {
                    sinkDesc = "Load from freed memory";
                } else if (isa<StoreInst>(SinkInst)) {
                    sinkDesc = "Store to freed memory";
                } else if (isa<GetElementPtrInst>(SinkInst)) {
                    sinkDesc = "GEP on freed memory";
                } else if (isa<CallInst>(SinkInst)) {
                    sinkDesc = "Function call with freed memory";
                }
                
                report->append_step(const_cast<Instruction*>(SinkInst), sinkDesc);
            }
        }
    }
    
    // Set confidence score
    report->set_conf_score(75);
    
    // Insert into BugReportMgr
    BugReportMgr::get_instance().insert_report(bugTypeId, report);
}

//===----------------------------------------------------------------------===//
// High-Level Detection
//===----------------------------------------------------------------------===//

int UseAfterFreeChecker::detectAndReport(
    Module *M, DyckGlobalValueFlowAnalysis *GVFA,
    bool contextSensitive, bool verbose) {
    
    // Store GVFA for use in reportVulnerability
    this->GVFA = GVFA;
    
    // Register bug type
    int bugTypeId = registerBugType();
    
    // Collect sources and sinks
    VulnerabilitySourcesType Sources;
    VulnerabilitySinksType Sinks;
    getSources(M, Sources);
    getSinks(M, Sinks);
    
    int vulnCount = 0;
    
    // Check reachability for each source-sink pair
    for (const auto &SinkPair : Sinks) {
        const Value *SinkValue = SinkPair.first;
        const std::set<const Value *> *SinkInsts = SinkPair.second;
        
        for (const auto &SourceEntry : Sources) {
            const Value *SourceValue = SourceEntry.first.first;
            int SourceMask = SourceEntry.second;
            
            // Check reachability
            bool reachable = contextSensitive 
                ? GVFA->contextSensitiveReachable(SourceValue, SinkValue)
                : GVFA->reachable(SinkValue, SourceMask);
            
            if (reachable) {
                vulnCount++;
                
                // Report to BugReportMgr
                reportVulnerability(bugTypeId, SourceValue, SinkValue, SinkInsts);
                
                // Print verbose output if requested
                if (verbose) {
                    llvm::outs() << "VULNERABILITY: " << getCategory() << "\n  Source: ";
                    SourceValue->print(llvm::outs());
                    llvm::outs() << "\n  Sink: ";
                    SinkValue->print(llvm::outs());
                    llvm::outs() << "\n";
                    for (const Value *SI : *SinkInsts) {
                        llvm::outs() << "    At: ";
                        SI->print(llvm::outs());
                        llvm::outs() << "\n";
                    }
                    llvm::outs() << "\n";
                }
            }
        }
    }
    
    return vulnCount;
}

