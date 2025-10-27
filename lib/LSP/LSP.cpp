/* 
* TODO: Design this module...
* LSP (language server protocol) server for Lotus
* 
* Server for the following services:
*  - Neural-Symbolic Analysis over LLVM IR: fine-grained analysis of LLVM IR.
*  - General Code Agent: meta data extraction, e.g., call graph.
*  - ...?
*
*  Interfaces
* - List functions
* - Callgraph: callee, caller, reachability, reachable functions, etc.
* - ...?
 */

#include <LSP/LSP.h>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <ctime>
#include <iomanip>


void printUsage() {
    std::cout << "Usage: lsp <options>" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help      Show this help message and exit" << std::endl;
    std::cout << "  -v, --version   Show version information and exit" << std::endl;
}

