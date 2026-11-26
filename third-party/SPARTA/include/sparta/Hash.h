/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <functional>

namespace sparta {
namespace hash_impl {

template <typename T> std::size_t hash_value(const T &value) {
  return std::hash<T>{}(value);
}

} // namespace hash_impl

inline void hash_combine(std::size_t &seed, std::size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

template <typename T> void hash_combine(std::size_t &seed, const T &value) {
  hash_combine(seed, hash_impl::hash_value(value));
}

template <typename Iterator>
std::size_t hash_range(Iterator begin, Iterator end) {
  std::size_t seed = 0;
  for (; begin != end; ++begin) {
    hash_combine(seed, *begin);
  }
  return seed;
}

} // namespace sparta
