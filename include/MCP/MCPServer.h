#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace lotus {
namespace mcp {

struct CallGraphData {
  std::map<std::string, std::set<std::string>> callees;
  std::map<std::string, std::set<std::string>> callers;
  std::set<std::string> allFunctions;
  std::vector<std::string> indirectCalls;
};

class MCPServer {
public:
  MCPServer();
  ~MCPServer();
  
  bool loadModule(const std::string &bitcodeFile);
  void buildCallGraph();
  
  std::vector<std::string> getCallees(const std::string &functionName);
  std::vector<std::string> getCallers(const std::string &functionName);
  std::vector<std::string> getAllFunctions();
  std::vector<std::string> getReachableFunctions(const std::string &from);
  bool canReach(const std::string &from, const std::string &to);
  
  std::string exportAsJSON();
  std::string exportAsDOT();
  
private:
  llvm::LLVMContext context_;
  std::unique_ptr<llvm::Module> module_;
  CallGraphData callGraph_;
  std::map<std::string, std::set<std::string>> transitiveClosure_;
  
  void extractCallGraph();
  void computeTransitiveClosure();
};

} // namespace mcp
} // namespace lotus

