//
// Analysis options shared by the AserPTA driver and solver.
//
#pragma once

#include <string>

namespace aser {

/// Optional persistent state for inclusion-based pointer-analysis results.
/// Empty paths retain the original solve-only behavior.
struct AnalysisOptions {
  std::string readPointsToPath;
  std::string writePointsToPath;

  bool operator==(const AnalysisOptions &other) const {
    return readPointsToPath == other.readPointsToPath &&
           writePointsToPath == other.writePointsToPath;
  }

  bool operator!=(const AnalysisOptions &other) const {
    return !(*this == other);
  }
};

} // namespace aser
