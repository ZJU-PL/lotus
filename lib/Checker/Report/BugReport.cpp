#include "Checker/Report/BugReport.h"
#include "Checker/Report/DebugInfoAnalysis.h"
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

void BugReport::append_step(Value* inst, const std::string& tip) {
    BugDiagStep* step = new BugDiagStep();
    step->inst = inst;
    step->tip = tip;
    
    // Extract debug information if available
    if (auto* I = dyn_cast_or_null<Instruction>(inst)) {
        if (const DebugLoc& Loc = I->getDebugLoc()) {
            step->src_file = Loc->getFilename();
            step->src_line = Loc->getLine();
            step->src_column = Loc->getColumn();
        }
        
        if (Function* F = I->getParent()->getParent()) {
            step->func_name = F->getName();
        }
    }
    
    trigger_steps.push_back(step);
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
        }
        
        if (!step->func_name.empty()) {
            OS << "          \"Function\": \"" << step->func_name << "\",\n";
        }
        
        OS << "          \"Tip\": \"" << step->tip << "\"\n";
        OS << "        }";
        
        if (i < trigger_steps.size() - 1) {
            OS << ",";
        }
        OS << "\n";
    }
    
    OS << "      ]\n";
    OS << "    }";
}

