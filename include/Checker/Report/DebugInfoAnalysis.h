// Use debug info to better report bugs, e.g., line number, function name, etc.

#pragma once

#include <string>

namespace llvm {
    class Instruction;
    class Value;
}

class DebugInfoAnalysis {
public:
    DebugInfoAnalysis();
    ~DebugInfoAnalysis() = default;

    // Get source location information (file:line:col)
    std::string getSourceLocation(const llvm::Instruction *I);

    // Get function name from debug info or LLVM IR
    std::string getFunctionName(const llvm::Instruction *I);

    // Get variable name from debug info or LLVM IR
    std::string getVariableName(const llvm::Value *V);

    // Get type name as string
    std::string getTypeName(const llvm::Value *V);

    // Print a formatted bug report with debug information
    void printBugReport(const llvm::Instruction *BugInst,
                       const std::string &BugType,
                       const llvm::Value *RelatedValue = nullptr);

private:
    // Helper methods can be added here if needed
};
