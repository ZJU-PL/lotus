#pragma once
#include <string>
namespace lotus::gsaf {
class GSAFOptions {
public:
  static bool EnableArithmeticFlow;
  static bool EnableHeapAllocFailure;
  static bool EnableFileAllocFailure;
  static std::string MemorySpecPath;
  static std::string IOSpecPath;
  static std::string BufferSpecPath;
  static std::string TaintSpecPath;
  static std::string DebugFunction;

  static bool DebugConstraints;

  static bool DebugTrace;

  static unsigned InlineDepth;

  static int SolverVersion;

  static bool EnableCSSymSummary;

  static unsigned InlineCSSymDepth;

  static bool DotGVFGValFlow;

  static std::string CollectGVFGData;

  static unsigned Timeout;

  static int MaxSummary;

  static bool EnableSideEffectSource;
};
} // namespace lotus::gsaf
