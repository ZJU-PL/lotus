#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace lotus {

enum class CallGraphAnalysisType {
  NORESOLVE,
  CHA,
  RTA,
  VTA,
  OTF,
  Invalid
};

std::string toString(CallGraphAnalysisType CGA);

CallGraphAnalysisType toCallGraphAnalysisType(llvm::StringRef S);

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, CallGraphAnalysisType CGA);

} // namespace lotus
