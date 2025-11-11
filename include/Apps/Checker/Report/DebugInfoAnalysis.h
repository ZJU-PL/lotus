// Use debug info to better report bugs, e.g., line number, function name, etc.
// Adapted from Clearblue's DebugInfoAnalysis for LLVM 14+

#pragma once

#include <string>
#include <unordered_map>
#include <map>
#include <vector>

namespace llvm {
    class Instruction;
    class Value;
    class Function;
    class MDNode;
}

class DebugInfoAnalysis {
public:
    DebugInfoAnalysis();
    ~DebugInfoAnalysis() = default;

    // Get source location information (file:line:col)
    std::string getSourceLocation(const llvm::Instruction *I);

    // Get function name from debug info or LLVM IR (with C++ demangling)
    std::string getFunctionName(const llvm::Instruction *I);

    // Get variable name from debug info or LLVM IR (with C++ demangling)
    std::string getVariableName(const llvm::Value *V);

    // Get type name as string
    std::string getTypeName(const llvm::Value *V);
    
    // Get source file path for a value
    std::string getSourceFile(const llvm::Value *V);
    
    // Get source line number
    int getSourceLine(const llvm::Value *V);
    
    // Get source column number
    int getSourceColumn(const llvm::Value *V);
    
    // Get the actual source code statement for an instruction
    // Returns empty string if source file is not accessible
    std::string getSourceCodeStatement(const llvm::Instruction *I);

    // Print a formatted bug report with debug information
    void printBugReport(const llvm::Instruction *BugInst,
                       const std::string &BugType,
                       const llvm::Value *RelatedValue = nullptr);

private:
    // Cache for source file contents (filename -> lines)
    static std::map<std::string, std::vector<std::string>> sourceFileCache;
    
    // Helper: Read a source file into cache
    static bool loadSourceFile(const std::string& filepath);
    
    // Helper: Find the actual path to a source file
    static std::string findSourceFile(const std::string& filename);
    
    // Helper: Collect metadata from debug intrinsics
    void collectMetadata(const llvm::Function *F);
    
    // Helper: Get variable descriptor metadata node
    llvm::MDNode* findVarInfoMDNode(const llvm::Value *V, const llvm::Function *F = nullptr);
    
    // Cache for variable names
    std::unordered_map<const llvm::Value*, std::string> varNameCache;
};
