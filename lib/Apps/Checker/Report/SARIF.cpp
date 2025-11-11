#include "Apps/Checker/Report/SARIF.h"
#include "Utils/LLVM/Demangle.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>

namespace sarif {

namespace {
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
cJSON* Location::toJson() const {
    cJSON* location = cJSON_CreateObject();
    
    if (!file.empty()) {
        cJSON* artifactLocation = cJSON_CreateObject();
        cJSON_AddStringToObject(artifactLocation, "uri", file.c_str());
        cJSON_AddItemToObject(location, "artifactLocation", artifactLocation);
    }
    
    if (line > 0) {
        cJSON* region = cJSON_CreateObject();
        cJSON_AddNumberToObject(region, "startLine", line);
        if (column > 0) {
            cJSON_AddNumberToObject(region, "startColumn", column);
        }
        if (!snippet.empty()) {
            cJSON* snippetObj = cJSON_CreateObject();
            cJSON_AddStringToObject(snippetObj, "text", snippet.c_str());
            cJSON_AddItemToObject(region, "snippet", snippetObj);
        }
        cJSON_AddItemToObject(location, "region", region);
    }
    
    if (!function.empty()) {
        cJSON* logicalLocation = cJSON_CreateObject();
        cJSON_AddStringToObject(logicalLocation, "name", function.c_str());
        cJSON_AddStringToObject(logicalLocation, "kind", "function");
        cJSON_AddItemToObject(location, "logicalLocation", logicalLocation);
    }
    
    return location;
}

cJSON* Location::toThreadFlowLocationJson() const {
    cJSON* physicalLocation = cJSON_CreateObject();
    
    if (!file.empty()) {
        cJSON* artifactLocation = cJSON_CreateObject();
        cJSON_AddStringToObject(artifactLocation, "uri", file.c_str());
        cJSON_AddItemToObject(physicalLocation, "artifactLocation", artifactLocation);
    }
    
    if (line > 0) {
        cJSON* region = cJSON_CreateObject();
        cJSON_AddNumberToObject(region, "startLine", line);
        if (column > 0) {
            cJSON_AddNumberToObject(region, "startColumn", column);
        }
        cJSON_AddItemToObject(physicalLocation, "region", region);
    }
    
    return physicalLocation;
}

// ThreadFlowLocation implementation
cJSON* ThreadFlowLocation::toJson() const {
    cJSON* tfl = cJSON_CreateObject();
    
    cJSON_AddItemToObject(tfl, "location", location.toThreadFlowLocationJson());
    
    if (!message.empty()) {
        cJSON* msgObj = cJSON_CreateObject();
        cJSON_AddStringToObject(msgObj, "text", message.c_str());
        cJSON_AddItemToObject(tfl, "message", msgObj);
    }
    
    if (nestingLevel > 0) {
        cJSON_AddNumberToObject(tfl, "nestingLevel", nestingLevel);
    }
    
    if (executionOrder > 0) {
        cJSON_AddNumberToObject(tfl, "executionOrder", executionOrder);
    }
    
    return tfl;
}

// CodeFlow implementation
cJSON* CodeFlow::toJson() const {
    cJSON* codeFlow = cJSON_CreateObject();
    
    if (!message.empty()) {
        cJSON* msgObj = cJSON_CreateObject();
        cJSON_AddStringToObject(msgObj, "text", message.c_str());
        cJSON_AddItemToObject(codeFlow, "message", msgObj);
    }
    
    if (!threadFlowLocations.empty()) {
        cJSON* threadFlows = cJSON_CreateArray();
        cJSON* threadFlow = cJSON_CreateObject();
        cJSON* locations = cJSON_CreateArray();
        
        for (const auto& tfl : threadFlowLocations) {
            cJSON_AddItemToArray(locations, tfl.toJson());
        }
        
        cJSON_AddItemToObject(threadFlow, "locations", locations);
        cJSON_AddItemToArray(threadFlows, threadFlow);
        cJSON_AddItemToObject(codeFlow, "threadFlows", threadFlows);
    }
    
    return codeFlow;
}

// Result implementation
cJSON* Result::toJson() const {
    cJSON* result = cJSON_CreateObject();
    
    if (!ruleId.empty()) {
        cJSON_AddStringToObject(result, "ruleId", ruleId.c_str());
    }
    
    cJSON* messageObj = cJSON_CreateObject();
    cJSON_AddStringToObject(messageObj, "text", message.c_str());
    cJSON_AddItemToObject(result, "message", messageObj);
    
    if (level != Level::Warning) {
        cJSON_AddStringToObject(result, "level", levelToString(level).c_str());
    }
    
    if (!locations.empty()) {
        cJSON* locationsArray = cJSON_CreateArray();
        for (const auto& location : locations) {
            cJSON_AddItemToArray(locationsArray, location.toJson());
        }
        cJSON_AddItemToObject(result, "locations", locationsArray);
    }
    
    if (!relatedLocations.empty()) {
        cJSON* relatedLocationsArray = cJSON_CreateArray();
        for (const auto& location : relatedLocations) {
            cJSON_AddItemToArray(relatedLocationsArray, location.toJson());
        }
        cJSON_AddItemToObject(result, "relatedLocations", relatedLocationsArray);
    }
    
    if (!codeFlows.empty()) {
        cJSON* codeFlowsArray = cJSON_CreateArray();
        for (const auto& codeFlow : codeFlows) {
            cJSON_AddItemToArray(codeFlowsArray, codeFlow.toJson());
        }
        cJSON_AddItemToObject(result, "codeFlows", codeFlowsArray);
    }
    
    return result;
}

// Rule implementation
cJSON* Rule::toJson() const {
    cJSON* rule = cJSON_CreateObject();
    
    if (!id.empty()) {
        cJSON_AddStringToObject(rule, "id", id.c_str());
    }
    
    if (!name.empty()) {
        cJSON_AddStringToObject(rule, "name", name.c_str());
    }
    
    if (!description.empty()) {
        cJSON* shortDesc = cJSON_CreateObject();
        cJSON_AddStringToObject(shortDesc, "text", description.c_str());
        cJSON_AddItemToObject(rule, "shortDescription", shortDesc);
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
    cJSON* doc = toJsonDocument();
    
    char* jsonStr = nullptr;
    if (pretty) {
        jsonStr = cJSON_Print(doc);
    } else {
        jsonStr = cJSON_PrintUnformatted(doc);
    }
    
    std::string result;
    if (jsonStr) {
        result = jsonStr;
        cJSON_free(jsonStr);
    }
    cJSON_Delete(doc);
    
    return result;
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

cJSON* SarifLog::toJsonDocument() const {
    cJSON* doc = cJSON_CreateObject();
    
    cJSON_AddStringToObject(doc, "version", "2.1.0");
    cJSON_AddStringToObject(doc, "$schema", "https://json.schemastore.org/sarif-2.1.0.json");
    
    cJSON* runsArray = cJSON_CreateArray();
    cJSON* run = cJSON_CreateObject();
    
    // Tool information
    cJSON* tool = cJSON_CreateObject();
    cJSON* driver = cJSON_CreateObject();
    cJSON_AddStringToObject(driver, "name", toolName.c_str());
    cJSON_AddStringToObject(driver, "version", toolVersion.c_str());
    
    if (!rules.empty()) {
        cJSON* rulesArray = cJSON_CreateArray();
        for (const auto& rule : rules) {
            cJSON_AddItemToArray(rulesArray, rule.toJson());
        }
        cJSON_AddItemToObject(driver, "rules", rulesArray);
    }
    
    cJSON_AddItemToObject(tool, "driver", driver);
    cJSON_AddItemToObject(run, "tool", tool);
    
    // Results
    if (!results.empty()) {
        cJSON* resultsArray = cJSON_CreateArray();
        for (const auto& result : results) {
            cJSON_AddItemToArray(resultsArray, result.toJson());
        }
        cJSON_AddItemToObject(run, "results", resultsArray);
    }
    
    cJSON_AddItemToArray(runsArray, run);
    cJSON_AddItemToObject(doc, "runs", runsArray);
    
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
            std::string funcName;
            
            // Try to get name from debug info first
            if (auto* subprogram = func->getSubprogram()) {
                funcName = subprogram->getName().str();
            } else {
                funcName = func->getName().str();
            }
            
            // Demangle C++ and Rust function names for better readability
            location.function = DemangleUtils::demangleWithCleanup(funcName);
        }
    }
    
    return location;
}

std::string levelToString(Level level) {
    switch (level) {
        case Level::Note: return "note";
        case Level::Warning: return "warning";
        case Level::Error: return "error";
        default: return "warning";
    }
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

} // namespace sarif
