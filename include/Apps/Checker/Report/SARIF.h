#ifndef SARIF_H
#define SARIF_H

#include <string>
#include <vector>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>
#include "Utils/General/cJSON.h"

namespace sarif {

enum class Level { Note, Warning, Error };

// Simple location representation
struct Location {
    std::string file;
    int line = 0;
    int column = 0;
    std::string function;
    std::string snippet;
    std::string message;  // Optional message for this location
    
    Location() = default;
    Location(const std::string& file, int line, int column = 0) 
        : file(file), line(line), column(column) {}
    
    cJSON* toJson() const;
    cJSON* toThreadFlowLocationJson() const;  // For codeFlow representation
};

// Thread flow location for code flows (execution paths)
struct ThreadFlowLocation {
    Location location;
    std::string message;
    int nestingLevel = 0;  // For showing call depth
    int executionOrder = 0;
    
    ThreadFlowLocation() = default;
    ThreadFlowLocation(const Location& loc, const std::string& msg = "", int order = 0)
        : location(loc), message(msg), executionOrder(order) {}
    
    cJSON* toJson() const;
};

// Code flow represents an execution path
struct CodeFlow {
    std::vector<ThreadFlowLocation> threadFlowLocations;
    std::string message;
    
    CodeFlow() = default;
    
    cJSON* toJson() const;
};

// Simple result representation
struct Result {
    std::string ruleId;
    std::string message;
    Level level = Level::Warning;
    std::vector<Location> locations;
    std::vector<Location> relatedLocations;
    std::vector<CodeFlow> codeFlows;  // Execution paths that lead to the result
    
    Result(const std::string& ruleId, const std::string& message) 
        : ruleId(ruleId), message(message) {}
    
    cJSON* toJson() const;
};

// Simple rule representation
struct Rule {
    std::string id;
    std::string name;
    std::string description;
    
    Rule(const std::string& id, const std::string& name, const std::string& description = "")
        : id(id), name(name), description(description) {}
    
    cJSON* toJson() const;
};

// Main SARIF log
class SarifLog {
public:
    SarifLog(const std::string& toolName = "Lotus", const std::string& version = "1.0.0");
    
    void addRule(const Rule& rule);
    void addResult(const Result& result);
    
    std::string toJsonString(bool pretty = true) const;
    void writeToFile(const std::string& filename, bool pretty = true) const;
    void writeToStream(llvm::raw_ostream& os, bool pretty = true) const;

private:
    std::string toolName;
    std::string toolVersion;
    std::vector<Rule> rules;
    std::vector<Result> results;
    
    cJSON* toJsonDocument() const;
};

// Utility functions
namespace utils {
    Location createLocationFromDebugLoc(const llvm::DebugLoc& debugLoc);
    Location createLocationFromInstruction(const llvm::Instruction* instruction);
    std::string levelToString(Level level);
    Level stringToLevel(const std::string& level);
}

// Simple builder for common use cases
class SarifBuilder {
public:
    SarifBuilder(const std::string& toolName = "Lotus");
    
    SarifBuilder& addRule(const std::string& id, const std::string& name, 
                         const std::string& description = "");
    SarifBuilder& addResult(const std::string& ruleId, const std::string& message,
                           const std::string& file, int line, int column = 0,
                           Level level = Level::Warning);
    
    SarifLog build();

private:
    SarifLog log;
};

} // namespace sarif

#endif // SARIF_H
