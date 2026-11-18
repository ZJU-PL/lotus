//===- MemorySSADump.h - textual sparse MemorySSA dump -------------------===//
#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

namespace llvm {
class Module;
}

namespace lotus {
namespace analysis {

class SVFG;

/// Print sparse MemorySSA nodes grouped by LLVM function and basic block.
///
/// `functionFilter` limits output to one function. An empty filter prints all
/// module-global and function-owned memory facts.
bool dumpMemorySSA(const SVFG &graph, const llvm::Module &module,
                   llvm::raw_ostream &output,
                   llvm::StringRef functionFilter = {},
                   std::string *error = nullptr);

} // namespace analysis
} // namespace lotus
