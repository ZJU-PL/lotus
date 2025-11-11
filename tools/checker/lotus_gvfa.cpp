// Global Value Flow Analysis Tool
// Unified tool for vulnerability detection (null pointer, use-after-free, etc.)
// Integrates GVFA with optional NullCheckAnalysis for improved precision


#include "Apps/Checker/gvfa/NullPointerChecker.h"
#include "Apps/Checker/gvfa/UseAfterFreeChecker.h"
#include "Apps/Checker/gvfa/UseOfUninitializedVariableChecker.h"
#include "Apps/Checker/gvfa/FreeOfNonHeapMemoryChecker.h"
#include "Apps/Checker/gvfa/InvalidUseOfStackAddressChecker.h"
#include "Apps/Checker/Report/BugReportMgr.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/NullPointer/NullCheckAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode file>"), cl::Required);
static cl::opt<std::string> VulnType("vuln-type", 
    cl::desc("Vulnerability type:\n"
             "  nullpointer - Null pointer dereference\n"
             "  useafterfree - Use after free\n"
             "  uninitialized - Use of uninitialized variable\n"
             "  freenonheap - Free of non-heap memory\n"
             "  stackaddress - Invalid use of stack address"), 
    cl::init("nullpointer"));
static cl::opt<bool> UseNPA("use-npa", cl::desc("Use NullCheckAnalysis to improve precision"), cl::init(false));
static cl::opt<bool> ContextSensitive("ctx", cl::desc("Use context-sensitive analysis"), cl::init(false));
static cl::opt<bool> Verbose("verbose", cl::desc("Print detailed vulnerability information"), cl::init(false));
static cl::opt<bool> DumpStats("dump-stats", cl::desc("Dump analysis statistics"), cl::init(false));
static cl::opt<std::string> JsonOutput("json-output", cl::desc("Output JSON report to file"), cl::init(""));
static cl::opt<int> MinScore("min-score", cl::desc("Minimum confidence score for reporting"), cl::init(0));

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram X(argc, argv);
    llvm_shutdown_obj Y;
    cl::ParseCommandLineOptions(argc, argv, "Global Value Flow Analysis Tool\n");
    
    // Load module
    LLVMContext Context;
    SMDiagnostic Err;
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }
    
    outs() << "Module: " << M->getModuleIdentifier() << " (" << M->getFunctionList().size() << " functions)\n";
    
    // Run prerequisite analyses
    legacy::PassManager PM;
    auto *DyckAA = new DyckAliasAnalysis();
    auto *DyckMRA = new DyckModRefAnalysis();
    PM.add(DyckAA);
    PM.add(DyckMRA);
    
    NullCheckAnalysis *NCA = nullptr;
    ContextSensitiveNullCheckAnalysis *CSNCA = nullptr;
    if (UseNPA && VulnType == "nullpointer") {
        if (ContextSensitive) {
            PM.add(CSNCA = new ContextSensitiveNullCheckAnalysis());
        } else {
            PM.add(NCA = new NullCheckAnalysis());
        }
    }
    PM.run(*M);
    
    // Setup GVFA
    DyckVFG VFG(DyckAA, DyckMRA, M.get());
    DyckGlobalValueFlowAnalysis GVFA(M.get(), &VFG, DyckAA, DyckMRA);
    
    // Create and configure vulnerability checker
    std::unique_ptr<GVFAVulnerabilityChecker> checker;
    if (VulnType == "nullpointer") {
        auto npChecker = std::make_unique<NullPointerChecker>();
        if (NCA) npChecker->setNullCheckAnalysis(NCA);
        if (CSNCA) npChecker->setContextSensitiveNullCheckAnalysis(CSNCA);
        checker = std::move(npChecker);
    } else if (VulnType == "useafterfree") {
        checker = std::make_unique<UseAfterFreeChecker>();
    } else if (VulnType == "uninitialized") {
        checker = std::make_unique<UseOfUninitializedVariableChecker>();
    } else if (VulnType == "freenonheap") {
        checker = std::make_unique<FreeOfNonHeapMemoryChecker>();
    } else if (VulnType == "stackaddress") {
        checker = std::make_unique<InvalidUseOfStackAddressChecker>();
    } else {
        errs() << "Unknown vulnerability type: " << VulnType << "\n";
        errs() << "Available types: nullpointer, useafterfree, uninitialized, freenonheap, stackaddress\n";
        return 1;
    }
    
    // Setup GVFA and run analysis
    GVFA.setVulnerabilityChecker(std::move(checker));
    GVFA.run();
    
    // Detect and report vulnerabilities using high-level API
    auto *VChecker = GVFA.getVulnerabilityChecker();
    int vulnCount = VChecker->detectAndReport(M.get(), &GVFA, ContextSensitive, Verbose);
    
    outs() << "Found " << vulnCount << " potential vulnerabilities.\n";
    
    if (DumpStats) {
        outs() << "\n=== Statistics ===\n"
               << "Total queries: " << GVFA.AllQueryCounter << "\n"
               << "Successful queries: " << GVFA.SuccsQueryCounter << "\n";
        if (GVFA.AllQueryCounter > 0) {
            outs() << "Success rate: " << (100.0 * GVFA.SuccsQueryCounter / GVFA.AllQueryCounter) << "%\n";
        }
        GVFA.printOnlineQueryTime(outs(), "Online Query");
    }
    
    // Print bug report summary
    BugReportMgr& bugMgr = BugReportMgr::get_instance();
    outs() << "\n=== Bug Report Summary ===\n";
    bugMgr.print_summary(outs());
    
    // Generate JSON report if requested
    if (!JsonOutput.empty()) {
        std::error_code EC;
        llvm::raw_fd_ostream JsonFile(JsonOutput, EC);
        if (EC) {
            errs() << "Error opening JSON output file: " << EC.message() << "\n";
            return 1;
        }
        bugMgr.generate_json_report(JsonFile, MinScore);
        outs() << "JSON report written to: " << JsonOutput << "\n";
    }
    
    return 0;
} 