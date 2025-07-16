#include "Checker/Report/SARIF.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <sstream>
//#include <iomanip>
#include <random>
//#include <chrono>

using namespace sarif;

namespace {
// Use typedef to avoid namespace conflicts
typedef RAPIDJSON_NAMESPACE::Value JsonValue;
typedef RAPIDJSON_NAMESPACE::Document JsonDocument;
typedef RAPIDJSON_NAMESPACE::Document::AllocatorType JsonAllocator;
typedef RAPIDJSON_NAMESPACE::StringBuffer JsonStringBuffer;
typedef RAPIDJSON_NAMESPACE::Writer<JsonStringBuffer> JsonWriter;
typedef RAPIDJSON_NAMESPACE::PrettyWriter<JsonStringBuffer> JsonPrettyWriter;

// Helper function to create JSON string value
JsonValue createJsonString(const std::string& str, JsonAllocator& allocator) {
    JsonValue val;
    val.SetString(str.c_str(), str.length(), allocator);
    return val;
}

// Helper function to convert SarifLevel to string
std::string levelToString(SarifLevel level) {
    switch (level) {
        case SarifLevel::None: return "none";
        case SarifLevel::Note: return "note";
        case SarifLevel::Warning: return "warning";
        case SarifLevel::Error: return "error";
        default: return "warning";
    }
}

// Helper function to convert SarifResultKind to string
std::string resultKindToString(SarifResultKind kind) {
    switch (kind) {
        case SarifResultKind::Fail: return "fail";
        case SarifResultKind::Pass: return "pass";
        case SarifResultKind::Review: return "review";
        case SarifResultKind::Open: return "open";
        case SarifResultKind::NotApplicable: return "notApplicable";
        case SarifResultKind::Informational: return "informational";
        default: return "fail";
    }
}
}

// SarifPhysicalLocation implementation
SarifPhysicalLocation::SarifPhysicalLocation(const std::string& uri, int startLine, int startColumn, 
                                           int endLine, int endColumn)
    : artifactUri(uri), startLine(startLine), startColumn(startColumn), 
      endLine(endLine), endColumn(endColumn) {}

void SarifPhysicalLocation::setArtifactLocation(const std::string& uri) {
    artifactUri = uri;
}

void SarifPhysicalLocation::setRegion(int startLine, int startColumn, int endLine, int endColumn) {
    this->startLine = startLine;
    this->startColumn = startColumn;
    this->endLine = endLine;
    this->endColumn = endColumn;
}

void SarifPhysicalLocation::setContextRegion(int startLine, int startColumn, int endLine, int endColumn) {
    this->contextStartLine = startLine;
    this->contextStartColumn = startColumn;
    this->contextEndLine = endLine;
    this->contextEndColumn = endColumn;
}

void SarifPhysicalLocation::setSnippet(const std::string& text) {
    snippet = text;
}

JsonValue SarifPhysicalLocation::toJson(JsonAllocator& allocator) const {
    JsonValue location(RAPIDJSON_NAMESPACE::kObjectType);
    
    // Artifact location
    if (!artifactUri.empty()) {
        JsonValue artifactLocation(RAPIDJSON_NAMESPACE::kObjectType);
        artifactLocation.AddMember("uri", createJsonString(artifactUri, allocator), allocator);
        location.AddMember("artifactLocation", artifactLocation, allocator);
    }
    
    // Region
    if (startLine > 0) {
        JsonValue region(RAPIDJSON_NAMESPACE::kObjectType);
        region.AddMember("startLine", startLine, allocator);
        if (startColumn > 0) {
            region.AddMember("startColumn", startColumn, allocator);
        }
        if (endLine > 0) {
            region.AddMember("endLine", endLine, allocator);
        }
        if (endColumn > 0) {
            region.AddMember("endColumn", endColumn, allocator);
        }
        if (!snippet.empty()) {
            JsonValue snippetObj(RAPIDJSON_NAMESPACE::kObjectType);
            snippetObj.AddMember("text", createJsonString(snippet, allocator), allocator);
            region.AddMember("snippet", snippetObj, allocator);
        }
        location.AddMember("region", region, allocator);
    }
    
    // Context region
    if (contextStartLine > 0) {
        JsonValue contextRegion(RAPIDJSON_NAMESPACE::kObjectType);
        contextRegion.AddMember("startLine", contextStartLine, allocator);
        if (contextStartColumn > 0) {
            contextRegion.AddMember("startColumn", contextStartColumn, allocator);
        }
        if (contextEndLine > 0) {
            contextRegion.AddMember("endLine", contextEndLine, allocator);
        }
        if (contextEndColumn > 0) {
            contextRegion.AddMember("endColumn", contextEndColumn, allocator);
        }
        location.AddMember("contextRegion", contextRegion, allocator);
    }
    
    return location;
}

// SarifLocation implementation
SarifLocation::SarifLocation(const SarifPhysicalLocation& physicalLocation)
    : physicalLocation(physicalLocation) {}

void SarifLocation::setId(int id) {
    this->id = id;
}

void SarifLocation::setPhysicalLocation(const SarifPhysicalLocation& physicalLocation) {
    this->physicalLocation = physicalLocation;
}

void SarifLocation::setMessage(const std::string& text) {
    message = text;
}

void SarifLocation::setLogicalLocation(const std::string& name, const std::string& fullyQualifiedName, 
                                     const std::string& kind) {
    logicalLocationName = name;
    logicalLocationFullyQualifiedName = fullyQualifiedName;
    logicalLocationKind = kind;
}

JsonValue SarifLocation::toJson(JsonAllocator& allocator) const {
    JsonValue location(RAPIDJSON_NAMESPACE::kObjectType);
    
    if (id.hasValue()) {
        location.AddMember("id", id.getValue(), allocator);
    }
    
    if (physicalLocation.hasValue()) {
        location.AddMember("physicalLocation", physicalLocation->toJson(allocator), allocator);
    }
    
    if (!message.empty()) {
        JsonValue messageObj(RAPIDJSON_NAMESPACE::kObjectType);
        messageObj.AddMember("text", createJsonString(message, allocator), allocator);
        location.AddMember("message", messageObj, allocator);
    }
    
    if (!logicalLocationName.empty()) {
        JsonValue logicalLocation(RAPIDJSON_NAMESPACE::kObjectType);
        logicalLocation.AddMember("name", createJsonString(logicalLocationName, allocator), allocator);
        if (!logicalLocationFullyQualifiedName.empty()) {
            logicalLocation.AddMember("fullyQualifiedName", 
                                    createJsonString(logicalLocationFullyQualifiedName, allocator), allocator);
        }
        if (!logicalLocationKind.empty()) {
            logicalLocation.AddMember("kind", createJsonString(logicalLocationKind, allocator), allocator);
        }
        location.AddMember("logicalLocation", logicalLocation, allocator);
    }
    
    return location;
}

// SarifMessage implementation
SarifMessage::SarifMessage(const std::string& text) : text(text) {}

SarifMessage::SarifMessage(const std::string& text, const std::vector<std::string>& arguments) 
    : text(text), arguments(arguments) {}

void SarifMessage::setText(const std::string& text) {
    this->text = text;
}

void SarifMessage::setMarkdown(const std::string& markdown) {
    this->markdown = markdown;
}

void SarifMessage::setArguments(const std::vector<std::string>& arguments) {
    this->arguments = arguments;
}

void SarifMessage::setId(const std::string& id) {
    this->id = id;
}

JsonValue SarifMessage::toJson(JsonAllocator& allocator) const {
    JsonValue message(RAPIDJSON_NAMESPACE::kObjectType);
    
    if (!text.empty()) {
        message.AddMember("text", createJsonString(text, allocator), allocator);
    }
    
    if (!markdown.empty()) {
        message.AddMember("markdown", createJsonString(markdown, allocator), allocator);
    }
    
    if (!id.empty()) {
        message.AddMember("id", createJsonString(id, allocator), allocator);
    }
    
    if (!arguments.empty()) {
        JsonValue argsArray(RAPIDJSON_NAMESPACE::kArrayType);
        for (const auto& arg : arguments) {
            argsArray.PushBack(createJsonString(arg, allocator), allocator);
        }
        message.AddMember("arguments", argsArray, allocator);
    }
    
    return message;
}

// SarifRule implementation
SarifRule::SarifRule(const std::string& id, const std::string& name) : id(id), name(name) {}

void SarifRule::setId(const std::string& id) {
    this->id = id;
}

void SarifRule::setName(const std::string& name) {
    this->name = name;
}

void SarifRule::setShortDescription(const std::string& text) {
    shortDescription = text;
}

void SarifRule::setFullDescription(const std::string& text) {
    fullDescription = text;
}

void SarifRule::setHelpUri(const std::string& uri) {
    helpUri = uri;
}

void SarifRule::setDefaultLevel(SarifLevel level) {
    defaultLevel = level;
}

void SarifRule::addTag(const std::string& tag) {
    tags.push_back(tag);
}

JsonValue SarifRule::toJson(JsonAllocator& allocator) const {
    JsonValue rule(RAPIDJSON_NAMESPACE::kObjectType);
    
    if (!id.empty()) {
        rule.AddMember("id", createJsonString(id, allocator), allocator);
    }
    
    if (!name.empty()) {
        rule.AddMember("name", createJsonString(name, allocator), allocator);
    }
    
    if (!shortDescription.empty()) {
        JsonValue shortDesc(RAPIDJSON_NAMESPACE::kObjectType);
        shortDesc.AddMember("text", createJsonString(shortDescription, allocator), allocator);
        rule.AddMember("shortDescription", shortDesc, allocator);
    }
    
    if (!fullDescription.empty()) {
        JsonValue fullDesc(RAPIDJSON_NAMESPACE::kObjectType);
        fullDesc.AddMember("text", createJsonString(fullDescription, allocator), allocator);
        rule.AddMember("fullDescription", fullDesc, allocator);
    }
    
    if (!helpUri.empty()) {
        rule.AddMember("helpUri", createJsonString(helpUri, allocator), allocator);
    }
    
    if (defaultLevel != SarifLevel::Warning) {
        rule.AddMember("defaultLevel", createJsonString(levelToString(defaultLevel), allocator), allocator);
    }
    
    if (!tags.empty()) {
        JsonValue tagsArray(RAPIDJSON_NAMESPACE::kArrayType);
        for (const auto& tag : tags) {
            tagsArray.PushBack(createJsonString(tag, allocator), allocator);
        }
        rule.AddMember("tags", tagsArray, allocator);
    }
    
    return rule;
}

// Simplified implementations for remaining classes...
// SarifCodeFlow implementation
void SarifCodeFlow::setMessage(const std::string& text) {
    message = text;
}

void SarifCodeFlow::addLocation(const SarifLocation& location, const std::string& message) {
    if (threadFlows.empty()) {
        threadFlows.emplace_back();
    }
    threadFlows[0].push_back(location);
}

void SarifCodeFlow::addThreadFlow(const std::vector<SarifLocation>& locations) {
    threadFlows.push_back(locations);
}

JsonValue SarifCodeFlow::toJson(JsonAllocator& allocator) const {
    JsonValue codeFlow(RAPIDJSON_NAMESPACE::kObjectType);
    
    if (!message.empty()) {
        JsonValue messageObj(RAPIDJSON_NAMESPACE::kObjectType);
        messageObj.AddMember("text", createJsonString(message, allocator), allocator);
        codeFlow.AddMember("message", messageObj, allocator);
    }
    
    JsonValue threadFlowsArray(RAPIDJSON_NAMESPACE::kArrayType);
    for (const auto& threadFlow : threadFlows) {
        JsonValue threadFlowObj(RAPIDJSON_NAMESPACE::kObjectType);
        JsonValue locationsArray(RAPIDJSON_NAMESPACE::kArrayType);
        
        for (const auto& location : threadFlow) {
            JsonValue threadFlowLocation(RAPIDJSON_NAMESPACE::kObjectType);
            threadFlowLocation.AddMember("location", location.toJson(allocator), allocator);
            locationsArray.PushBack(threadFlowLocation, allocator);
        }
        
        threadFlowObj.AddMember("locations", locationsArray, allocator);
        threadFlowsArray.PushBack(threadFlowObj, allocator);
    }
    
    codeFlow.AddMember("threadFlows", threadFlowsArray, allocator);
    
    return codeFlow;
}

// SarifResult implementation
SarifResult::SarifResult(const std::string& ruleId, const SarifMessage& message) 
    : ruleId(ruleId), message(message) {}

void SarifResult::setRuleId(const std::string& ruleId) {
    this->ruleId = ruleId;
}

void SarifResult::setRuleIndex(int index) {
    ruleIndex = index;
}

void SarifResult::setMessage(const SarifMessage& message) {
    this->message = message;
}

void SarifResult::setLevel(SarifLevel level) {
    this->level = level;
}

void SarifResult::setKind(SarifResultKind kind) {
    this->kind = kind;
}

void SarifResult::addLocation(const SarifLocation& location) {
    locations.push_back(location);
}

void SarifResult::addRelatedLocation(const SarifLocation& location) {
    relatedLocations.push_back(location);
}

void SarifResult::setCodeFlow(const SarifCodeFlow& codeFlow) {
    this->codeFlow = codeFlow;
}

JsonValue SarifResult::toJson(JsonAllocator& allocator) const {
    JsonValue result(RAPIDJSON_NAMESPACE::kObjectType);
    
    if (!ruleId.empty()) {
        result.AddMember("ruleId", createJsonString(ruleId, allocator), allocator);
    }
    
    if (ruleIndex >= 0) {
        result.AddMember("ruleIndex", ruleIndex, allocator);
    }
    
    result.AddMember("message", message.toJson(allocator), allocator);
    
    if (level != SarifLevel::Warning) {
        result.AddMember("level", createJsonString(levelToString(level), allocator), allocator);
    }
    
    if (kind != SarifResultKind::Fail) {
        result.AddMember("kind", createJsonString(resultKindToString(kind), allocator), allocator);
    }
    
    if (!locations.empty()) {
        JsonValue locationsArray(RAPIDJSON_NAMESPACE::kArrayType);
        for (const auto& location : locations) {
            locationsArray.PushBack(location.toJson(allocator), allocator);
        }
        result.AddMember("locations", locationsArray, allocator);
    }
    
    if (!relatedLocations.empty()) {
        JsonValue relatedLocationsArray(RAPIDJSON_NAMESPACE::kArrayType);
        for (const auto& location : relatedLocations) {
            relatedLocationsArray.PushBack(location.toJson(allocator), allocator);
        }
        result.AddMember("relatedLocations", relatedLocationsArray, allocator);
    }
    
    if (codeFlow.hasValue()) {
        JsonValue codeFlowsArray(RAPIDJSON_NAMESPACE::kArrayType);
        codeFlowsArray.PushBack(codeFlow->toJson(allocator), allocator);
        result.AddMember("codeFlows", codeFlowsArray, allocator);
    }
    
    return result;
}

// SarifToolComponent implementation
SarifToolComponent::SarifToolComponent(const std::string& name, const std::string& version) 
    : name(name), version(version) {}

void SarifToolComponent::setName(const std::string& name) {
    this->name = name;
}

void SarifToolComponent::setVersion(const std::string& version) {
    this->version = version;
}

void SarifToolComponent::addRule(const SarifRule& rule) {
    rules.push_back(rule);
}

JsonValue SarifToolComponent::toJson(JsonAllocator& allocator) const {
    JsonValue component(RAPIDJSON_NAMESPACE::kObjectType);
    
    if (!name.empty()) {
        component.AddMember("name", createJsonString(name, allocator), allocator);
    }
    
    if (!version.empty()) {
        component.AddMember("version", createJsonString(version, allocator), allocator);
    }
    
    if (!rules.empty()) {
        JsonValue rulesArray(RAPIDJSON_NAMESPACE::kArrayType);
        for (const auto& rule : rules) {
            rulesArray.PushBack(rule.toJson(allocator), allocator);
        }
        component.AddMember("rules", rulesArray, allocator);
    }
    
    return component;
}

// SarifTool implementation
SarifTool::SarifTool(const SarifToolComponent& driver) : driver(driver) {}

void SarifTool::setDriver(const SarifToolComponent& driver) {
    this->driver = driver;
}

JsonValue SarifTool::toJson(JsonAllocator& allocator) const {
    JsonValue tool(RAPIDJSON_NAMESPACE::kObjectType);
    
    tool.AddMember("driver", driver.toJson(allocator), allocator);
    
    return tool;
}

// SarifRun implementation
SarifRun::SarifRun(const SarifTool& tool) : tool(tool) {}

void SarifRun::setTool(const SarifTool& tool) {
    this->tool = tool;
}

void SarifRun::addResult(const SarifResult& result) {
    results.push_back(result);
}

JsonValue SarifRun::toJson(JsonAllocator& allocator) const {
    JsonValue run(RAPIDJSON_NAMESPACE::kObjectType);
    
    run.AddMember("tool", tool.toJson(allocator), allocator);
    
    if (!results.empty()) {
        JsonValue resultsArray(RAPIDJSON_NAMESPACE::kArrayType);
        for (const auto& result : results) {
            resultsArray.PushBack(result.toJson(allocator), allocator);
        }
        run.AddMember("results", resultsArray, allocator);
    }
    
    return run;
}

// SarifLog implementation
SarifLog::SarifLog() : version("2.1.0"), schema("https://json.schemastore.org/sarif-2.1.0.json") {}

void SarifLog::addRun(const SarifRun& run) {
    runs.push_back(run);
}

void SarifLog::setVersion(const std::string& version) {
    this->version = version;
}

void SarifLog::setSchema(const std::string& schema) {
    this->schema = schema;
}

std::string SarifLog::toJsonString(bool pretty) const {
    JsonDocument doc = toJsonDocument();
    
    JsonStringBuffer buffer;
    if (pretty) {
        JsonPrettyWriter writer(buffer);
        doc.Accept(writer);
    } else {
        JsonWriter writer(buffer);
        doc.Accept(writer);
    }
    
    return buffer.GetString();
}

void SarifLog::writeToStream(llvm::raw_ostream& os, bool pretty) const {
    os << toJsonString(pretty);
}

bool SarifLog::writeToFile(const std::string& filename, bool pretty) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    file << toJsonString(pretty);
    file.close();
    return true;
}

JsonDocument SarifLog::toJsonDocument() const {
    JsonDocument doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();
    
    doc.AddMember("version", createJsonString(version, allocator), allocator);
    doc.AddMember("$schema", createJsonString(schema, allocator), allocator);
    
    JsonValue runsArray(RAPIDJSON_NAMESPACE::kArrayType);
    for (const auto& run : runs) {
        runsArray.PushBack(run.toJson(allocator), allocator);
    }
    doc.AddMember("runs", runsArray, allocator);
    
    return doc;
}

// Utility functions implementation
namespace utils {

SarifPhysicalLocation createPhysicalLocationFromDebugLoc(const llvm::DebugLoc& debugLoc) {
    SarifPhysicalLocation location;
    
    if (debugLoc) {
        location.setRegion(debugLoc.getLine(), debugLoc.getCol());
        
        if (auto* scope = debugLoc.getScope()) {
            if (auto* diScope = llvm::dyn_cast<llvm::DIScope>(scope)) {
                std::string uri = diScope->getDirectory().str() + "/" + diScope->getFilename().str();
                location.setArtifactLocation(uri);
            }
        }
    }
    
    return location;
}

SarifLocation createLocationFromInstruction(const llvm::Instruction* instruction) {
    SarifLocation location;
    
    if (instruction && instruction->getDebugLoc()) {
        SarifPhysicalLocation physLoc = createPhysicalLocationFromDebugLoc(instruction->getDebugLoc());
        location.setPhysicalLocation(physLoc);
        
        if (auto* func = instruction->getFunction()) {
            location.setLogicalLocation(func->getName().str(), func->getName().str(), "function");
        }
    }
    
    return location;
}

SarifMessage createFormattedMessage(const std::string& format, const std::vector<std::string>& args) {
    SarifMessage message;
    message.setText(format);
    message.setArguments(args);
    return message;
}

SarifLevel stringToSarifLevel(const std::string& level) {
    if (level == "none") return SarifLevel::None;
    if (level == "note") return SarifLevel::Note;
    if (level == "warning") return SarifLevel::Warning;
    if (level == "error") return SarifLevel::Error;
    return SarifLevel::Warning;
}

std::string sarifLevelToString(SarifLevel level) {
    return levelToString(level);
}

std::string generateUuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    ss << std::hex;
    
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            ss << "-";
        }
        ss << dis(gen);
    }
    
    return ss.str();
}

SarifToolComponent createLotusToolComponent() {
    SarifToolComponent tool("Lotus", "1.0.0");
    return tool;
}

SarifRule createRuleFromChecker(const std::string& checkerId, const std::string& checkerName,
                               const std::string& description, const std::string& helpUri) {
    SarifRule rule(checkerId, checkerName);
    rule.setShortDescription(checkerName);
    rule.setFullDescription(description);
    if (!helpUri.empty()) {
        rule.setHelpUri(helpUri);
    }
    return rule;
}

} // namespace utils

// SarifBuilder implementation
SarifBuilder::SarifBuilder() {
    toolComponent = utils::createLotusToolComponent();
}

SarifBuilder& SarifBuilder::withTool(const std::string& name, const std::string& version) {
    toolComponent.setName(name);
    toolComponent.setVersion(version);
    return *this;
}

SarifBuilder& SarifBuilder::addRule(const std::string& id, const std::string& name, 
                                   const std::string& description) {
    SarifRule rule(id, name);
    rule.setShortDescription(name);
    rule.setFullDescription(description);
    rules.push_back(rule);
    toolComponent.addRule(rule);
    return *this;
}

SarifBuilder& SarifBuilder::addResult(const std::string& ruleId, const std::string& message,
                                     const std::string& uri, int line, int column) {
    SarifMessage msg(message);
    SarifResult result(ruleId, msg);
    
    SarifPhysicalLocation physLoc(uri, line, column);
    SarifLocation location(physLoc);
    result.addLocation(location);
    
    results.push_back(result);
    return *this;
}

SarifLog SarifBuilder::build() {
    SarifLog log;
    SarifTool tool(toolComponent);
    SarifRun run(tool);
    
    for (const auto& result : results) {
        run.addResult(result);
    }
    
    log.addRun(run);
    return log;
}
