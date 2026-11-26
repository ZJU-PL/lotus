#pragma once

#include <cstddef>

namespace ifds {
enum class DFIIndexDirection { Forward, Backward };
struct DFIConfig {
  static constexpr bool BuildQueryIndex = true;
  static constexpr DFIIndexDirection IndexDirection =
      DFIIndexDirection::Forward;
};
struct DFIConfigNoIndex : DFIConfig {
  static constexpr bool BuildQueryIndex = false;
};
struct DFIConfigBackward : DFIConfig {
  static constexpr DFIIndexDirection IndexDirection =
      DFIIndexDirection::Backward;
};
struct DFIStatistics {
  size_t expanded_vertices = 0;
  size_t normal_flow_evaluations = 0;
  size_t call_flow_evaluations = 0;
  size_t return_flow_evaluations = 0;
  size_t local_index_builds = 0;
  size_t summary_edges = 0;
  size_t query_intervals = 0;
};
} // namespace ifds
