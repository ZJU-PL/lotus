#include "Analysis/CallGraph/CallGraphAnalysisType.h"

namespace lotus {

std::string toString(CallGraphAnalysisType CGA) {
  switch (CGA) {
  case CallGraphAnalysisType::NORESOLVE:
    return "NORESOLVE";
  case CallGraphAnalysisType::CHA:
    return "CHA";
  case CallGraphAnalysisType::RTA:
    return "RTA";
  case CallGraphAnalysisType::VTA:
    return "VTA";
  case CallGraphAnalysisType::OTF:
    return "OTF";
  case CallGraphAnalysisType::Invalid:
    return "Invalid";
  }
  return "Invalid";
}

CallGraphAnalysisType toCallGraphAnalysisType(llvm::StringRef S) {
  if (S.equals_insensitive("cha")) {
    return CallGraphAnalysisType::CHA;
  }
  if (S.equals_insensitive("rta")) {
    return CallGraphAnalysisType::RTA;
  }
  if (S.equals_insensitive("vta")) {
    return CallGraphAnalysisType::VTA;
  }
  if (S.equals_insensitive("otf")) {
    return CallGraphAnalysisType::OTF;
  }
  if (S.equals_insensitive("noresolve") || S.equals_insensitive("nores")) {
    return CallGraphAnalysisType::NORESOLVE;
  }
  return CallGraphAnalysisType::Invalid;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              CallGraphAnalysisType CGA) {
  return OS << toString(CGA);
}

} // namespace lotus
