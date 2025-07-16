#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/IR/LegacyPassManager.h>

#include <string>
#include <memory>

#include "Dataflow/GVFA/GlobalValueFlowAnalysis.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Alias/DyckAA/DyckValueFlowAnalysis.h"

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional,
                                         cl::desc("<input bitcode file>"),
                                         cl::Required);

static cl::opt<std::string> VulnType("vuln-type", 
                                     cl::desc("Vulnerability type (nullpointer, taint)"),
                                     cl::init("nullpointer"));

static cl::opt<bool> TestCFLReachability("test-cfl-reachability",
                                        cl::desc("Test CFL reachability queries"),
                                        cl::init(false));

static cl::opt<bool> DumpStats("dump-stats",
                              cl::desc("Dump analysis statistics"),
                              cl::init(false));

static cl::opt<bool> Verbose("verbose",
                           cl::desc("Print detailed vulnerability information"),
                           cl::init(false));

int main(int argc, char **argv) {
    // Initialize LLVM
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram X(argc, argv);
    llvm_shutdown_obj Y;
    
    cl::ParseCommandLineOptions(argc, argv, "Dyck Global Value Flow Analysis Tool\n");
    
    // Load the module
    LLVMContext Context;
    SMDiagnostic Err;
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }
    
    outs() << "Loaded module: " << M->getModuleIdentifier() 
           << " (" << M->getFunctionList().size() << " functions)\n";
    
    // Run analysis passes
    legacy::PassManager PM;
    auto *DyckAAPass = new DyckAliasAnalysis();
    auto *DyckMRAPass = new DyckModRefAnalysis();
    PM.add(DyckAAPass);
    PM.add(DyckMRAPass);
    PM.run(*M);
    
    // Create VFG and GVFA
    DyckVFG VFG(DyckAAPass, DyckMRAPass, M.get());
    DyckGlobalValueFlowAnalysis GVFA(M.get(), &VFG, DyckAAPass, DyckMRAPass);
    
    // Set vulnerability checker
    std::unique_ptr<VulnerabilityChecker> checker;
    if (VulnType == "nullpointer") {
        checker = std::make_unique<NullPointerVulnerabilityChecker>();
    } else if (VulnType == "taint") {
        checker = std::make_unique<TaintVulnerabilityChecker>();
    } else {
        errs() << "Unknown vulnerability type: " << VulnType << "\n";
        return 1;
    }
    
    GVFA.setVulnerabilityChecker(std::move(checker));
    GVFA.run();
    
    // Get sources and sinks
    VulnerabilitySourcesType Sources;
    VulnerabilitySinksType Sinks;
    GVFA.getVulnerabilityChecker()->getSources(M.get(), Sources);
    GVFA.getVulnerabilityChecker()->getSinks(M.get(), Sinks);
    
    // Perform vulnerability detection
    int vulnerabilitiesFound = 0;
    
    for (const auto &SinkPair : Sinks) {
        const Value *SinkValue = SinkPair.first;
        const std::set<const Value *> *SinkInsts = SinkPair.second;
        
        for (const auto &SourcePair : Sources) {
            const Value *SourceValue = SourcePair.first.first;
            int SourceMask = SourcePair.second;
            
            bool isReachable = false;
            if (TestCFLReachability) {
                // Use context-sensitive reachability when CFL testing is enabled
                isReachable = GVFA.contextSensitiveReachable(SourceValue, SinkValue);
            } else {
                // Use regular reachability
                isReachable = GVFA.reachable(SinkValue, SourceMask);
            }
            
            if (isReachable) {
                vulnerabilitiesFound++;
                if (Verbose) {
                    outs() << "VULNERABILITY: " << GVFA.getVulnerabilityChecker()->getCategory() 
                           << " detected!\n  Source: ";
                    SourceValue->print(outs());
                    outs() << "\n  Sink: ";
                    SinkValue->print(outs());
                    outs() << "\n";
                    
                    for (const Value *SinkInst : *SinkInsts) {
                        outs() << "  At: ";
                        SinkInst->print(outs());
                        outs() << "\n";
                    }
                    outs() << "\n";
                }
            }
        }
    }
    
    outs() << "Found " << vulnerabilitiesFound << " potential vulnerabilities.\n";
    
    // Print statistics if requested
    if (DumpStats) {
        outs() << "\n=== Statistics ===\n"
               << "Total queries: " << GVFA.AllQueryCounter << "\n"
               << "Successful queries: " << GVFA.SuccsQueryCounter << "\n";
        if (GVFA.AllQueryCounter > 0) {
            double successRate = (double)GVFA.SuccsQueryCounter / GVFA.AllQueryCounter * 100.0;
            outs() << "Success rate: " << successRate << "%\n";
        }
        GVFA.printOnlineQueryTime(outs(), "Online Query");
    }
    
    return 0;
} 