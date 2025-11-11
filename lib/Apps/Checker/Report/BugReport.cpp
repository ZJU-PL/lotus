#include "Apps/Checker/Report/BugReport.h"
#include "Apps/Checker/Report/DebugInfoAnalysis.h"
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>
#include <fstream>
#include <sstream>
#include <map>
#include <unistd.h>

using namespace llvm;

// Shared DebugInfoAnalysis instance (one for all bug reports)
static DebugInfoAnalysis debugInfo;

void BugReport::append_step(Value* inst, const std::string& tip) {
    BugDiagStep* step = new BugDiagStep();
    step->inst = inst;
    step->tip = tip;
    
    // Extract LLVM IR representation
    if (inst) {
        std::string ir_str;
        raw_string_ostream ir_os(ir_str);
        inst->print(ir_os);
        step->llvm_ir = ir_os.str();
        
        // Extract variable name using DebugInfoAnalysis
        step->var_name = debugInfo.getVariableName(inst);
        
        // Extract type information
        step->type_name = debugInfo.getTypeName(inst);
    }
    
    // Extract debug information if available
    if (auto* I = dyn_cast_or_null<Instruction>(inst)) {
        // Get source location using DebugInfoAnalysis
        std::string srcLoc = debugInfo.getSourceLocation(I);
        
        // Parse source location
        size_t firstColon = srcLoc.find(':');
        size_t secondColon = srcLoc.find(':', firstColon + 1);
        
        if (firstColon != std::string::npos) {
            step->src_file = srcLoc.substr(0, firstColon);
            
            if (secondColon != std::string::npos) {
                try {
                    step->src_line = std::stoi(srcLoc.substr(firstColon + 1, secondColon - firstColon - 1));
                    step->src_column = std::stoi(srcLoc.substr(secondColon + 1));
                } catch (...) {
                    step->src_line = 0;
                    step->src_column = 0;
                }
            } else {
                try {
                    step->src_line = std::stoi(srcLoc.substr(firstColon + 1));
                } catch (...) {
                    step->src_line = 0;
                }
            }
        }
        
        // Get function name using DebugInfoAnalysis (includes demangling)
        step->func_name = debugInfo.getFunctionName(I);
        
        // Extract the actual source code statement using DebugInfoAnalysis
        step->source_code = debugInfo.getSourceCodeStatement(I);
    }
    
    trigger_steps.push_back(step);
}

// Helper to escape JSON strings
static std::string escapeJSON(const std::string& str) {
    std::string escaped;
    for (char c : str) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    escaped += buf;
                } else {
                    escaped += c;
                }
        }
    }
    return escaped;
}

void BugReport::export_json(raw_ostream& OS) const {
    OS << "    {\n";
    OS << "      \"Dominated\": " << (dominated ? "true" : "false") << ",\n";
    OS << "      \"Valid\": " << (valid ? "true" : "false") << ",\n";
    OS << "      \"Score\": " << conf_score << ",\n";
    OS << "      \"DiagSteps\": [\n";
    
    for (size_t i = 0; i < trigger_steps.size(); ++i) {
        const BugDiagStep* step = trigger_steps[i];
        OS << "        {\n";
        
        if (!step->src_file.empty()) {
            OS << "          \"File\": \"" << step->src_file << "\",\n";
            OS << "          \"Line\": " << step->src_line << ",\n";
            if (step->src_column > 0) {
                OS << "          \"Column\": " << step->src_column << ",\n";
            }
        }
        
        if (!step->func_name.empty()) {
            OS << "          \"Function\": \"" << step->func_name << "\",\n";
        }
        
        if (!step->var_name.empty()) {
            OS << "          \"Variable\": \"" << escapeJSON(step->var_name) << "\",\n";
        }
        
        if (!step->type_name.empty()) {
            OS << "          \"Type\": \"" << escapeJSON(step->type_name) << "\",\n";
        }
        
        if (!step->source_code.empty()) {
            OS << "          \"SourceCode\": \"" << escapeJSON(step->source_code) << "\",\n";
        }
        
        if (!step->llvm_ir.empty()) {
            OS << "          \"LLVM_IR\": \"" << escapeJSON(step->llvm_ir) << "\",\n";
        }
        
        OS << "          \"Tip\": \"" << escapeJSON(step->tip) << "\"\n";
        OS << "        }";
        
        if (i < trigger_steps.size() - 1) {
            OS << ",";
        }
        OS << "\n";
    }
    
    OS << "      ]\n";
    OS << "    }";
}

