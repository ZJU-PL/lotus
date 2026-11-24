#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pdg {

/// Diagnostic counters and truncation markers emitted by query execution.
struct PDGQueryDiagnostics {
  bool depth_limit_hit = false;
  bool state_limit_hit = false;
  bool path_limit_hit = false;
  bool path_length_limit_hit = false;
  bool stack_depth_limit_hit = false;
  size_t explored_states = 0;
  size_t max_depth_reached = 0;
  size_t max_stack_depth_reached = 0;
  size_t summary_cache_hits = 0;
  size_t summary_cache_misses = 0;
  size_t closure_cache_hits = 0;
  size_t closure_cache_misses = 0;
  size_t criteria_cache_hits = 0;
  size_t criteria_cache_misses = 0;
  std::vector<std::string> unresolved_criteria;
  std::vector<std::string> notes;
};

} // namespace pdg
