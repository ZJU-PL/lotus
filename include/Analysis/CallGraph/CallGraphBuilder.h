#pragma once

#include "Analysis/CallGraph/CallGraph.h"
#include "Analysis/CallGraph/CallGraphAnalysisType.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"
#include "Analysis/CallGraph/Resolver.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace llvm {
class Module;
class Function;
} // namespace llvm

namespace lotus {

class DIBasedTypeHierarchy;

CallGraph buildCallGraph(const llvm::Module &M, Resolver &Res,
                         llvm::ArrayRef<const llvm::Function *> EntryPoints);

CallGraph buildCallGraph(const llvm::Module &M, Resolver &Res,
                         llvm::ArrayRef<std::string> EntryPointNames = {"main"});

CallGraph buildCallGraph(const llvm::Module &M, CallGraphAnalysisType CGType,
                         llvm::ArrayRef<std::string> EntryPointNames = {"main"},
                         const DIBasedTypeHierarchy *TH = nullptr,
                         const LLVMVFTableProvider *VTP = nullptr);

CallGraph buildCallGraphWithModels(
    llvm::Module &M, CallGraphAnalysisType CGType = CallGraphAnalysisType::OTF,
    llvm::ArrayRef<std::string> EntryPointNames = {"main"},
    bool IncludeGlobalCtorsDtors = true,
    bool RewriteExternCallbacks = true);

std::vector<const llvm::Function *>
getEntryPoints(const llvm::Module &M,
               llvm::ArrayRef<std::string> EntryPointNames = {"main"});

} // namespace lotus
