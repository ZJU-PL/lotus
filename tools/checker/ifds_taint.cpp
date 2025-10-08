/**
 * @file llvm-ai.cpp
 * @brief LLVM IFDS/IDE Analysis Tool
 * 
 * A command-line tool for running IFDS/IDE interprocedural dataflow analysis 
 * on LLVM bitcode files using the Sparta framework.
 */

#include <Analysis/IFDS/IFDSFramework.h>
#include <Analysis/IFDS/IFDSTaintAnalysis.h>

#include <Alias/DyckAA/DyckAliasAnalysis.h>

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

using namespace llvm;
using namespace ifds;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"), cl::init(false));

static cl::opt<int> AnalysisType("analysis", cl::desc("Type of analysis to run: 0=taint"),
                                 cl::init(0));

// Using Dyck alias analysis by default

static cl::opt<bool> ShowResults("show-results", cl::desc("Show detailed analysis results"), 
                                 cl::init(true));

static cl::opt<int> MaxDetailedResults("max-results", cl::desc("Maximum number of detailed results to show"), 
                                      cl::init(10));

static cl::opt<std::string> SourceFunctions("sources", cl::desc("Comma-separated list of source functions"),
                                            cl::init(""));

static cl::opt<std::string> SinkFunctions("sinks", cl::desc("Comma-separated list of sink functions"),
                                          cl::init(""));

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

int main(int argc, char **argv) {
    InitLLVM X(argc, argv);
    
    cl::ParseCommandLineOptions(argc, argv, "LLVM IFDS/IDE Analysis Tool\n");
    
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
    
    // Set up Dyck alias analysis
    if (Verbose) {
        outs() << "Using Dyck alias analysis\n";
    }
    
    legacy::PassManager PM;
    auto dyckPass = std::make_unique<DyckAliasAnalysis>();
    DyckAliasAnalysis* dyckAA = dyckPass.get();
    PM.add(dyckPass.release());
    PM.run(*M);
    
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
                taintAnalysis.set_alias_analysis(dyckAA);
                
                IFDSSolver<TaintAnalysis> solver(taintAnalysis);
                
                // Enable progress bar when running in verbose mode
                if (Verbose) {
                    solver.set_show_progress(true);
                }
                
                solver.solve(*M);
                
                if (ShowResults) {
                    taintAnalysis.report_vulnerabilities(solver, outs(), MaxDetailedResults.getValue());
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
    
    return 0;
}
