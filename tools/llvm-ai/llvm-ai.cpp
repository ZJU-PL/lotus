/**
 * @file llvm-ai.cpp
 * @brief LLVM IFDS/IDE Analysis Tool
 * 
 * A command-line tool for running IFDS/IDE interprocedural dataflow analysis 
 * on LLVM bitcode files using the Sparta framework.
 */

#include <Analysis/IFDS/IFDSFramework.h>
#include <Analysis/IFDS/TaintAnalysis.h>
#include <Analysis/IFDS/ReachingDefinitions.h>

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

#include <iostream>
#include <memory>
#include <string>
#include <sstream>

using namespace llvm;
using namespace sparta::ifds;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"), cl::init(false));

static cl::opt<int> AnalysisType("analysis", cl::desc("Type of analysis to run: 0=taint, 1=reaching-defs"),
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
                solver.solve(*M);
                
                if (ShowResults) {
                    outs() << "\nTaint Flow Vulnerability Analysis:\n";
                    outs() << "==================================\n";
                    
                    const auto& results = solver.get_all_results();
                    size_t vulnerability_count = 0;
                    const size_t max_vulnerabilities = MaxDetailedResults.getValue();
                    
                    // Look for taint flow vulnerabilities (tainted data reaching sinks)
                    for (const auto& result : results) {
                        const auto& node = result.first;
                        const auto& facts = result.second;
                        
                        if (facts.empty() || !node.instruction) continue;
                        
                        // Check if this is a sink instruction with tainted data
                        if (auto* call = llvm::dyn_cast<llvm::CallInst>(node.instruction)) {
                            if (const llvm::Function* callee = call->getCalledFunction()) {
                                std::string func_name = callee->getName().str();
                                
                                // Check if this is a known sink function
                                bool is_sink = (func_name == "system" || func_name == "exec" || 
                                              func_name == "execl" || func_name == "execv" || 
                                              func_name == "popen" || func_name == "printf" || 
                                              func_name == "fprintf" || func_name == "sprintf" || 
                                              func_name == "strcpy" || func_name == "strcat");
                                
                                if (is_sink) {
                                    // Check if any arguments are tainted
                                    bool has_tainted_args = false;
                                    std::string tainted_args;
                                    
                                    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
                                        const llvm::Value* arg = call->getOperand(i);
                                        
                                        // Check if this argument is tainted
                                        for (const auto& fact : facts) {
                                            if (fact.is_tainted_var() && fact.get_value() == arg) {
                                                has_tainted_args = true;
                                                if (!tainted_args.empty()) tainted_args += ", ";
                                                tainted_args += "arg" + std::to_string(i);
                                                break;
                                            }
                                        }
                                    }
                                    
                                    if (has_tainted_args) {
                                        vulnerability_count++;
                                        if (vulnerability_count <= max_vulnerabilities) {
                                            outs() << "\n🚨 VULNERABILITY #" << vulnerability_count << ":\n";
                                            outs() << "  Sink: " << func_name << " at " << *call << "\n";
                                            outs() << "  Tainted arguments: " << tainted_args << "\n";
                                            outs() << "  Location: " << call->getDebugLoc() << "\n";
                                        }
                                    }
                                }
                            }
                        }
                    }
                    
                    if (vulnerability_count == 0) {
                        outs() << "✅ No taint flow vulnerabilities detected.\n";
                        outs() << "   (This means no tainted data reached dangerous sink functions)\n";
                    } else {
                        outs() << "\n📊 Summary:\n";
                        outs() << "  Total vulnerabilities found: " << vulnerability_count << "\n";
                        if (vulnerability_count > max_vulnerabilities) {
                            outs() << "  (Showing first " << max_vulnerabilities << " vulnerabilities)\n";
                        }
                    }
                }
                break;
            }
            case 1: { // Reaching definitions
                outs() << "Running interprocedural reaching definitions analysis...\n";
                
                ReachingDefinitionsAnalysis reachingDefs;
                
                // Set up alias analysis
                reachingDefs.set_alias_analysis(dyckAA);
                
                IFDSSolver<ReachingDefinitionsAnalysis> solver(reachingDefs);
                solver.solve(*M);
                
                if (ShowResults) {
                    outs() << "\nReaching Definitions Results:\n";
                    outs() << "=============================\n";
                    
                    const auto& results = solver.get_all_results();
                    for (const auto& result : results) {
                        const auto& node = result.first;
                        const auto& facts = result.second;
                        
                        if (!facts.empty()) {
                            outs() << "At instruction: " << *node.instruction << "\n";
                            outs() << "  Definition facts: [" << facts.size() << " facts]\n\n";
                        }
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
    
    return 0;
}
