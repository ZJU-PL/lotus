/*
 * Taint Configuration Parser Implementation
 */

#include "Checker/TaintConfigParser.h"
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <sstream>
// #include <algorithm>

namespace checker {

void TaintConfig::dump(llvm::raw_ostream& OS) const {
    OS << "Sources: " << sources.size() << ", Sinks: " << sinks.size() 
       << ", Ignored: " << ignored.size() << "\n";
}

std::unique_ptr<TaintConfig> TaintConfigParser::parse_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        llvm::errs() << "Error: Could not open taint config file: " << filename << "\n";
        return nullptr;
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    return parse_string(content);
}

std::unique_ptr<TaintConfig> TaintConfigParser::parse_string(const std::string& content) {
    auto config = std::make_unique<TaintConfig>();
    std::istringstream stream(content);
    std::string line;
    
    while (std::getline(stream, line)) {
        parse_line(trim(line), *config);
    }
    
    return config;
}

void TaintConfigParser::parse_line(const std::string& line, TaintConfig& config) {
    if (line.empty() || line[0] == '#') return;
    
    auto tokens = split(line);
    if (tokens.size() < 2) return;
    
    std::string directive = tokens[0];
    std::string func_name = tokens[1];
    
    if (directive == "SOURCE") {
        config.sources.insert(func_name);
    } else if (directive == "SINK") {
        config.sinks.insert(func_name);
    } else if (directive == "IGNORE") {
        config.ignored.insert(func_name);
    }
    // PIPE directives are ignored for now
}

std::vector<std::string> TaintConfigParser::split(const std::string& str) {
    std::vector<std::string> tokens;
    std::istringstream stream(str);
    std::string token;
    
    while (stream >> token) {
        tokens.push_back(token);
    }
    
    return tokens;
}

std::string TaintConfigParser::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

} // namespace checker
