/**
 * @file NullPointerChecker.cpp
 * @brief Implementation of null pointer dereference vulnerability checker
 */

#include "Checker/GVFA/NullPointerChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugTypes.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/NullPointer/NullCheckAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"

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

/**
 * Check if a library function dereferences its arguments.
 * Returns true if the specified argument index is dereferenced by the function.
 */
bool libraryFunctionDereferencesArg(StringRef Name, unsigned ArgIdx) {
    // Memory operations - dereference destination (arg 0) and sometimes source (arg 1)
    if (Name == "memcpy" || Name.startswith("__memcpy_chk") ||
        Name == "memmove" || Name.startswith("__memmove_chk")) {
        return ArgIdx == 0 || ArgIdx == 1;  // Both src and dst are dereferenced
    }
    if (Name == "memset" || Name.startswith("__memset_chk")) {
        return ArgIdx == 0;  // Destination is dereferenced
    }
    
    // String operations - dereference string arguments
    if (Name == "strcpy" || Name.startswith("__strcpy_chk") ||
        Name == "strncpy" || Name.startswith("__strncpy_chk")) {
        return ArgIdx == 0 || ArgIdx == 1;  // Both destination and source
    }
    if (Name == "strcat" || Name.startswith("__strcat_chk") ||
        Name == "strncat" || Name.startswith("__strncat_chk")) {
        return ArgIdx == 0 || ArgIdx == 1;  // Both destination and source
    }
    if (Name == "strcmp" || Name == "strncmp" ||
        Name == "strlen" || Name == "strnlen") {
        return ArgIdx < 2;  // First one or two arguments
    }
    
    // Character search functions
    if (Name == "strchr" || Name == "strrchr" || Name == "strstr") {
        return ArgIdx == 0;  // First argument is dereferenced
    }
    
    return false;
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void NullPointerChecker::getSources(Module *M, VulnerabilitySourcesType &Sources) {
    // Find null pointer sources:
    // 1. NULL constants stored to variables
    // 2. Memory allocation functions (can return NULL on failure)
    
    forEachInstruction(M, [&Sources](const Instruction *I) {
        // NULL constants stored to variables
        if (auto *SI = dyn_cast<StoreInst>(I)) {
            if (isa<ConstantPointerNull>(SI->getValueOperand())) {
                Sources[{SI, 1}] = 1;
            }
        }
        // Memory allocation functions (can fail and return NULL)
        else if (auto *Call = dyn_cast<CallInst>(I)) {
            if (auto *CalledF = Call->getCalledFunction()) {
                StringRef Name = CalledF->getName();
                
                // Standard C allocation functions
                if (Name == "malloc" || Name == "calloc" || 
                    Name == "realloc" || Name == "reallocf") {
                    Sources[{Call, 1}] = 1;
                }
                // C++ new operators (can return NULL with nothrow)
                else if (Name == "_Znwm" || Name == "_Znam" ||           // new, new[]
                         Name == "_ZnwmRKSt9nothrow_t" ||                // new(nothrow)
                         Name == "_ZnamRKSt9nothrow_t") {                // new[](nothrow)
                    Sources[{Call, 1}] = 1;
                }
            }
        }
    });
    
    llvm::outs() << "Found " << Sources.size() << " null pointer sources\n";
}

void NullPointerChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
    int filteredCount = 0;
    int totalCount = 0;
    int libraryFuncCount = 0;
    
    // Helper lambda to add a sink
    auto addSink = [&](const Value *PtrOp, const Instruction *I) {
        if (!PtrOp || !PtrOp->getType()->isPointerTy()) {
            return;
        }
        
        totalCount++;
        
        // Filter out proven non-null pointers if NullCheckAnalysis is available
        if (isProvenNonNull(PtrOp, I)) {
            filteredCount++;
            return;
        }
        
        if (Sinks.find(PtrOp) == Sinks.end()) {
            Sinks[PtrOp] = new std::set<const Value *>();
        }
        Sinks[PtrOp]->insert(I);
    };
    
    // Find null pointer sinks:
    // 1. Direct dereferences (load, store, GEP)
    // 2. Library function calls that dereference arguments
    
    forEachInstruction(M, [&](const Instruction *I) {
        // Direct dereferences
        if (auto *LI = dyn_cast<LoadInst>(I)) {
            addSink(LI->getPointerOperand(), I);
        } 
        else if (auto *SI = dyn_cast<StoreInst>(I)) {
            addSink(SI->getPointerOperand(), I);
        } 
        else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
            addSink(GEP->getPointerOperand(), I);
        }
        // Library function calls that dereference their arguments
        else if (auto *Call = dyn_cast<CallInst>(I)) {
            if (auto *F = Call->getCalledFunction()) {
                StringRef Name = F->getName();
                
                // Check each argument to see if it's dereferenced by this function
                for (unsigned i = 0; i < Call->arg_size(); i++) {
                    if (libraryFunctionDereferencesArg(Name, i)) {
                        Value *Arg = Call->getArgOperand(i);
                        if (Arg->getType()->isPointerTy()) {
                            libraryFuncCount++;
                            addSink(Arg, I);
                        }
                    }
                }
            }
        }
    });
    
    llvm::outs() << "Found " << totalCount << " sinks total "
                 << "(including " << libraryFuncCount << " library function arguments)\n";
    
    if (NCA || CSNCA) {
        llvm::outs() << "NullCheckAnalysis filtered out " << filteredCount 
                     << " / " << totalCount << " proven non-null sinks\n";
    }
}

//===----------------------------------------------------------------------===//
// Transfer Validation
//===----------------------------------------------------------------------===//

bool NullPointerChecker::isValidTransfer(const Value * /*From*/, const Value *To) const {
    // Allow flow through most instructions except sanitizers
    if (auto *CI = dyn_cast<CallInst>(To)) {
        if (auto *F = CI->getCalledFunction()) {
            StringRef Name = F->getName();
            // Block flow through null check functions
            return !Name.contains("check") && !Name.contains("assert");
        }
    }
    return true;
}

bool NullPointerChecker::isProvenNonNull(const Value *Ptr, const Instruction *Inst) const {
    // If no NullCheckAnalysis available, cannot prove non-null
    if (!NCA && !CSNCA) {
        return false;
    }
    
    // Cast away const to call mayNull (NullCheckAnalysis interface requires non-const)
    auto *MutablePtr = const_cast<Value *>(Ptr);
    auto *MutableInst = const_cast<Instruction *>(Inst);
    
    if (NCA) {
        // mayNull returns true if it MAY be null
        // So if it returns false, the pointer is proven NOT null
        return !NCA->mayNull(MutablePtr, MutableInst);
    }
    
    // For context-sensitive analysis, we would need context information
    // For now, use context-insensitive query (conservative)
    if (CSNCA) {
        // Use empty context for now (conservative approximation)
        Context emptyCtx;
        return !CSNCA->mayNull(MutablePtr, MutableInst, emptyCtx);
    }
    
    return false;
}

//===----------------------------------------------------------------------===//
// Bug Reporting
//===----------------------------------------------------------------------===//

int NullPointerChecker::registerBugType() {
    BugReportMgr& mgr = BugReportMgr::get_instance();
    return mgr.register_bug_type(
        "NULL Pointer Dereference",
        BugDescription::BI_HIGH,
        BugDescription::BC_SECURITY,
        "CWE-476, CWE-690"
    );
}

void NullPointerChecker::reportVulnerability(
    int bugTypeId, const Value *Source, const Value *Sink, 
    const std::set<const Value *> *SinkInsts) {
    
    BugReport* report = new BugReport(bugTypeId);
    
    // Add source step (where null value originates)
    if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
        report->append_step(const_cast<Instruction*>(SourceInst), 
                           "Null value originates here");
    } else {
        // For constants or non-instructions, add a descriptive step
        std::string sourceDesc = "Null value source: ";
        llvm::raw_string_ostream OS(sourceDesc);
        Source->print(OS);
        // Create a dummy step with the first sink instruction
        if (SinkInsts && !SinkInsts->empty()) {
            if (auto *FirstSinkInst = dyn_cast<Instruction>(*SinkInsts->begin())) {
                report->append_step(const_cast<Instruction*>(FirstSinkInst), OS.str());
            }
        }
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
                        desc = "Null value stored to memory";
                    } else if (isa<LoadInst>(I)) {
                        desc = "Potentially null value loaded from memory";
                    } else if (isa<CallInst>(I)) {
                        desc = "Potentially null value passed in function call";
                    } else if (isa<ReturnInst>(I)) {
                        desc = "Potentially null value returned";
                    } else if (isa<PHINode>(I)) {
                        desc = "Potentially null value from control flow merge";
                    } else if (isa<GetElementPtrInst>(I)) {
                        desc = "Pointer arithmetic on potentially null value";
                    } else {
                        desc = "Value propagates through here";
                    }
                    
                    report->append_step(const_cast<Instruction*>(I), desc);
                }
            }
        } catch (...) {
            // If witness path extraction fails, continue without it
        }
    }
    
    // Add sink steps (where null pointer is dereferenced)
    if (SinkInsts) {
        for (const Value *SI : *SinkInsts) {
            if (auto *SinkInst = dyn_cast<Instruction>(SI)) {
                std::string sinkDesc = "Potential null pointer dereference";
                
                // Make the message more specific based on instruction type
                if (isa<LoadInst>(SinkInst)) {
                    sinkDesc = "Load from potentially null pointer";
                } else if (isa<StoreInst>(SinkInst)) {
                    sinkDesc = "Store to potentially null pointer";
                } else if (isa<GetElementPtrInst>(SinkInst)) {
                    sinkDesc = "GEP on potentially null pointer";
                }
                
                report->append_step(const_cast<Instruction*>(SinkInst), sinkDesc);
            }
        }
    }
    
    // Set confidence score based on whether NullCheckAnalysis is used
    int confidence = (NCA || CSNCA) ? 85 : 70;
    report->set_conf_score(confidence);
    
    // Insert into BugReportMgr
    BugReportMgr::get_instance().insert_report(bugTypeId, report);
}

//===----------------------------------------------------------------------===//
// High-Level Detection
//===----------------------------------------------------------------------===//

int NullPointerChecker::detectAndReport(
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

