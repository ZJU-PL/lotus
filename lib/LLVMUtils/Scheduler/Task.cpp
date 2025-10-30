#include "LLVMUtils/Scheduler/Task.h"

#include <sstream>

using namespace llvm;

std::string FunctionTask::toString() {
  std::ostringstream Oss;
  if (Func && Func->hasName()) {
    Oss << "FunctionTask[" << Func->getName().str() << "]";
  } else {
    Oss << "FunctionTask[anonymous]";
  }
  return Oss.str();
}

std::string GCTask::toString() {
  std::ostringstream Oss;
  Oss << "GCTask[" << FuncSet.size() << " functions]";
  return Oss.str();
}

