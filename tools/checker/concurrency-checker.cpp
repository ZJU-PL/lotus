#include "Checker/concurrency/ConcurrencyChecker.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

//#include <iostream>
#include <string>

using namespace llvm;
using namespace concurrency;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"), cl::Required);
static cl::opt<bool> EnableDataRaces("check-data-races", cl::desc("Enable data race detection"), cl::init(true));
static cl::opt<bool> EnableDeadlocks("check-deadlocks", cl::desc("Enable deadlock detection"), cl::init(true));
static cl::opt<bool> EnableAtomicity("check-atomicity", cl::desc("Enable atomicity violation detection"), cl::init(true));

int main(int argc, char** argv) {
    cl::ParseCommandLineOptions(argc, argv, "Concurrency Checker Tool\n");

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

    // Run the checks
    outs() << "Running concurrency checks...\n";
    auto reports = checker.runChecks();

    // Print results
    auto stats = checker.getStatistics();
    outs() << "\n=== Concurrency Analysis Statistics ===\n";
    outs() << "Total Instructions: " << stats.totalInstructions << "\n";
    outs() << "MHP Pairs: " << stats.mhpPairs << "\n";
    outs() << "Locks Analyzed: " << stats.locksAnalyzed << "\n";
    outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
    outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
    outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound << "\n";

    if (reports.empty()) {
        outs() << "\n✅ No concurrency issues found!\n";
        return 0;
    }

    outs() << "\n=== Concurrency Issues Found ===\n";

    for (size_t i = 0; i < reports.size(); ++i) {
        const auto& report = reports[i];

        outs() << "\n" << (i + 1) << ". ";

        switch (report.bugType) {
            case ConcurrencyBugType::DATA_RACE:
                outs() << "🚨 DATA RACE";
                break;
            case ConcurrencyBugType::DEADLOCK:
                outs() << "🚨 DEADLOCK";
                break;
            case ConcurrencyBugType::ATOMICITY_VIOLATION:
                outs() << "⚠️  ATOMICITY VIOLATION";
                break;
        }

        outs() << "\n   " << report.description << "\n";

        if (report.instruction1) {
            outs() << "   Location 1: ";
            report.instruction1->print(outs());
            outs() << "\n";
        }

        if (report.instruction2) {
            outs() << "   Location 2: ";
            report.instruction2->print(outs());
            outs() << "\n";
        }

        outs() << "   Severity: ";
        switch (report.importance) {
            case BugDescription::BI_LOW: outs() << "Low"; break;
            case BugDescription::BI_MEDIUM: outs() << "Medium"; break;
            case BugDescription::BI_HIGH: outs() << "High"; break;
            default: outs() << "Unknown"; break;
        }
        outs() << "\n";
    }

    return reports.empty() ? 0 : 1;
}
