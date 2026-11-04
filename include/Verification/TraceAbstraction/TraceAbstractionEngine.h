#pragma once

#include "Verification/TraceAbstraction/IcfgToNwa.h"

#include <map>

namespace lotus::verification::traceabstraction {

struct TraceAbstractionOptions {
  unsigned maxIterations = 100;
  std::chrono::milliseconds timeout{30000};
  bool enableInductiveGeneralization = true;
};

struct VerificationResult {
  enum Status { Safe, Unsafe, Unknown };
  Status status = Unknown;
  unsigned iterations = 0; // number of candidate traces checked
  std::vector<const llvm::Instruction *> counterexample;
  std::vector<std::int64_t> matching;
  std::map<std::string, std::string> assignments;
  std::string reason;
};

class TraceAbstractionEngine {
public:
  explicit TraceAbstractionEngine(TraceAbstractionOptions options = {});
  VerificationResult verify(const llvm::Function &function,
                            const llvm::Instruction *targetError = nullptr);
  VerificationResult verify(const ICFG &icfg, const ICFGNode *errorNode);

private:
  TraceAbstractionOptions options_;
  VerificationResult run(const ProgramAutomaton &program, nwa::Context context);
};
} // namespace lotus::verification::traceabstraction
