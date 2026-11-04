#pragma once

#include <cstddef>
#include <vector>

namespace lotus::verification::automata {

// Borrowed C++17 range. Providers retain ownership and keep returned ranges
// valid across subsequent queries. The range itself never allocates.
template <typename T> class ArrayView {
  const T *data_ = nullptr;
  std::size_t size_ = 0;

public:
  ArrayView() = default;
  ArrayView(const T *data, std::size_t size) : data_(data), size_(size) {}
  template <typename Allocator>
  ArrayView(const std::vector<T, Allocator> &values)
      : data_(values.data()), size_(values.size()) {}
  const T *begin() const { return data_; }
  const T *end() const { return size_ ? data_ + size_ : data_; }
  const T *data() const { return data_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  const T &front() const { return data_[0]; }
  const T &operator[](std::size_t i) const { return data_[i]; }
};
} // namespace lotus::verification::automata
