#pragma once

#include "Alias/InclusionBased/TPA/Context/Context.h"

namespace llvm {
class Instruction;
} // namespace llvm

namespace context {

class ProgramPoint;

class KLimitContext {
private:
  // K-limiting context sensitivity parameter
  // Controls the maximum depth of the call stack that is tracked
  // A value of 0 means context-insensitive (no call stack tracking)
  // Higher values provide more precision at the cost of scalability
  static unsigned defaultLimit;

public:
  // Set the K-limit parameter
  // Parameters: k - the maximum call stack depth to track
  // Recommended values: 1-3 for most programs, higher for small programs
  static void setLimit(unsigned k) { defaultLimit = k; }
  // Get the current K-limit value
  static unsigned getLimit() { return defaultLimit; }

  // Push a new call site onto the context stack using standard suffix k-CFA.
  // Retains the k most recent call sites (dropping the oldest frame when
  // depth >= k). For k == 0, always returns the global (empty) context.
  // Parameters: ctx - the current context before the call, inst - the call
  // instruction
  static const Context *pushContext(const Context *, const llvm::Instruction *);
  // Push a new call site using a ProgramPoint (contains both context and node)
  static const Context *pushContext(const ProgramPoint &);
};

} // namespace context
