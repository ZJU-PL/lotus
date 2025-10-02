/**
 * @file pdg-query.cpp
 * @brief Command-line tool for querying Program Dependence Graphs
 *
 * This tool provides a command-line interface for executing queries against
 * Program Dependence Graphs using the PDG query language. It supports both
 * interactive and batch modes for query execution.
 */

#include "IR/PDG/QueryParser.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/InitLLVM.h"

#include <iostream>
#include <fstream>
#include <string>

using namespace llvm;
using namespace pdg;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string> QueryString("query", "q",
                                        cl::desc("Execute a single query"),
                                        cl::value_desc("query"));

static cl::opt<std::string> PolicyString("policy", "p",
                                         cl::desc("Execute a policy check"),
                                         cl::value_desc("policy"));

static cl::opt<std::string> QueryFile("query-file", "f",
                                      cl::desc("Execute queries from file"),
                                      cl::value_desc("filename"));

static cl::opt<bool> Interactive("interactive", "i",
                                 cl::desc("Run in interactive mode"));

static cl::opt<bool> Verbose("verbose", "v",
                             cl::desc("Enable verbose output"));

static cl::opt<std::string> TargetFunction("function",
                                           cl::desc("Target function for analysis"),
                                           cl::value_desc("function_name"));

void printUsage(const char* programName) {
    errs() << "Usage: " << programName << " [options] <input.bc>\n";
    errs() << "Options:\n";
    errs() << "  -q, --query <query>        Execute a single query\n";
    errs() << "  -p, --policy <policy>      Execute a policy check\n";
    errs() << "  -f, --query-file <file>    Execute queries from file\n";
    errs() << "  -i, --interactive          Run in interactive mode\n";
    errs() << "  -v, --verbose              Enable verbose output\n";
    errs() << "  --function <name>          Target function for analysis\n";
}

void printPDGInfo(ProgramGraph& pdg) {
    outs() << "PDG Information:\n";
    outs() << "  Total nodes: " << pdg.numNode() << "\n";
    outs() << "  Total edges: " << pdg.numEdge() << "\n";
    outs() << "  Functions: " << pdg.getFuncWrapperMap().size() << "\n";
}

void executeQuery(QueryParser& interpreter, const std::string& query, bool isPolicy = false) {
    if (Verbose) {
        outs() << "Executing " << (isPolicy ? "policy" : "query") << ": " << query << "\n";
    }
    
    try {
        if (isPolicy) {
            interpreter.evaluatePolicy(query);
        } else {
            interpreter.evaluate(query);
        }
    } catch (const std::exception& e) {
        errs() << "Error: " << e.what() << "\n";
    }
}

void runInteractiveMode(QueryParser& interpreter) {
    outs() << "PDG Query Interactive Mode\n";
    outs() << "Type 'help' for commands, 'quit' to exit\n";
    outs() << "> ";
    
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            outs() << "> ";
            continue;
        }
        
        if (line == "quit" || line == "exit") {
            break;
        }
        
        if (line == "help") {
            outs() << "Commands: help, quit, info, <query>, policy <policy>\n";
        } else if (line == "info") {
            printPDGInfo(interpreter.getExecutor().getPDG());
        } else if (line.substr(0, 7) == "policy ") {
            std::string policy = line.substr(7);
            executeQuery(interpreter, policy, true);
        } else {
            executeQuery(interpreter, line, false);
        }
        
        outs() << "> ";
    }
}

void runBatchMode(QueryParser& interpreter, const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        errs() << "Error: Could not open file " << filename << "\n";
        return;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Check if it's a policy (contains "is empty")
        bool isPolicy = line.find("is empty") != std::string::npos;
        
        executeQuery(interpreter, line, isPolicy);
    }
}

int main(int argc, char** argv) {
    InitLLVM X(argc, argv);
    
    cl::ParseCommandLineOptions(argc, argv, "PDG Query Tool\n");
    
    if (InputFilename.empty()) {
        printUsage(argv[0]);
        return 1;
    }
    
    // Create LLVM context and load module
    LLVMContext Context;
    SMDiagnostic Err;
    
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }
    
    outs() << "Loaded module: " << InputFilename << "\n";
    
    // Build PDG
    ProgramGraph& pdg = ProgramGraph::getInstance();
    pdg.build(*M);
    pdg.bindDITypeToNodes(*M);
    
    if (Verbose) {
        printPDGInfo(pdg);
    }
    
    // Create query interpreter
    QueryParser interpreter;
    
    // Execute queries based on mode
    if (Interactive) {
        runInteractiveMode(interpreter);
    } else if (!QueryString.empty()) {
        executeQuery(interpreter, QueryString, false);
    } else if (!PolicyString.empty()) {
        executeQuery(interpreter, PolicyString, true);
    } else if (!QueryFile.empty()) {
        runBatchMode(interpreter, QueryFile);
    } else {
        outs() << "No query specified. Use -q for a single query, -i for interactive mode, or -f for batch file.\n";
        outs() << "Example: " << argv[0] << " -q \"pgm\" " << InputFilename << "\n";
    }
    
    return 0;
}
