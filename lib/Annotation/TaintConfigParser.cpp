/*
 * Taint Configuration Parser Implementation
 */

#include "Annotation/Taint/TaintConfigParser.h"
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <sstream>
//#include <algorithm>

void TaintConfig::dump(llvm::raw_ostream& OS) const {
    OS << "Sources: " << sources.size() << ", Sinks: " << sinks.size() 
       << ", Ignored: " << ignored.size() 
       << ", Function specs: " << function_specs.size() << "\n";
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

std::unique_ptr<TaintConfig> TaintConfigParser::parse_file_quiet(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
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
        
        // Parse taint specifications: SOURCE func_name Arg0 D T
        // Format: <location> <access_mode> <taint_type>
        for (size_t i = 2; i < tokens.size(); i += 3) {
            if (i + 2 < tokens.size()) {
                TaintSpec spec;
                if (parse_taint_spec(tokens, i, spec)) {
                    config.function_specs[func_name].source_specs.push_back(spec);
                }
            }
        }
    } else if (directive == "SINK") {
        config.sinks.insert(func_name);
        
        // Parse sink specifications similarly
        for (size_t i = 2; i < tokens.size(); i += 3) {
            if (i + 2 < tokens.size()) {
                TaintSpec spec;
                if (parse_taint_spec(tokens, i, spec)) {
                    config.function_specs[func_name].sink_specs.push_back(spec);
                }
            }
        }
    } else if (directive == "IGNORE") {
        config.ignored.insert(func_name);
    } else if (directive == "PIPE") {
        // Parse pipe specifications: PIPE func_name Ret V Arg0 D
        // Format: <from_location> <from_access> <to_location> <to_access>
        if (tokens.size() >= 6) {
            PipeSpec pipe_spec;
            // Parse 'from' spec (tokens 2-3, assuming taint type is implicit)
            std::vector<std::string> from_tokens = {tokens[2], tokens[3], "T"};
            if (parse_taint_spec(from_tokens, 0, pipe_spec.from)) {
                std::vector<std::string> to_tokens = {tokens[4], tokens[5], "T"};
                if (parse_taint_spec(to_tokens, 0, pipe_spec.to)) {
                    config.function_specs[func_name].pipe_specs.push_back(pipe_spec);
                }
            }
        }
    }
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

bool TaintConfigParser::parse_taint_spec(const std::vector<std::string>& tokens, size_t start_idx, TaintSpec& spec) {
    if (start_idx + 2 >= tokens.size()) return false;
    
    const std::string& location_token = tokens[start_idx];
    const std::string& access_token = tokens[start_idx + 1];
    const std::string& taint_token = tokens[start_idx + 2];
    
    // Parse location (Arg0, AfterArg1, Ret, etc.)
    if (location_token == "Ret") {
        spec.location = TaintSpec::RET;
        spec.arg_index = -1;
    } else if (location_token.substr(0, 8) == "AfterArg") {
        spec.location = TaintSpec::AFTER_ARG;
        try {
            spec.arg_index = std::stoi(location_token.substr(8));
        } catch (...) {
            return false;
        }
    } else if (location_token.substr(0, 3) == "Arg") {
        spec.location = TaintSpec::ARG;
        try {
            spec.arg_index = std::stoi(location_token.substr(3));
        } catch (...) {
            return false;
        }
    } else {
        return false;
    }
    
    // Parse access mode (V = value, D = deref, R = deref/read)
    if (access_token == "V") {
        spec.access_mode = TaintSpec::VALUE;
    } else if (access_token == "D" || access_token == "R") {
        spec.access_mode = TaintSpec::DEREF;
    } else {
        return false;
    }
    
    // Parse taint type (T = tainted, U = uninitialized)
    if (taint_token == "T") {
        spec.taint_type = TaintSpec::TAINTED;
    } else if (taint_token == "U") {
        spec.taint_type = TaintSpec::UNINITIALIZED;
    } else {
        return false;
    }
    
    return true;
}
