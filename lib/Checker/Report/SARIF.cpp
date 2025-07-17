#include "Checker/Report/SARIF.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <sstream>

using namespace sarif;

namespace {
// Helper function to create JSON string value
JsonValue createJsonString(const std::string& str, JsonAllocator& allocator) {
    JsonValue val;
    val.SetString(str.c_str(), str.length(), allocator);
    return val;
}

// Helper function to convert Level to string
std::string levelToString(Level level) {
    switch (level) {
        case Level::Note: return "note";
        case Level::Warning: return "warning";
        case Level::Error: return "error";
        default: return "warning";
    }
}
}

// Location implementation
JsonValue Location::toJson(JsonAllocator& allocator) const {
    JsonValue location(rapidjson::kObjectType);
    
    if (!file.empty()) {
        JsonValue artifactLocation(rapidjson::kObjectType);
        artifactLocation.AddMember("uri", createJsonString(file, allocator), allocator);
        location.AddMember("artifactLocation", artifactLocation, allocator);
    }
    
    if (line > 0) {
        JsonValue region(rapidjson::kObjectType);
        region.AddMember("startLine", line, allocator);
        if (column > 0) {
            region.AddMember("startColumn", column, allocator);
        }
        if (!snippet.empty()) {
            JsonValue snippetObj(rapidjson::kObjectType);
            snippetObj.AddMember("text", createJsonString(snippet, allocator), allocator);
            region.AddMember("snippet", snippetObj, allocator);
        }
        location.AddMember("region", region, allocator);
    }
    
    if (!function.empty()) {
        JsonValue logicalLocation(rapidjson::kObjectType);
        logicalLocation.AddMember("name", createJsonString(function, allocator), allocator);
        logicalLocation.AddMember("kind", createJsonString("function", allocator), allocator);
        location.AddMember("logicalLocation", logicalLocation, allocator);
    }
    
    return location;
}

// Result implementation
JsonValue Result::toJson(JsonAllocator& allocator) const {
    JsonValue result(rapidjson::kObjectType);
    
    if (!ruleId.empty()) {
        result.AddMember("ruleId", createJsonString(ruleId, allocator), allocator);
    }
    
    JsonValue messageObj(rapidjson::kObjectType);
    messageObj.AddMember("text", createJsonString(message, allocator), allocator);
    result.AddMember("message", messageObj, allocator);
    
    if (level != Level::Warning) {
        result.AddMember("level", createJsonString(levelToString(level), allocator), allocator);
    }
    
    if (!locations.empty()) {
        JsonValue locationsArray(rapidjson::kArrayType);
        for (const auto& location : locations) {
            locationsArray.PushBack(location.toJson(allocator), allocator);
        }
        result.AddMember("locations", locationsArray, allocator);
    }
    
    if (!relatedLocations.empty()) {
        JsonValue relatedLocationsArray(rapidjson::kArrayType);
        for (const auto& location : relatedLocations) {
            relatedLocationsArray.PushBack(location.toJson(allocator), allocator);
        }
        result.AddMember("relatedLocations", relatedLocationsArray, allocator);
    }
    
    return result;
}

// Rule implementation
JsonValue Rule::toJson(JsonAllocator& allocator) const {
    JsonValue rule(rapidjson::kObjectType);
    
    if (!id.empty()) {
        rule.AddMember("id", createJsonString(id, allocator), allocator);
    }
    
    if (!name.empty()) {
        rule.AddMember("name", createJsonString(name, allocator), allocator);
    }
    
    if (!description.empty()) {
        JsonValue shortDesc(rapidjson::kObjectType);
        shortDesc.AddMember("text", createJsonString(description, allocator), allocator);
        rule.AddMember("shortDescription", shortDesc, allocator);
    }
    
    return rule;
}

// SarifLog implementation
SarifLog::SarifLog(const std::string& toolName, const std::string& version) 
    : toolName(toolName), toolVersion(version) {}

void SarifLog::addRule(const Rule& rule) {
    rules.push_back(rule);
}

void SarifLog::addResult(const Result& result) {
    results.push_back(result);
}

std::string SarifLog::toJsonString(bool pretty) const {
    JsonDocument doc = toJsonDocument();
    
    rapidjson::StringBuffer buffer;
    if (pretty) {
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
    } else {
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
    }
    
    return buffer.GetString();
}

void SarifLog::writeToFile(const std::string& filename, bool pretty) const {
    std::ofstream file(filename);
    if (file.is_open()) {
        file << toJsonString(pretty);
        file.close();
    }
}

void SarifLog::writeToStream(llvm::raw_ostream& os, bool pretty) const {
    os << toJsonString(pretty);
}

JsonDocument SarifLog::toJsonDocument() const {
    JsonDocument doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();
    
    doc.AddMember("version", createJsonString("2.1.0", allocator), allocator);
    doc.AddMember("$schema", createJsonString("https://json.schemastore.org/sarif-2.1.0.json", allocator), allocator);
    
    JsonValue runsArray(rapidjson::kArrayType);
    JsonValue run(rapidjson::kObjectType);
    
    // Tool information
    JsonValue tool(rapidjson::kObjectType);
    JsonValue driver(rapidjson::kObjectType);
    driver.AddMember("name", createJsonString(toolName, allocator), allocator);
    driver.AddMember("version", createJsonString(toolVersion, allocator), allocator);
    
    if (!rules.empty()) {
        JsonValue rulesArray(rapidjson::kArrayType);
        for (const auto& rule : rules) {
            rulesArray.PushBack(rule.toJson(allocator), allocator);
        }
        driver.AddMember("rules", rulesArray, allocator);
    }
    
    tool.AddMember("driver", driver, allocator);
    run.AddMember("tool", tool, allocator);
    
    // Results
    if (!results.empty()) {
        JsonValue resultsArray(rapidjson::kArrayType);
        for (const auto& result : results) {
            resultsArray.PushBack(result.toJson(allocator), allocator);
        }
        run.AddMember("results", resultsArray, allocator);
    }
    
    runsArray.PushBack(run, allocator);
    doc.AddMember("runs", runsArray, allocator);
    
    return doc;
}

// Utility functions implementation
namespace utils {

Location createLocationFromDebugLoc(const llvm::DebugLoc& debugLoc) {
    Location location;
    
    if (debugLoc) {
        location.line = debugLoc.getLine();
        location.column = debugLoc.getCol();
        
        if (auto* scope = debugLoc.getScope()) {
            if (auto* diScope = llvm::dyn_cast<llvm::DIScope>(scope)) {
                location.file = diScope->getDirectory().str() + "/" + diScope->getFilename().str();
            }
        }
    }
    
    return location;
}

Location createLocationFromInstruction(const llvm::Instruction* instruction) {
    Location location;
    
    if (instruction && instruction->getDebugLoc()) {
        location = createLocationFromDebugLoc(instruction->getDebugLoc());
        
        if (auto* func = instruction->getFunction()) {
            location.function = func->getName().str();
        }
    }
    
    return location;
}

std::string levelToString(Level level) {
    return ::levelToString(level);
}

Level stringToLevel(const std::string& level) {
    if (level == "note") return Level::Note;
    if (level == "warning") return Level::Warning;
    if (level == "error") return Level::Error;
    return Level::Warning;
}

} // namespace utils

// SarifBuilder implementation
SarifBuilder::SarifBuilder(const std::string& toolName) : log(toolName) {}

SarifBuilder& SarifBuilder::addRule(const std::string& id, const std::string& name, 
                                   const std::string& description) {
    log.addRule(Rule(id, name, description));
    return *this;
}

SarifBuilder& SarifBuilder::addResult(const std::string& ruleId, const std::string& message,
                                     const std::string& file, int line, int column,
                                     Level level) {
    Result result(ruleId, message);
    result.level = level;
    result.locations.push_back(Location(file, line, column));
    log.addResult(result);
    return *this;
}

SarifLog SarifBuilder::build() {
    return log;
}
