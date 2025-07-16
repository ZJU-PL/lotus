#ifndef SARIF_H
#define SARIF_H

#include <string>
#include <vector>
//#include <memory>
#include <map>

#include <llvm/IR/DebugLoc.h>
#include <llvm/ADT/Optional.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Support/rapidjson/document.h"
#include "Support/rapidjson/writer.h"
#include "Support/rapidjson/stringbuffer.h"
#include "Support/rapidjson/prettywriter.h"

namespace sarif {

// Forward declarations
class SarifLog;
class SarifRun;
class SarifResult;
class SarifLocation;
class SarifMessage;
class SarifRule;
class SarifTool;

// SARIF severity levels
enum class SarifLevel {
    None,
    Note,
    Warning,
    Error
};

// SARIF result kinds
enum class SarifResultKind {
    Fail,
    Pass,
    Review,
    Open,
    NotApplicable,
    Informational
};

// SARIF physical location representation
class SarifPhysicalLocation {
public:
    SarifPhysicalLocation() = default;
    SarifPhysicalLocation(const std::string& uri, int startLine = 0, int startColumn = 0, 
                         int endLine = 0, int endColumn = 0);
    
    void setArtifactLocation(const std::string& uri);
    void setRegion(int startLine, int startColumn, int endLine = 0, int endColumn = 0);
    void setContextRegion(int startLine, int startColumn, int endLine = 0, int endColumn = 0);
    void setSnippet(const std::string& text);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    std::string artifactUri;
    int startLine = 0;
    int startColumn = 0;
    int endLine = 0;
    int endColumn = 0;
    int contextStartLine = 0;
    int contextStartColumn = 0;
    int contextEndLine = 0;
    int contextEndColumn = 0;
    std::string snippet;
};

// SARIF location representation
class SarifLocation {
public:
    SarifLocation() = default;
    SarifLocation(const SarifPhysicalLocation& physicalLocation);
    
    void setId(int id);
    void setPhysicalLocation(const SarifPhysicalLocation& physicalLocation);
    void setMessage(const std::string& text);
    void setLogicalLocation(const std::string& name, const std::string& fullyQualifiedName = "", 
                           const std::string& kind = "");
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    llvm::Optional<int> id;
    llvm::Optional<SarifPhysicalLocation> physicalLocation;
    std::string message;
    std::string logicalLocationName;
    std::string logicalLocationFullyQualifiedName;
    std::string logicalLocationKind;
};

// SARIF message representation
class SarifMessage {
public:
    SarifMessage() = default;
    SarifMessage(const std::string& text);
    SarifMessage(const std::string& text, const std::vector<std::string>& arguments);
    
    void setText(const std::string& text);
    void setMarkdown(const std::string& markdown);
    void setArguments(const std::vector<std::string>& arguments);
    void setId(const std::string& id);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    std::string text;
    std::string markdown;
    std::vector<std::string> arguments;
    std::string id;
};

// SARIF rule representation
class SarifRule {
public:
    SarifRule() = default;
    SarifRule(const std::string& id, const std::string& name);
    
    void setId(const std::string& id);
    void setName(const std::string& name);
    void setShortDescription(const std::string& text);
    void setFullDescription(const std::string& text);
    void setHelpUri(const std::string& uri);
    void setDefaultLevel(SarifLevel level);
    void addTag(const std::string& tag);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    std::string id;
    std::string name;
    std::string shortDescription;
    std::string fullDescription;
    std::string helpUri;
    SarifLevel defaultLevel = SarifLevel::Warning;
    std::vector<std::string> tags;
};

// SARIF code flow representation for data flow analysis
class SarifCodeFlow {
public:
    SarifCodeFlow() = default;
    
    void setMessage(const std::string& text);
    void addLocation(const SarifLocation& location, const std::string& message = "");
    void addThreadFlow(const std::vector<SarifLocation>& locations);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    std::string message;
    std::vector<std::vector<SarifLocation>> threadFlows;
};

// SARIF result representation
class SarifResult {
public:
    SarifResult() = default;
    SarifResult(const std::string& ruleId, const SarifMessage& message);
    
    void setRuleId(const std::string& ruleId);
    void setRuleIndex(int index);
    void setMessage(const SarifMessage& message);
    void setLevel(SarifLevel level);
    void setKind(SarifResultKind kind);
    void addLocation(const SarifLocation& location);
    void addRelatedLocation(const SarifLocation& location);
    void setCodeFlow(const SarifCodeFlow& codeFlow);
    void setGuid(const std::string& guid);
    void setCorrelationGuid(const std::string& guid);
    void addFingerprint(const std::string& name, const std::string& value);
    void addPartialFingerprint(const std::string& name, const std::string& value);
    void addProperty(const std::string& name, const std::string& value);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    std::string ruleId;
    int ruleIndex = -1;
    SarifMessage message;
    SarifLevel level = SarifLevel::Warning;
    SarifResultKind kind = SarifResultKind::Fail;
    std::vector<SarifLocation> locations;
    std::vector<SarifLocation> relatedLocations;
    llvm::Optional<SarifCodeFlow> codeFlow;
    std::string guid;
    std::string correlationGuid;
    std::map<std::string, std::string> fingerprints;
    std::map<std::string, std::string> partialFingerprints;
    std::map<std::string, std::string> properties;
};

// SARIF tool component representation
class SarifToolComponent {
public:
    SarifToolComponent() = default;
    SarifToolComponent(const std::string& name, const std::string& version);
    
    void setName(const std::string& name);
    void setVersion(const std::string& version);
    void setSemanticVersion(const std::string& version);
    void setOrganization(const std::string& organization);
    void setProduct(const std::string& product);
    void setShortDescription(const std::string& text);
    void setFullDescription(const std::string& text);
    void setInformationUri(const std::string& uri);
    void setDownloadUri(const std::string& uri);
    void addRule(const SarifRule& rule);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    std::string name;
    std::string version;
    std::string semanticVersion;
    std::string organization;
    std::string product;
    std::string shortDescription;
    std::string fullDescription;
    std::string informationUri;
    std::string downloadUri;
    std::vector<SarifRule> rules;
};

// SARIF tool representation
class SarifTool {
public:
    SarifTool() = default;
    SarifTool(const SarifToolComponent& driver);
    
    void setDriver(const SarifToolComponent& driver);
    void addExtension(const SarifToolComponent& extension);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    SarifToolComponent driver;
    std::vector<SarifToolComponent> extensions;
};

// SARIF run representation
class SarifRun {
public:
    SarifRun() = default;
    SarifRun(const SarifTool& tool);
    
    void setTool(const SarifTool& tool);
    void addResult(const SarifResult& result);
    void setBaselineGuid(const std::string& guid);
    void setAutomationDetailsId(const std::string& id);
    void setAutomationDetailsGuid(const std::string& guid);
    void setAutomationDetailsCorrelationGuid(const std::string& guid);
    void addOriginalUriBaseId(const std::string& key, const std::string& uri);
    void addArtifact(const std::string& uri, const std::string& mimeType = "", 
                    const std::string& sourceLanguage = "");
    void setColumnKind(const std::string& kind);
    void addProperty(const std::string& name, const std::string& value);
    
    rapidjson::Value toJson(rapidjson::Document::AllocatorType& allocator) const;

private:
    SarifTool tool;
    std::vector<SarifResult> results;
    std::string baselineGuid;
    std::string automationDetailsId;
    std::string automationDetailsGuid;
    std::string automationDetailsCorrelationGuid;
    std::map<std::string, std::string> originalUriBaseIds;
    std::vector<std::map<std::string, std::string>> artifacts;
    std::string columnKind = "utf16CodeUnits";
    std::map<std::string, std::string> properties;
};

// Main SARIF log representation
class SarifLog {
public:
    SarifLog();
    
    void addRun(const SarifRun& run);
    void setVersion(const std::string& version);
    void setSchema(const std::string& schema);
    
    // Generate JSON string representation
    std::string toJsonString(bool pretty = true) const;
    
    // Write to output stream
    void writeToStream(llvm::raw_ostream& os, bool pretty = true) const;
    
    // Write to file
    bool writeToFile(const std::string& filename, bool pretty = true) const;
    
    // Get JSON document
    rapidjson::Document toJsonDocument() const;

private:
    std::string version;
    std::string schema;
    std::vector<SarifRun> runs;
};

// Utility functions for converting LLVM structures to SARIF
namespace utils {

// Convert LLVM debug location to SARIF physical location
SarifPhysicalLocation createPhysicalLocationFromDebugLoc(const llvm::DebugLoc& debugLoc);

// Convert LLVM instruction to SARIF location
SarifLocation createLocationFromInstruction(const llvm::Instruction* instruction);

// Convert LLVM function to SARIF logical location
SarifLocation createLogicalLocationFromFunction(const llvm::Function* function);

// Create SARIF message from format string and arguments
SarifMessage createFormattedMessage(const std::string& format, 
                                   const std::vector<std::string>& args);

// Convert severity level string to SARIF level
SarifLevel stringToSarifLevel(const std::string& level);

// Convert SARIF level to string
std::string sarifLevelToString(SarifLevel level);

// Generate UUID for GUID fields
std::string generateUuid();

// Create default tool component for Lotus framework
SarifToolComponent createLotusToolComponent();

// Create SARIF rule from checker information
SarifRule createRuleFromChecker(const std::string& checkerId, 
                               const std::string& checkerName,
                               const std::string& description,
                               const std::string& helpUri = "");

} // namespace utils

// Builder pattern for easier SARIF construction
class SarifBuilder {
public:
    SarifBuilder();
    
    // Tool configuration
    SarifBuilder& withTool(const std::string& name, const std::string& version);
    SarifBuilder& withOrganization(const std::string& organization);
    SarifBuilder& withProduct(const std::string& product);
    
    // Rule configuration
    SarifBuilder& addRule(const std::string& id, const std::string& name, 
                         const std::string& description);
    
    // Result creation
    SarifBuilder& addResult(const std::string& ruleId, const std::string& message,
                           const std::string& uri, int line, int column);
    
    // Advanced result creation with code flow
    SarifBuilder& addResultWithCodeFlow(const std::string& ruleId, 
                                       const std::string& message,
                                       const std::vector<SarifLocation>& codeFlow);
    
    // Build final SARIF log
    SarifLog build();

private:
    SarifToolComponent toolComponent;
    std::vector<SarifRule> rules;
    std::vector<SarifResult> results;
    std::map<std::string, std::string> properties;
};

} // namespace sarif

#endif // SARIF_H
