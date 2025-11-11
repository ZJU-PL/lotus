// Use debug info of LLVM to better report bugs, e.g., line number, position, function name, variable name, etc.
// Enhanced version adapted from Clearblue's DebugInfoAnalysis for LLVM 14+

#include "Apps/Checker/Report/DebugInfoAnalysis.h"
#include "Utils/LLVM/Demangle.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Path.h>
#include <fstream>
#include <unistd.h>

using namespace llvm;

// Static cache for source file contents
std::map<std::string, std::vector<std::string>> DebugInfoAnalysis::sourceFileCache;

DebugInfoAnalysis::DebugInfoAnalysis() = default;

//===----------------------------------------------------------------------===//
// Source File Handling (adapted from Clearblue)
//===----------------------------------------------------------------------===//

bool DebugInfoAnalysis::loadSourceFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        // Trim leading whitespace for better display
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) {
            lines.push_back(line.substr(start));
        } else {
            lines.push_back("");
        }
    }
    file.close();
    
    sourceFileCache[filepath] = lines;
    return true;
}

std::string DebugInfoAnalysis::findSourceFile(const std::string& filename) {
    if (filename.empty()) {
        return "";
    }
    
    // Try various search strategies
    std::vector<std::string> searchPaths;
    
    // 1. Try as absolute path
    if (filename[0] == '/') {
        searchPaths.push_back(filename);
    } else {
        // 2. Relative paths - try common locations
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            std::string cwdStr(cwd);
            
            // Current directory
            searchPaths.push_back(cwdStr + "/" + filename);
            
            // Parent directories
            searchPaths.push_back(cwdStr + "/../" + filename);
            searchPaths.push_back(cwdStr + "/../../" + filename);
            searchPaths.push_back(cwdStr + "/../../../" + filename);
            
            // Common benchmark locations
            searchPaths.push_back(cwdStr + "/benchmarks/" + filename);
            searchPaths.push_back(cwdStr + "/../benchmarks/" + filename);
            searchPaths.push_back(cwdStr + "/benchmarks/micro/npd/" + filename);
            searchPaths.push_back(cwdStr + "/benchmarks/micro/overflow/" + filename);
        }
        
        // Also try relative to current working directory
        searchPaths.push_back(filename);
        searchPaths.push_back("../" + filename);
        searchPaths.push_back("../../" + filename);
    }
    
    // Try each path
    for (const auto& path : searchPaths) {
        std::ifstream test(path);
        if (test.is_open()) {
            test.close();
            return path;
        }
    }
    
    return "";  // Not found
}

//===----------------------------------------------------------------------===//
// Source Code Extraction
//===----------------------------------------------------------------------===//

std::string DebugInfoAnalysis::getSourceCodeStatement(const Instruction *I) {
    if (!I) return "";
    
    std::string filepath = getSourceFile(I);
    int line = getSourceLine(I);
    
    if (filepath.empty() || line <= 0) {
        return "";
    }
    
    // Check cache first
    auto it = sourceFileCache.find(filepath);
    if (it == sourceFileCache.end()) {
        // Try to find and load the file
        std::string actualPath = findSourceFile(filepath);
        if (!actualPath.empty()) {
            if (loadSourceFile(actualPath)) {
                // Now use the actual path for lookup
                filepath = actualPath;
                it = sourceFileCache.find(filepath);
            } else {
                // Load failed, cache empty result
                sourceFileCache[filepath] = std::vector<std::string>();
                return "";
            }
        } else {
            // File not found, cache empty result
            sourceFileCache[filepath] = std::vector<std::string>();
            return "";
        }
    }
    
    // Get the line from cache
    if (it != sourceFileCache.end() && line > 0 && line <= (int)it->second.size()) {
        return it->second[line - 1];
    }
    
    return "";
}

//===----------------------------------------------------------------------===//
// Debug Info Extraction (adapted for LLVM 14+)
//===----------------------------------------------------------------------===//

std::string DebugInfoAnalysis::getSourceFile(const Value *V) {
    if (!V) return "";
    
    if (const Instruction *I = dyn_cast<Instruction>(V)) {
        if (const DebugLoc& DL = I->getDebugLoc()) {
            if (auto *Scope = DL.getScope()) {
                if (auto *File = cast<DIScope>(Scope)->getFile()) {
                    // Return full path by combining directory and filename
                    std::string dir = File->getDirectory().str();
                    std::string file = File->getFilename().str();
                    
                    if (dir.empty()) {
                        return file;
                    } else if (file[0] == '/') {
                        return file;  // Already absolute
                    } else {
                        return dir + "/" + file;
                    }
                }
            }
        }
    }
    
    return "";
}

int DebugInfoAnalysis::getSourceLine(const Value *V) {
    if (!V) return 0;
    
    if (const Instruction *I = dyn_cast<Instruction>(V)) {
        if (const DebugLoc& DL = I->getDebugLoc()) {
            return DL.getLine();
        }
    }
    
    return 0;
}

int DebugInfoAnalysis::getSourceColumn(const Value *V) {
    if (!V) return 0;
    
    if (const Instruction *I = dyn_cast<Instruction>(V)) {
        if (const DebugLoc& DL = I->getDebugLoc()) {
            return DL.getCol();
        }
    }
    
    return 0;
}

std::string DebugInfoAnalysis::getSourceLocation(const Instruction *I) {
    if (!I) return "unknown:0";

    const DebugLoc &DL = I->getDebugLoc();
    if (!DL) return "unknown:0";

    unsigned Line = DL.getLine();
    unsigned Col = DL.getCol();

    if (auto *Scope = DL.getScope()) {
        if (isa<DIScope>(Scope)) {
            if (auto *File = cast<DIScope>(Scope)->getFile()) {
                return File->getFilename().str() + ":" + std::to_string(Line) +
                       ":" + std::to_string(Col);
            }
        }
    }

    return "unknown:" + std::to_string(Line);
}

std::string DebugInfoAnalysis::getFunctionName(const Instruction *I) {
    if (!I) return "unknown_function";

    const Function *F = I->getFunction();
    if (!F) return "unknown_function";

    std::string funcName;
    
    // Try to get name from debug info first (real source name)
    if (DISubprogram *Subprogram = F->getSubprogram()) {
        funcName = Subprogram->getName().str();
    } else {
        funcName = F->getName().str();
    }
    
    // Demangle C++ and Rust function names for better readability
    return DemangleUtils::demangleWithCleanup(funcName);
}

std::string DebugInfoAnalysis::getVariableName(const Value *V) {
    if (!V) return "temp_var";

    // Check cache first
    auto it = varNameCache.find(V);
    if (it != varNameCache.end()) {
        return it->second;
    }

    std::string varName;
    
    // Try to get debug info for local variables using dbg.declare/dbg.value
    if (auto *I = dyn_cast<Instruction>(V)) {
        // Scan the function for dbg intrinsics that reference this value
        const Function *F = I->getFunction();
        if (F) {
            for (const auto &BB : *F) {
                for (const auto &Inst : BB) {
                    // Check dbg.declare
                    if (auto *DbgDeclare = dyn_cast<DbgDeclareInst>(&Inst)) {
                        if (DbgDeclare->getAddress() == V) {
                            if (auto *Var = DbgDeclare->getVariable()) {
                                varName = Var->getName().str();
                                break;
                            }
                        }
                    }
                    // Check dbg.value
                    else if (auto *DbgValue = dyn_cast<DbgValueInst>(&Inst)) {
                        if (DbgValue->getValue() == V) {
                            if (auto *Var = DbgValue->getVariable()) {
                                varName = Var->getName().str();
                                break;
                            }
                        }
                    }
                }
                if (!varName.empty()) break;
            }
        }
    }

    // Fallback to LLVM IR name
    if (varName.empty() && V->hasName()) {
        varName = V->getName().str();
        // Demangle if needed
        varName = DemangleUtils::demangleWithCleanup(varName);
    }

    // For load/store, try to get the pointer operand's name
    if (varName.empty()) {
        if (auto* LI = dyn_cast<LoadInst>(V)) {
            const Value* Ptr = LI->getPointerOperand();
            if (Ptr && Ptr->hasName()) {
                varName = Ptr->getName().str();
            }
        } else if (auto* SI = dyn_cast<StoreInst>(V)) {
            const Value* Ptr = SI->getPointerOperand();
            if (Ptr && Ptr->hasName()) {
                varName = Ptr->getName().str();
            }
        } else if (auto* CI = dyn_cast<CallInst>(V)) {
            // For call instructions, use the function name
            if (const Function* F = CI->getCalledFunction()) {
                varName = F->getName().str();
            }
        }
    }

    if (varName.empty()) {
        varName = "temp_var";
    }

    // Cache the result
    varNameCache[V] = varName;
    return varName;
}

std::string DebugInfoAnalysis::getTypeName(const Value *V) {
    if (!V) return "unknown_type";

    Type *Ty = V->getType();
    if (!Ty) return "unknown_type";

    std::string TypeStr;
    raw_string_ostream RSO(TypeStr);
    Ty->print(RSO);
    return RSO.str();
}

void DebugInfoAnalysis::printBugReport(const Instruction *BugInst,
                                     const std::string &BugType,
                                     const Value *RelatedValue) {
    printf("[BUG REPORT] %s\n", BugType.c_str());
    printf("  Location: %s\n", getSourceLocation(BugInst).c_str());
    printf("  Function: %s\n", getFunctionName(BugInst).c_str());
    if (RelatedValue) {
        printf("  Variable: %s\n", getVariableName(RelatedValue).c_str());
        printf("  Type: %s\n", getTypeName(RelatedValue).c_str());
    }
    
    // Try to show source code
    std::string srcCode = getSourceCodeStatement(BugInst);
    if (!srcCode.empty()) {
        printf("  Source Code: %s\n", srcCode.c_str());
    }
    
    printf("\n");
}
