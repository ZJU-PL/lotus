/** @file AnalysisLimit.h @brief Configurable limits and flags for symbolic execution. */
#pragma once

#include <string>

namespace SymbolicExecution {

class AnalysisLimit {
public:
  static unsigned INST_QUERY_LIMIT_V;
  static unsigned FUNC_QUERY_LIMIT_V;
  static unsigned VALUE_SET_LIMIT_V;
  static unsigned SYMBOLIC_VAL_SET_LIMIT_V;
  static unsigned POINTS_SET_LIMIT_V;
  static unsigned TAINT_VAL_SET_LIMIT_V;
  static unsigned FUNC_INLINE_LIMIT_V;
  static unsigned CONSTRAINT_SIZE_LIMIT_V;
  static unsigned MAX_FUNC_SOLVER_LIMIT_V;

  static bool shouldAnalyzeNonArrayAlloca();
  static const std::string &getDebugFunctionName();
};

} // namespace SymbolicExecution
