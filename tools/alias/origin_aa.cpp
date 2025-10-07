/*
 * Canary: OriginAA Pointer Analysis Driver
 * This file implements the main driver for running OriginAA pointer analysis
 * on LLVM bitcode files. It parses command-line options, loads the input module,
 * runs the OriginAA analysis, and outputs the results.

 K-callsite sensitve and origin-sensitive (from https://github.com/bozhen-liu/AFG)
 */

#include "Alias/OriginAA/PointerAnalysis.h"
#include "Alias/OriginAA/KCallsitePointerAnalysis.h"
#include "Alias/OriginAA/OriginPointerAnalysis.h"
#include "Alias/OriginAA/Flags.h"
#include "LLVMUtils/RecursiveTimer.h"
//#include "LLVMUtils/Statistics.h"
 
 #include <llvm/IR/LLVMContext.h>
 #include <llvm/IR/Module.h>
 #include <llvm/IR/Verifier.h>
 #include <llvm/IRReader/IRReader.h>
 #include <llvm/Support/InitLLVM.h>
 #include <llvm/Support/CommandLine.h>
 #include <llvm/Support/Signals.h>
 #include <llvm/Support/ToolOutputFile.h>
 #include <llvm/Support/SourceMgr.h>
 #include <llvm/Support/FileSystem.h>
 #include <memory>
 #include <fstream>
 
 using namespace llvm;
 
 static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                                           cl::init("-"), cl::value_desc("filename"));
 
 static cl::opt<std::string> OutputFilename("o", cl::desc("Override output filename"),
                                            cl::value_desc("filename"));
 
 static cl::opt<bool> OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                                    cl::init(false));
 
 static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"), cl::init(false));
 
 // Debug output is controlled by the global DebugMode option from Flags.h
 
 static cl::opt<bool> EnableTaintAnalysis("taint", cl::desc("Enable taint analysis"), cl::init(false));
 
 static cl::opt<bool> HandleIndirectCallsOpt("handle-indirect", cl::desc("Handle indirect calls"), cl::init(true));
 
 static cl::opt<unsigned> MaxVisitOpt("max-visits", cl::desc("Maximum number of function visits"), cl::init(10));
 
 static cl::opt<std::string> OutputDir("output-dir", cl::desc("Output directory for analysis results"),
                                       cl::value_desc("directory"));
 
 static cl::opt<bool> PrintCallGraph("print-cg", cl::desc("Print call graph"), cl::init(false));
 
 static cl::opt<bool> PrintPointsToMap("print-pts", cl::desc("Print points-to map"), cl::init(false));
 
 static cl::opt<bool> PrintTaintedNodes("print-tainted", cl::desc("Print tainted nodes"), cl::init(false));
 
 static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));

static cl::opt<std::string> LocalAnalysisMode("analysis-mode", 
    cl::desc("Pointer analysis mode: 'ci' (context-insensitive), 'kcs' (k-callsite-sensitive), or 'origin' (origin-sensitive)"),
    cl::init("ci"), cl::value_desc("mode"));

// KValue is defined globally in Flags.h
 
 int main(int argc, char **argv) {
     InitLLVM X(argc, argv);
     
     // No initialization needed for pointer analysis
     
     cl::ParseCommandLineOptions(argc, argv, "Canary OriginAA Pointer Analysis Tool\n");
 
     LLVMContext Context;
     SMDiagnostic Err;
 
     // Load the input module
     std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
     if (!M) {
         Err.print(argv[0], errs());
         return 1;
     }
 
     // Verify the module
     if (verifyModule(*M, &errs())) {
         errs() << "Error: Module verification failed\n";
         return 1;
     }
 
     // Create output file if specified
     std::unique_ptr<ToolOutputFile> Out;
     if (!OutputFilename.empty()) {
         std::error_code EC;
         Out = std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
         if (EC) {
             errs() << EC.message() << '\n';
             return 1;
         }
     }
 
     // Set up the output stream
     raw_ostream &OS = Out ? Out->os() : outs();
 
    // Create and configure the pointer analysis based on mode
    std::unique_ptr<PointerAnalysis> PA;
    if (LocalAnalysisMode == "kcs") {
        PA = std::make_unique<KCallsitePointerAnalysis>(KValue, *M);
        if (Verbose) {
            errs() << "Analysis mode: k-callsite-sensitive (k=" << KValue << ")\n";
        }
    } else if (LocalAnalysisMode == "origin") {
        PA = std::make_unique<OriginPointerAnalysis>(KValue, *M);
        if (Verbose) {
            errs() << "Analysis mode: origin-sensitive (k=" << KValue << ")\n";
        }
    } else { // Default to context-insensitive
        PA = std::make_unique<PointerAnalysis>(*M);
        if (Verbose) {
            errs() << "Analysis mode: context-insensitive\n";
        }
    }
    
    // Set configuration options
    PA->setDebugMode(DebugMode);
    PA->setTaintingEnabled(EnableTaintAnalysis);
    PA->setHandleIndirectCalls(HandleIndirectCallsOpt);
    PA->setMaxVisit(MaxVisitOpt);
     
    if (Verbose) {
        errs() << "Starting OriginAA Pointer Analysis...\n";
        errs() << "Input file: " << InputFilename << "\n";
        errs() << "Debug mode: " << (DebugMode ? "enabled" : "disabled") << "\n";
        errs() << "Taint analysis: " << (EnableTaintAnalysis ? "enabled" : "disabled") << "\n";
        errs() << "Handle indirect calls: " << (HandleIndirectCallsOpt ? "enabled" : "disabled") << "\n";
        errs() << "Max visits per function: " << MaxVisitOpt << "\n";
    }

    // Run the analysis
    RecursiveTimer timer("OriginAA Analysis");
    PA->analyze();
 
    // Output results
    if (OnlyStatistics) {
        PA->printStatistics();
    } else {
        // Print call graph if requested
        if (PrintCallGraph) {
            std::ofstream cgFile("callgraph.txt");
            PA->printCallGraph(cgFile);
            cgFile.close();
            OS << "Call graph written to callgraph.txt\n";
        }
        
        // Print points-to map if requested
        if (PrintPointsToMap) {
            std::ofstream ptsFile("pointsto.txt");
            PA->printPointsToMap(ptsFile);
            ptsFile.close();
            OS << "Points-to map written to pointsto.txt\n";
        }
        
        // Print tainted nodes if requested
        if (PrintTaintedNodes) {
            std::ofstream taintFile("tainted.txt");
            PA->printTaintedNodes(taintFile);
            taintFile.close();
            OS << "Tainted nodes written to tainted.txt\n";
        }
        
        // Always print statistics
        PA->printStatistics();
    }
 
     // Write output file if specified
     if (Out) {
         Out->keep();
     }
 
     return 0;
 }
 