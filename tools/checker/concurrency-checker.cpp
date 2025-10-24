#include "Checker/concurrency/ConcurrencyChecker.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

using namespace llvm;
using namespace concurrency;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"), cl::Required);
static cl::opt<bool> EnableDataRaces("check-data-races", cl::desc("Enable data race detection"), cl::init(true));
static cl::opt<bool> EnableDeadlocks("check-deadlocks", cl::desc("Enable deadlock detection"), cl::init(true));
static cl::opt<bool> EnableAtomicity("check-atomicity", cl::desc("Enable atomicity violation detection"), cl::init(true));

int main(int argc, char** argv) {
    // Initialize centralized report options
    report_options::initializeReportOptions();
    
    cl::ParseCommandLineOptions(argc, argv, "Concurrency Checker Tool\n"
                                "  Use --report-json=<file> or --report-sarif=<file> for output\n");

    // Parse the input LLVM IR file
    SMDiagnostic err;
    LLVMContext context;
    std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);

    if (!module) {
        err.print(argv[0], errs());
        return 1;
    }

    outs() << "Analyzing module: " << module->getModuleIdentifier() << "\n";

    // Create the concurrency checker
    ConcurrencyChecker checker(*module);

    // Enable/disable specific checks based on command line options
    checker.enableDataRaceCheck(EnableDataRaces);
    checker.enableDeadlockCheck(EnableDeadlocks);
    checker.enableAtomicityCheck(EnableAtomicity);

    // Run the checks (bugs are automatically reported to BugReportMgr)
    outs() << "Running concurrency checks...\n";
    checker.runChecks();

    // Print analysis statistics
    auto stats = checker.getStatistics();
    outs() << "\n=== Concurrency Analysis Statistics ===\n";
    outs() << "Total Instructions: " << stats.totalInstructions << "\n";
    outs() << "MHP Pairs: " << stats.mhpPairs << "\n";
    outs() << "Locks Analyzed: " << stats.locksAnalyzed << "\n";
    outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
    outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
    outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound << "\n";

    // Print bug report summary (Clearblue pattern - applies to all checkers)
    BugReportMgr& mgr = BugReportMgr::get_instance();
    mgr.print_summary(outs());
    
    // Handle centralized output formats (applies to all checkers)
    if (!report_options::JsonOutputFile.empty()) {
        std::error_code EC;
        raw_fd_ostream json_out(report_options::JsonOutputFile, EC, sys::fs::OF_None);
        if (!EC) {
            mgr.generate_json_report(json_out, report_options::MinConfidenceScore);
            outs() << "\nJSON report written to: " << report_options::JsonOutputFile << "\n";
        } else {
            errs() << "Error writing JSON report: " << EC.message() << "\n";
        }
    }
    
    if (!report_options::SarifOutputFile.empty()) {
        // SARIF output would be implemented in BugReportMgr
        outs() << "\nNote: SARIF output support coming soon (centralized in BugReportMgr)\n";
    }
    
    int total_bugs = stats.dataRacesFound + stats.deadlocksFound + stats.atomicityViolationsFound;
    return total_bugs > 0 ? 1 : 0;
}
