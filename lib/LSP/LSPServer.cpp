#include <LSP/LSPServer.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <sstream>
//#include <iostream>

namespace lotus {
namespace lsp {

LSPServer::LSPServer() = default;
LSPServer::~LSPServer() = default;

bool LSPServer::loadModule(const std::string &bitcodeFile) {
  llvm::SMDiagnostic err;
  module_ = llvm::parseIRFile(bitcodeFile, err, context_);
  if (!module_) {
    err.print("LSPServer", llvm::errs());
    return false;
  }
  return true;
}

void LSPServer::buildCallGraph() {
  if (!module_) return;
  callGraph_ = CallGraphData();
  extractCallGraph();
  computeTransitiveClosure();
}

void LSPServer::extractCallGraph() {
  for (auto &F : *module_) {
    if (!F.isDeclaration()) 
      callGraph_.allFunctions.insert(F.getName().str());
  }
  
  for (auto &F : *module_) {
    if (F.isDeclaration()) continue;
    std::string caller = F.getName().str();
    
    for (auto &BB : F) {
      for (auto &I : BB) {
        llvm::Function *callee = nullptr;
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I))
          callee = CI->getCalledFunction();
        else if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(&I))
          callee = II->getCalledFunction();
        
        if (callee && !callee->isIntrinsic()) {
          std::string calleeName = callee->getName().str();
          if (calleeName.find("llvm.") != 0) {
            callGraph_.callees[caller].insert(calleeName);
            callGraph_.callers[calleeName].insert(caller);
          }
        } else if (!callee && llvm::isa<llvm::CallInst>(&I)) {
          callGraph_.indirectCalls.push_back(caller + ":indirect");
        }
      }
    }
  }
}

void LSPServer::computeTransitiveClosure() {
  transitiveClosure_ = callGraph_.callees;
  
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &from : callGraph_.allFunctions) {
      size_t oldSize = transitiveClosure_[from].size();
      for (const auto &mid : transitiveClosure_[from]) {
        transitiveClosure_[from].insert(
          transitiveClosure_[mid].begin(), 
          transitiveClosure_[mid].end()
        );
      }
      if (transitiveClosure_[from].size() > oldSize) changed = true;
    }
  }
}

std::vector<std::string> LSPServer::getCallees(const std::string &func) {
  auto it = callGraph_.callees.find(func);
  return it != callGraph_.callees.end() ? 
    std::vector<std::string>(it->second.begin(), it->second.end()) : 
    std::vector<std::string>{};
}

std::vector<std::string> LSPServer::getCallers(const std::string &func) {
  auto it = callGraph_.callers.find(func);
  return it != callGraph_.callers.end() ? 
    std::vector<std::string>(it->second.begin(), it->second.end()) : 
    std::vector<std::string>{};
}

std::vector<std::string> LSPServer::getAllFunctions() {
  return std::vector<std::string>(
    callGraph_.allFunctions.begin(), 
    callGraph_.allFunctions.end()
  );
}

std::vector<std::string> LSPServer::getReachableFunctions(const std::string &from) {
  auto it = transitiveClosure_.find(from);
  return it != transitiveClosure_.end() ? 
    std::vector<std::string>(it->second.begin(), it->second.end()) : 
    std::vector<std::string>{};
}

bool LSPServer::canReach(const std::string &from, const std::string &to) {
  auto it = transitiveClosure_.find(from);
  return it != transitiveClosure_.end() && it->second.count(to);
}

std::string LSPServer::exportAsJSON() {
  std::stringstream ss;
  ss << "{\"functions\":[";
  bool first = true;
  for (const auto &f : callGraph_.allFunctions) {
    ss << (first ? "" : ",") << "\"" << f << "\"";
    first = false;
  }
  ss << "],\"callGraph\":{";
  first = true;
  for (const auto &entry : callGraph_.callees) {
    ss << (first ? "" : ",") << "\"" << entry.first << "\":[";
    bool f2 = true;
    for (const auto &c : entry.second) {
      ss << (f2 ? "" : ",") << "\"" << c << "\"";
      f2 = false;
    }
    ss << "]";
    first = false;
  }
  ss << "}}\n";
  return ss.str();
}

std::string LSPServer::exportAsDOT() {
  std::stringstream ss;
  ss << "digraph CallGraph {\n  node [shape=box];\n";
  for (const auto &entry : callGraph_.callees)
    for (const auto &callee : entry.second)
      ss << "  \"" << entry.first << "\" -> \"" << callee << "\";\n";
  ss << "}\n";
  return ss.str();
}

} // namespace lsp
} // namespace lotus
