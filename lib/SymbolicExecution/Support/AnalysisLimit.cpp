#include "SymbolicExecution/Support/AnalysisLimit.h"

#include "llvm/Support/CommandLine.h"

namespace SymbolicExecution {

namespace {

llvm::cl::opt<bool> AnalyzeNonArrayAlloca(
    "symex-analyze-nonarray-alloca",
    llvm::cl::desc("Treat nonarray alloca also as memory allocation sites"),
    llvm::cl::init(false));

llvm::cl::opt<std::string> SymexDebugFunName("symex-debug-fun",
                                             llvm::cl::init(""));

} // namespace

#define REGISTER_SYMEX_LIMIT(FieldName, OptName, Val)                          \
  unsigned AnalysisLimit::FieldName = Val;                                     \
  static llvm::cl::opt<unsigned, true> Opt_##FieldName(                        \
      OptName, llvm::cl::location(AnalysisLimit::FieldName),                   \
      llvm::cl::init(Val));

REGISTER_SYMEX_LIMIT(INST_QUERY_LIMIT_V, "symex-inst-query-limit", 5)
REGISTER_SYMEX_LIMIT(FUNC_QUERY_LIMIT_V, "symex-func-query-limit", 200)
REGISTER_SYMEX_LIMIT(VALUE_SET_LIMIT_V, "symex-value-set-limit", 20)
REGISTER_SYMEX_LIMIT(SYMBOLIC_VAL_SET_LIMIT_V, "symex-sym-value-set-limit", 20)
REGISTER_SYMEX_LIMIT(POINTS_SET_LIMIT_V, "symex-pts-set-limit", 20)
REGISTER_SYMEX_LIMIT(TAINT_VAL_SET_LIMIT_V, "symex-taint-set-limit", 30000)
REGISTER_SYMEX_LIMIT(FUNC_INLINE_LIMIT_V, "symex-func-inline-limit", 10)
REGISTER_SYMEX_LIMIT(CONSTRAINT_SIZE_LIMIT_V, "symex-constraint-sz-limit", 1000)
REGISTER_SYMEX_LIMIT(MAX_FUNC_SOLVER_LIMIT_V, "symex-max-func-solver-limit",
                     1024)

#undef REGISTER_SYMEX_LIMIT

bool AnalysisLimit::shouldAnalyzeNonArrayAlloca() {
  return AnalyzeNonArrayAlloca;
}

const std::string &AnalysisLimit::getDebugFunctionName() {
  return SymexDebugFunName;
}

} // namespace SymbolicExecution
