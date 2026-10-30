#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace lotus::alias::flow_sensitive {

// Shared by the Lotus exporter and the independently linked SVF observer.
// Seed zero retains the lexical prefix used by earlier experiments.
template <typename Map>
void sampleComparisonValues(Map &values, std::size_t limit, uint32_t seed) {
  if (values.size() <= limit)
    return;
  if (seed == 0) {
    while (values.size() > limit)
      values.erase(std::prev(values.end()));
    return;
  }
  using Entry = std::pair<uint64_t, typename Map::iterator>;
  std::vector<Entry> ranked;
  ranked.reserve(values.size());
  for (auto it = values.begin(); it != values.end(); ++it) {
    uint64_t hash = UINT64_C(14695981039346656037) ^ seed;
    for (unsigned char byte : it->first)
      hash = (hash ^ byte) * UINT64_C(1099511628211);
    ranked.emplace_back(hash, it);
  }
  std::sort(ranked.begin(), ranked.end(), [](const Entry &a, const Entry &b) {
    return a.first == b.first ? a.second->first < b.second->first
                              : a.first < b.first;
  });
  for (std::size_t index = limit; index < ranked.size(); ++index)
    values.erase(ranked[index].second);
}

} // namespace lotus::alias::flow_sensitive
