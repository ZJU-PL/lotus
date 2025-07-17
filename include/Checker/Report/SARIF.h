#ifndef SARIF_H
#define SARIF_H

#include <string>
#include <vector>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>
#include "Support/rapidjson/document.h"
#include "Support/rapidjson/writer.h"
#include "Support/rapidjson/stringbuffer.h"
#include "Support/rapidjson/prettywriter.h"

namespace sarif {

// Use rapidjson namespace directly
typedef rapidjson::Value JsonValue;
typedef rapidjson::Document JsonDocument;
typedef rapidjson::Document::AllocatorType JsonAllocator;

enum class Level { Note, Warning, Error };

// Simple location representation
struct Location {
    std::string file;
    int line = 0;
    int column = 0;
    std::string function;
    std::string snippet;
    
    Location() = default;
    Location(const std::string& file, int line, int column = 0) 
        : file(file), line(line), column(column) {}
    
    JsonValue toJson(JsonAllocator& allocator) const;
};

// Simple result representation
struct Result {
    std::string ruleId;
    std::string message;
    Level level = Level::Warning;
    std::vector<Location> locations;
    std::vector<Location> relatedLocations;
    
    Result(const std::string& ruleId, const std::string& message) 
        : ruleId(ruleId), message(message) {}
    
    JsonValue toJson(JsonAllocator& allocator) const;
};

// Simple rule representation
struct Rule {
    std::string id;
    std::string name;
    std::string description;
    
    Rule(const std::string& id, const std::string& name, const std::string& description = "")
        : id(id), name(name), description(description) {}
    
    JsonValue toJson(JsonAllocator& allocator) const;
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
    
    JsonDocument toJsonDocument() const;
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
