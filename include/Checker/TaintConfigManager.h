/*
 * Taint Configuration Manager
 * 
 * Simple singleton for managing taint configurations
 */

#pragma once

#include "Checker/TaintConfigParser.h"
#include <llvm/IR/Instructions.h>
#include <memory>
#include <string>

namespace checker {

// Simple singleton manager
class TaintConfigManager {
private:
    static std::unique_ptr<TaintConfigManager> instance;
    std::unique_ptr<TaintConfig> config;
    
public:
    TaintConfigManager() {}
    
    static TaintConfigManager& getInstance() {
        if (!instance) {
            instance = std::make_unique<TaintConfigManager>();
        }
        return *instance;
    }
    
    bool load_config(const std::string& config_file) {
        config = TaintConfigParser::parse_file(config_file);
        return config != nullptr;
    }
    
    bool load_default_config() {
        return load_config("config/taint.spec");
    }
    
    bool is_source(const std::string& func_name) const {
        return config && config->is_source(func_name);
    }
    
    bool is_sink(const std::string& func_name) const {
        return config && config->is_sink(func_name);
    }
    
    bool is_ignored(const std::string& func_name) const {
        return config && config->is_ignored(func_name);
    }
    
    bool is_source(const llvm::CallInst* call) const {
        if (!call) return false;
        const llvm::Function* callee = call->getCalledFunction();
        return callee && is_source(callee->getName().str());
    }
    
    bool is_sink(const llvm::CallInst* call) const {
        if (!call) return false;
        const llvm::Function* callee = call->getCalledFunction();
        return callee && is_sink(callee->getName().str());
    }
    
    void dump_config(llvm::raw_ostream& OS) const {
        if (config) config->dump(OS);
    }
    
    size_t get_source_count() const {
        return config ? config->sources.size() : 0;
    }
    
    size_t get_sink_count() const {
        return config ? config->sinks.size() : 0;
    }
    
    std::vector<std::string> get_all_source_functions() const {
        std::vector<std::string> result;
        if (config) {
            for (const auto& func : config->sources) {
                result.push_back(func);
            }
        }
        return result;
    }
    
    std::vector<std::string> get_all_sink_functions() const {
        std::vector<std::string> result;
        if (config) {
            for (const auto& func : config->sinks) {
                result.push_back(func);
            }
        }
        return result;
    }
};

// Convenience namespace
namespace taint_config {
    inline bool is_source(const std::string& func_name) {
        return TaintConfigManager::getInstance().is_source(func_name);
    }
    
    inline bool is_sink(const std::string& func_name) {
        return TaintConfigManager::getInstance().is_sink(func_name);
    }
    
    inline bool is_ignored(const std::string& func_name) {
        return TaintConfigManager::getInstance().is_ignored(func_name);
    }
    
    inline bool is_source(const llvm::CallInst* call) {
        return TaintConfigManager::getInstance().is_source(call);
    }
    
    inline bool is_sink(const llvm::CallInst* call) {
        return TaintConfigManager::getInstance().is_sink(call);
    }
    
    inline bool load_config(const std::string& config_file) {
        return TaintConfigManager::getInstance().load_config(config_file);
    }
    
    inline bool load_default_config() {
        return TaintConfigManager::getInstance().load_default_config();
    }
    
    inline void dump_config(llvm::raw_ostream& OS) {
        TaintConfigManager::getInstance().dump_config(OS);
    }
    
    inline size_t get_source_count() {
        return TaintConfigManager::getInstance().get_source_count();
    }
    
    inline size_t get_sink_count() {
        return TaintConfigManager::getInstance().get_sink_count();
    }
}

} // namespace checker
