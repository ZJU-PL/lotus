#pragma once

#include <string>
#include <string_view>

namespace llvm {
class Module;
}

namespace llvm_util {
std::string optimize_module(llvm::Module &M, std::string_view optArgs);
}