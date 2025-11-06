/*
* MCP (Model Context Protocol) Tool for Lotus
*
* Provides callgraph analysis and queries for LLVM IR
*/

#include <MCP/MCPServer.h>
#include <iostream>
#include <fstream>

using namespace lotus::mcp;

void printUsage() {
    std::cout << "Usage: mcp <bitcode> <command> [args]\n\n"
              << "Commands:\n"
              << "  list                   List all functions\n"
              << "  callees <func>         Get direct callees\n"
              << "  callers <func>         Get direct callers\n"
              << "  reachable <func>       Get all reachable functions\n"
              << "  can-reach <from> <to>  Check reachability\n"
              << "  export-json            Export as JSON\n"
              << "  export-dot             Export as DOT (Graphviz)\n\n"
              << "Examples:\n"
              << "  mcp program.bc callees main\n"
              << "  mcp program.bc can-reach main exit\n"
              << "  mcp program.bc export-dot > graph.dot\n";
}

int main(int argc, char **argv) {
    if (argc < 3 || std::string(argv[1]) == "-h") {
        printUsage();
        return 0;
    }
    
    MCPServer server;
    if (!server.loadModule(argv[1])) {
        std::cerr << "Failed to load: " << argv[1] << std::endl;
        return 1;
    }
    server.buildCallGraph();
    
    std::string cmd = argv[2];
    
    if (cmd == "list") {
        auto funcs = server.getAllFunctions();
        std::cout << "Functions: " << funcs.size() << "\n";
        for (const auto &f : funcs) std::cout << f << "\n";
    }
    else if (cmd == "callees" && argc > 3) {
        auto results = server.getCallees(argv[3]);
        std::cout << "Callees of " << argv[3] << ": " << results.size() << "\n";
        for (const auto &r : results) std::cout << "  " << r << "\n";
    }
    else if (cmd == "callers" && argc > 3) {
        auto results = server.getCallers(argv[3]);
        std::cout << "Callers of " << argv[3] << ": " << results.size() << "\n";
        for (const auto &r : results) std::cout << "  " << r << "\n";
    }
    else if (cmd == "reachable" && argc > 3) {
        auto results = server.getReachableFunctions(argv[3]);
        std::cout << "Reachable from " << argv[3] << ": " << results.size() << "\n";
        for (const auto &r : results) std::cout << "  " << r << "\n";
    }
    else if (cmd == "can-reach" && argc > 4) {
        bool reach = server.canReach(argv[3], argv[4]);
        std::cout << argv[3] << (reach ? " can " : " cannot ") 
                  << "reach " << argv[4] << "\n";
    }
    else if (cmd == "export-json") {
        std::cout << server.exportAsJSON();
    }
    else if (cmd == "export-dot") {
        std::cout << server.exportAsDOT();
    }
    else {
        std::cerr << "Unknown or incomplete command\n";
        printUsage();
        return 1;
    }
    
    return 0;
}

