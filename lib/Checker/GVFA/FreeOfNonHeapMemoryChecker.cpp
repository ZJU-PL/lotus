/**
 * @file FreeOfNonHeapMemoryChecker.cpp
 * @brief Implementation of free-of-non-heap-memory checker
 */

#include "Checker/GVFA/FreeOfNonHeapMemoryChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugTypes.h"

#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

namespace {

bool isFreeCall(const CallInst *CI) {
    if (!CI) return false;
    auto *Callee = CI->getCalledFunction();
    if (!Callee) return false;
    StringRef Name = Callee->getName();
    return Name == "free" || Name == "cfree" || Name == "_ZdlPv" || Name == "_ZdaPv";
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void FreeOfNonHeapMemoryChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find non-heap memory sources (stack allocations and globals)
    
    // Stack allocations
    for (auto &F : *M) {
        if (F.isDeclaration()) continue;
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *AI = dyn_cast<AllocaInst>(&I)) {
                    Sources[{AI, 1}] = 1;
                }
            }
        }
    }
    
    // Global variables
    for (auto &GV : M->globals()) {
        Sources[{&GV, 1}] = 1;
    }
}

void FreeOfNonHeapMemoryChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    // Find free() call sites
    for (auto &F : *M) {
        if (F.isDeclaration()) continue;
        
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (isFreeCall(CI) && CI->arg_size() > 0) {
                        const Value *PtrArg = CI->getArgOperand(0);
                        Sinks[PtrArg] = new std::set<const Value *>();
                        Sinks[PtrArg]->insert(CI);
                    }
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Transfer Validation
//===----------------------------------------------------------------------===//

bool FreeOfNonHeapMemoryChecker::isValidTransfer(const Value * /*From*/, const Value *To) const {
    // Allow most transfers, but block through heap allocation wrappers
    if (auto *CI = dyn_cast<CallInst>(To)) {
        if (auto *F = CI->getCalledFunction()) {
            StringRef Name = F->getName();
            // Block flow through allocation functions (they would sanitize)
            if (Name == "malloc" || Name == "calloc" || Name == "realloc" ||
                Name.startswith("_Zn") || Name.startswith("_Zna")) {
                return false;
            }
        }
    }
    return true;
}

//===----------------------------------------------------------------------===//
// Bug Reporting
//===----------------------------------------------------------------------===//

int FreeOfNonHeapMemoryChecker::registerBugType() {
    BugReportMgr& mgr = BugReportMgr::get_instance();
    return mgr.register_bug_type(
        "Free of Memory Not on the Heap",
        BugDescription::BI_HIGH,
        BugDescription::BC_SECURITY,
        "CWE-590"
    );
}

void FreeOfNonHeapMemoryChecker::reportVulnerability(
    int bugTypeId, const Value *Source, const Value *Sink, 
    const std::set<const Value *> *SinkInsts) {
    
    BugReport* report = new BugReport(bugTypeId);
    
    // Add source step
    if (auto *AI = dyn_cast<AllocaInst>(Source)) {
        report->append_step(const_cast<AllocaInst*>(AI), 
                          "Stack memory allocated here");
    } else if (auto *GV = dyn_cast<GlobalVariable>(Source)) {
        // For globals, use the first sink instruction as location
        if (SinkInsts && !SinkInsts->empty()) {
            if (auto *FirstSink = dyn_cast<Instruction>(*SinkInsts->begin())) {
                std::string desc = "Global variable '";
                desc += GV->getName().str();
                desc += "' is not on the heap";
                report->append_step(const_cast<Instruction*>(FirstSink), desc);
            }
        }
    } else if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
        report->append_step(const_cast<Instruction*>(SourceInst), 
                          "Non-heap memory originates here");
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
                    std::string desc = "Non-heap pointer propagates";
                    if (isa<GetElementPtrInst>(I)) {
                        desc = "Pointer arithmetic on non-heap memory";
                    } else if (isa<LoadInst>(I)) {
                        desc = "Non-heap pointer loaded from memory";
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
            if (auto *CI = dyn_cast<CallInst>(SI)) {
                report->append_step(const_cast<CallInst*>(CI), 
                                  "Attempt to free non-heap memory");
            }
        }
    }
    
    report->set_conf_score(90);
    BugReportMgr::get_instance().insert_report(bugTypeId, report);
}

//===----------------------------------------------------------------------===//
// High-Level Detection
//===----------------------------------------------------------------------===//

int FreeOfNonHeapMemoryChecker::detectAndReport(
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

