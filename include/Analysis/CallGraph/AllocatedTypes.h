#pragma once

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"

#include <vector>

namespace lotus {

[[nodiscard]] std::vector<const llvm::DICompositeType *>
collectAllocatedTypes(const llvm::Module &Mod);

} // namespace lotus
