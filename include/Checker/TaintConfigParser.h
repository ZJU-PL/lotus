/*
 * Taint Configuration Parser
 * 
 * Simple parser for taint specification files (.spec format)
 */

#pragma once

#include <string>
#include <unordered_set>
#include <memory>
#include <llvm/Support/raw_ostream.h>

namespace checker {

// Simple taint configuration
class TaintConfig {
public:
    std::unordered_set<std::string> sources;
    std::unordered_set<std::string> sinks;
    std::unordered_set<std::string> ignored;
    
    bool is_source(const std::string& func) const { return sources.count(func); }
    bool is_sink(const std::string& func) const { return sinks.count(func); }
    bool is_ignored(const std::string& func) const { return ignored.count(func); }
    
    void dump(llvm::raw_ostream& OS) const;
};

// Simple parser
class TaintConfigParser {
public:
    static std::unique_ptr<TaintConfig> parse_file(const std::string& filename);
    static std::unique_ptr<TaintConfig> parse_string(const std::string& content);
    
private:
    static void parse_line(const std::string& line, TaintConfig& config);
    static std::vector<std::string> split(const std::string& str);
    static std::string trim(const std::string& str);
};

} // namespace checker
