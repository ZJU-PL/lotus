/**
 * @file llvm-ai.cpp
 * @brief LLVM IFDS/IDE Analysis Tool
 * 
 * A command-line tool for running IFDS/IDE interprocedural dataflow analysis 
 * on LLVM bitcode files using the Sparta framework.
 */

#include <Analysis/IFDS/IFDSFramework.h>
#include <Analysis/IFDS/IFDSSolvers.h>
#include <Analysis/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Alias/AliasAnalysisWrapper.h>

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

//#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>

using namespace llvm;
using namespace ifds;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"), cl::init(false));

static cl::opt<int> AnalysisType("analysis", cl::desc("Type of analysis to run: 0=taint"),
                                 cl::init(0));

static cl::opt<std::string> AliasAnalysisType("aa", 
    cl::desc("Alias analysis type: andersen, dyck, cfl-anders, cfl-steens, seadsa, allocaa, basic, combined (default: dyck)"),
    cl::init("dyck"));

static cl::opt<bool> ShowResults("show-results", cl::desc("Show detailed analysis results"), 
                                 cl::init(true));

static cl::opt<int> MaxDetailedResults("max-results", cl::desc("Maximum number of detailed results to show"), 
                                      cl::init(10));

static cl::opt<std::string> SourceFunctions("sources", cl::desc("Comma-separated list of source functions"),
                                            cl::init(""));

static cl::opt<std::string> SinkFunctions("sinks", cl::desc("Comma-separated list of sink functions"),
                                          cl::init(""));

static cl::opt<bool> EnableParallel("parallel", cl::desc("Enable parallel IFDS processing"),
                                   cl::init(true));

static cl::opt<unsigned> NumThreads("threads", cl::desc("Number of threads for parallel processing"),
                                   cl::init(std::thread::hardware_concurrency()));

static cl::opt<unsigned> WorklistBatchSize("batch-size", cl::desc("Worklist batch size for load balancing"),
                                          cl::init(100));


static cl::opt<unsigned> SyncFrequency("sync-freq", cl::desc("Synchronization frequency (edges between syncs)"),
                                      cl::init(1000));

static cl::opt<bool> PrintStats("print-stats", cl::desc("Print LLVM statistics"),
                                cl::init(false));

// Helper function to parse comma-separated function names
std::vector<std::string> parseFunctionList(const std::string& input) {
    std::vector<std::string> functions;
    if (input.empty()) return functions;
    
    std::stringstream ss(input);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            functions.push_back(item);
        }
    }
    return functions;
}

// Helper function to parse alias analysis type from string
lotus::AAType parseAliasAnalysisType(const std::string& aaTypeStr) {
    std::string lowerStr = aaTypeStr;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);
    
    if (lowerStr == "andersen") return lotus::AAType::Andersen;
    if (lowerStr == "dyck" || lowerStr == "dyckaa") return lotus::AAType::DyckAA;
    if (lowerStr == "cfl-anders" || lowerStr == "cflanders") return lotus::AAType::CFLAnders;
    if (lowerStr == "cfl-steens" || lowerStr == "cflsteens") return lotus::AAType::CFLSteens;
    if (lowerStr == "seadsa") return lotus::AAType::SeaDsa;
    if (lowerStr == "allocaa" || lowerStr == "alloc") return lotus::AAType::AllocAA;
    if (lowerStr == "basic" || lowerStr == "basicaa") return lotus::AAType::BasicAA;
    if (lowerStr == "tbaa") return lotus::AAType::TBAA;
    if (lowerStr == "globals" || lowerStr == "globalsaa") return lotus::AAType::GlobalsAA;
    if (lowerStr == "scevaa" || lowerStr == "scev") return lotus::AAType::SCEVAA;
    if (lowerStr == "sraa") return lotus::AAType::SRAA;
    if (lowerStr == "combined") return lotus::AAType::Combined;
    if (lowerStr == "underapprox") return lotus::AAType::UnderApprox;
    
    // Default to DyckAA
    errs() << "Warning: Unknown alias analysis type '" << aaTypeStr 
           << "', defaulting to DyckAA\n";
    return lotus::AAType::DyckAA;
}

int main(int argc, char **argv) {
    InitLLVM X(argc, argv);
    
    cl::ParseCommandLineOptions(argc, argv, "LLVM IFDS/IDE Analysis Tool\n");
    
    // Enable statistics collection if requested
    if (PrintStats) {
        llvm::EnableStatistics();
    }
    
    // Set up LLVM context and source manager
    LLVMContext Context;
    SMDiagnostic Err;
    
    // Load the input module
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }
    
    if (Verbose) {
        outs() << "Loaded module: " << M->getName() << "\n";
        outs() << "Functions in module: " << M->size() << "\n";
    }
    
    // Set up alias analysis wrapper
    lotus::AAType aaType = parseAliasAnalysisType(AliasAnalysisType.getValue());
    auto aliasWrapper = std::make_unique<lotus::AliasAnalysisWrapper>(*M, aaType);
    
    if (Verbose) {
        outs() << "Using alias analysis: " << lotus::AliasAnalysisFactory::getTypeName(aaType) << "\n";
    }
    
    if (!aliasWrapper->isInitialized()) {
        errs() << "Warning: Alias analysis failed to initialize properly\n";
    }
    
    // Run the selected analysis
    try {
        switch (AnalysisType.getValue()) {
            case 0: { // Taint analysis
                outs() << "Running interprocedural taint analysis...\n";

                TaintAnalysis taintAnalysis;

                // Set up custom sources and sinks if provided
                auto sources = parseFunctionList(SourceFunctions);
                auto sinks = parseFunctionList(SinkFunctions);

                for (const auto& source : sources) {
                    taintAnalysis.add_source_function(source);
                }
                for (const auto& sink : sinks) {
                    taintAnalysis.add_sink_function(sink);
                }

                // Set up alias analysis
                taintAnalysis.set_alias_analysis(aliasWrapper.get());

                auto analysisStart = std::chrono::high_resolution_clock::now();

                if (EnableParallel) {
                    outs() << "Using parallel IFDS solver with " << NumThreads << " threads\n";

                    // Configure parallel solver
                    ifds::ParallelIFDSConfig config;
                    config.num_threads = NumThreads;
                    config.enable_parallel_processing = true;
                    config.parallel_mode = ifds::ParallelIFDSConfig::ParallelMode::WORKLIST_PARALLELISM;
                    config.worklist_batch_size = WorklistBatchSize;
                    config.sync_frequency = SyncFrequency;

                    ifds::ParallelIFDSSolver<ifds::TaintAnalysis> solver(taintAnalysis, config);

                    // Enable progress bar when running in verbose mode
                    if (Verbose) {
                        solver.set_show_progress(true);
                    }

                    solver.solve(*M);

                    auto analysisEnd = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        analysisEnd - analysisStart);

                    // Report performance statistics
                    auto stats = solver.get_performance_stats();
                    outs() << "\n=== Parallel Analysis Performance ===\n";
                    outs() << "Total time: " << duration.count() << " ms\n";
                    outs() << "Edges processed: " << stats.total_edges_processed << "\n";
                    outs() << "Path edges discovered: " << stats.total_path_edges << "\n";
                    outs() << "Summary edges discovered: " << stats.total_summary_edges << "\n";
                    outs() << "Average edges/second: " << (int)stats.average_edges_per_second << "\n";
                    outs() << "Max worklist size: " << stats.max_worklist_size << "\n";

                    if (ShowResults) {
                        taintAnalysis.report_vulnerabilities(solver, outs(), MaxDetailedResults.getValue());
                    }

                } else {
                    outs() << "Using sequential IFDS solver\n";

                    ifds::IFDSSolver<ifds::TaintAnalysis> solver(taintAnalysis);

                    // Enable progress bar when running in verbose mode
                    if (Verbose) {
                        solver.set_show_progress(true);
                    }

                    solver.solve(*M);

                    auto analysisEnd = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        analysisEnd - analysisStart);

                    outs() << "Sequential analysis completed in " << duration.count() << " ms\n";

                    if (ShowResults) {
                        taintAnalysis.report_vulnerabilities(solver, outs(), MaxDetailedResults.getValue());
                    }
                }
                break;
            }
            default:
                errs() << "Unknown analysis type\n";
                return 1;
        }

        outs() << "Analysis completed successfully.\n";

    } catch (const std::exception &e) {
        errs() << "Error running analysis: " << e.what() << "\n";
        return 1;
    }
    
    // Statistics will be printed automatically at program exit if enabled
    
    return 0;
}
