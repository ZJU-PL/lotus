// Kint: A Bug-Finding Tool for C Programs (Refactored version)

#include "Checker/kint/MKintPass.h"
#include "Checker/kint/Options.h"
#include "Support/Log.h"

#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

using namespace llvm;

// Command line options
static cl::opt<std::string> InputFilename(cl::Positional, 
                                         cl::desc("<IR file>"),
                                         cl::Required);

// registering pass (new pass manager).
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo()
{
    return { LLVM_PLUGIN_API_VERSION, "MKintPass", "v0.1", [](PassBuilder& PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "mkint-pass") {
                            // do mem2reg.
                            MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
                            MPM.addPass(createModuleToFunctionPassAdaptor(SROAPass()));
                            MPM.addPass(kint::MKintPass());
                            return true;
                        }
                        return false;
                    });
            } };
}

int main(int argc, char **argv) {
    // Initialize LLVM
    llvm::InitLLVM X(argc, argv);

    // Initialize kint command line options
    kint::initializeCommandLineOptions();

    // Parse all command-line options
    llvm::cl::ParseCommandLineOptions(argc, argv, "Kint: An Integer Bug Detector\n"
                                     "  Use --check-all=true to enable all checkers at once\n"
                                     "  Use --check-<checker-name>=true to enable specific checkers\n"
                                     "  See README.checkers.md for more information\n");

    // Configure the logger
    mkint::LogConfig logConfig;
    logConfig.quiet = kint::QuietLogging;
    logConfig.useStderr = kint::StderrLogging;
    logConfig.logFile = kint::LogFile;
    
    // Convert from command-line LogLevel to mkint::LogLevel
    switch (kint::CurrentLogLevel) {
        case kint::LogLevel::DEBUG:
            logConfig.logLevel = mkint::LogLevel::DEBUG;
            break;
        case kint::LogLevel::INFO:
            logConfig.logLevel = mkint::LogLevel::INFO;
            break;
        case kint::LogLevel::WARNING:
            logConfig.logLevel = mkint::LogLevel::WARNING;
            break;
        case kint::LogLevel::ERROR:
            logConfig.logLevel = mkint::LogLevel::ERROR;
            break;
        case kint::LogLevel::NONE:
            logConfig.logLevel = mkint::LogLevel::NONE;
            logConfig.quiet = true; // Also set quiet mode for backward compatibility
            break;
    }
    
    // If quiet is set manually, override the log level
    if (kint::QuietLogging) {
        logConfig.logLevel = mkint::LogLevel::NONE;
    }
    
    mkint::Logger::getInstance().configure(logConfig);

    // Apply the CheckAll flag if set to true
    if (kint::CheckAll) {
        kint::CheckIntOverflow = true;
        kint::CheckDivByZero = true;
        kint::CheckBadShift = true;
        kint::CheckArrayOOB = true;
        kint::CheckDeadBranch = true;
    }

    // Print checker configuration
    MKINT_LOG() << "Checker Configuration:";
    MKINT_LOG() << "  Integer Overflow: " << (kint::CheckIntOverflow ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Division by Zero: " << (kint::CheckDivByZero ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Bad Shift: " << (kint::CheckBadShift ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Array Out of Bounds: " << (kint::CheckArrayOOB ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Dead Branch: " << (kint::CheckDeadBranch ? "Enabled" : "Disabled");

    // Add performance configuration information
    MKINT_LOG() << "Performance Configuration:";
    MKINT_LOG() << "  Function Timeout: " << (kint::FunctionTimeout == 0 ? "No limit" : std::to_string(kint::FunctionTimeout) + " seconds");

    // Warn if no checkers are enabled
    if (!kint::CheckIntOverflow && !kint::CheckDivByZero && !kint::CheckBadShift && !kint::CheckArrayOOB && !kint::CheckDeadBranch) {
        MKINT_WARN() << "No bug checkers are enabled. No bugs will be detected.";
        MKINT_WARN() << "Use --check-all=true or enable individual checkers with --check-<checker-name>=true";
    }

    // Load the module to analyze
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Err;
    std::unique_ptr<llvm::Module> M;

    M = llvm::parseIRFile(InputFilename, Err, Context);
    if (!M) {
        Err.print(argv[0], llvm::errs());
        return 1;
    }

    // Create and run the pass
    llvm::ModuleAnalysisManager MAM;
    llvm::ModulePassManager MPM;
    llvm::PassBuilder PB;

    PB.registerModuleAnalyses(MAM);
    MPM.addPass(kint::MKintPass());
    MPM.run(*M, MAM);

    return 0;
}