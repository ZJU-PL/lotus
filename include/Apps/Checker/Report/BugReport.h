#ifndef CHECKER_REPORT_BUGREPORT_H
#define CHECKER_REPORT_BUGREPORT_H

#include "Apps/Checker/Report/BugTypes.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace llvm {
class Instruction;
}

/**
 * BugDiagStep describes a single step in the bug diagnostic trace.
 * A bug report consists of one or more diagnostic steps showing
 * how the bug manifests.
 */
struct BugDiagStep {
    // The LLVM instruction or value for this diagnostic step
    llvm::Value* inst = nullptr;
    
    // Source file location
    std::string src_file;
    int src_line = 0;
    int src_column = 0;
    
    // Human-readable description of what happens at this step
    std::string tip;
    
    // Function containing this instruction
    std::string func_name;
    
    // LLVM IR representation
    std::string llvm_ir;
    
    // Variable/pointer name (if available from debug info or instruction name)
    std::string var_name;
    
    // Type information for the value
    std::string type_name;
    
    // Actual source code line (if available)
    std::string source_code;
};

/**
 * BugReport represents a complete bug with diagnostic trace.
 * Follows Clearblue's pattern of reporting bugs as sequences of steps.
 */
class BugReport {
public:
    BugReport(int bug_type_id) 
        : bug_type_id(bug_type_id), dominated(false), valid(true), conf_score(100) {}
    
    ~BugReport() {
        for (BugDiagStep* step : trigger_steps)
            delete step;
    }
    
    // Add a diagnostic step to the trace
    void append_step(BugDiagStep* step) {
        trigger_steps.push_back(step);
    }
    
    // Create and append a diagnostic step
    void append_step(llvm::Value* inst, const std::string& tip);
    
    // Get the bug type ID
    int get_bug_type_id() const { return bug_type_id; }
    
    // Get all diagnostic steps
    const std::vector<BugDiagStep*>& get_steps() const { return trigger_steps; }
    
    // Dominated flag (for ranking/filtering)
    bool is_dominated() const { return dominated; }
    void set_dominated(bool val) { dominated = val; }
    
    // Valid flag (whether this report is considered valid)
    bool is_valid() const { return valid; }
    void set_valid(bool val) { valid = val; }
    
    // Confidence score [0-100]
    int get_conf_score() const { return conf_score; }
    void set_conf_score(int score) { conf_score = score; }
    
    // Export to JSON format
    void export_json(llvm::raw_ostream& OS) const;
    
private:
    int bug_type_id;
    std::vector<BugDiagStep*> trigger_steps;
    bool dominated;
    bool valid;
    int conf_score;
};

#endif // CHECKER_REPORT_BUGREPORT_H

