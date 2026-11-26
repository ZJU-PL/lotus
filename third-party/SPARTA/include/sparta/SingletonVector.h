/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the SPARTA LICENSE file.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sparta {

// The sequence interface used by FlatMap's collision buckets. A singleton
// is stored directly in the object, preserving upstream small_vector<T, 1>'s
// no-allocation fast path. Larger buckets use a standard vector.
template <typename T> class SingletonVector {
  template <bool Const> class Iterator {
    using Owner =
        std::conditional_t<Const, const SingletonVector, SingletonVector>;
    friend class SingletonVector;
    template <bool> friend class Iterator;
    Owner *m_owner{nullptr};
    std::ptrdiff_t m_index{0};
    Iterator(Owner *owner, std::ptrdiff_t index)
        : m_owner(owner), m_index(index) {}

  public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<Const, const T *, T *>;
    using reference = std::conditional_t<Const, const T &, T &>;
    Iterator() = default;
    template <bool C = Const, std::enable_if_t<C, int> = 0>
    Iterator(const Iterator<false> &other)
        : m_owner(other.m_owner), m_index(other.m_index) {}
    reference operator*() const { return (*m_owner)[m_index]; }
    pointer operator->() const { return std::addressof(**this); }
    reference operator[](difference_type n) const { return *(*this + n); }
    Iterator &operator++() {
      ++m_index;
      return *this;
    }
    Iterator operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }
    Iterator &operator--() {
      --m_index;
      return *this;
    }
    Iterator operator--(int) {
      auto old = *this;
      --*this;
      return old;
    }
    Iterator &operator+=(difference_type n) {
      m_index += n;
      return *this;
    }
    Iterator &operator-=(difference_type n) {
      m_index -= n;
      return *this;
    }
    Iterator operator+(difference_type n) const {
      auto copy = *this;
      return copy += n;
    }
    Iterator operator-(difference_type n) const {
      auto copy = *this;
      return copy -= n;
    }
    friend Iterator operator+(difference_type n, Iterator it) {
      return it += n;
    }
    template <bool C>
    difference_type operator-(const Iterator<C> &other) const {
      assert(m_owner == other.m_owner);
      return m_index - other.m_index;
    }
    template <bool C> bool operator==(const Iterator<C> &other) const {
      return m_owner == other.m_owner && m_index == other.m_index;
    }
    template <bool C> bool operator!=(const Iterator<C> &other) const {
      return !(*this == other);
    }
    template <bool C> bool operator<(const Iterator<C> &other) const {
      return (*this - other) < 0;
    }
    template <bool C> bool operator>(const Iterator<C> &other) const {
      return other < *this;
    }
    template <bool C> bool operator<=(const Iterator<C> &other) const {
      return !(other < *this);
    }
    template <bool C> bool operator>=(const Iterator<C> &other) const {
      return !(*this < other);
    }
  };

public:
  using value_type = T;
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;
  using difference_type = std::ptrdiff_t;
  using size_type = std::size_t;
  using const_reference = const T &;
  using const_pointer = const T *;

  SingletonVector() = default;
  SingletonVector(const SingletonVector &other) {
    if (other.size() == 1)
      m_data.template emplace<1>(other[0]);
    else if (other.size() > 1)
      m_data.template emplace<2>(std::get<2>(other.m_data));
  }
  SingletonVector(SingletonVector &&) = default;
  SingletonVector &operator=(const SingletonVector &other) {
    if (this != &other) {
      // Build before replacing the current sequence. std::vector provides
      // the usual guarantees when copying a multi-element bucket.
      SingletonVector copy(other);
      *this = std::move(copy);
    }
    return *this;
  }
  SingletonVector &operator=(SingletonVector &&) = default;

  size_type size() const {
    return m_data.index() == 1   ? 1
           : m_data.index() == 2 ? std::get<2>(m_data).size()
                                 : 0;
  }
  bool empty() const { return size() == 0; }
  size_type max_size() const { return std::vector<T>().max_size(); }
  bool uses_inline_storage() const { return m_data.index() != 2; }
  T &operator[](size_type index) {
    return m_data.index() == 1 ? std::get<1>(m_data)
                               : std::get<2>(m_data)[index];
  }
  const T &operator[](size_type index) const {
    return m_data.index() == 1 ? std::get<1>(m_data)
                               : std::get<2>(m_data)[index];
  }
  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, size()); }
  const_iterator begin() const { return cbegin(); }
  const_iterator end() const { return cend(); }
  const_iterator cbegin() const { return const_iterator(this, 0); }
  const_iterator cend() const { return const_iterator(this, size()); }

  template <typename... Args>
  iterator emplace(const_iterator position, Args &&...args) {
    const auto index = position - cbegin();
    assert(index >= 0 && static_cast<size_type>(index) <= size());
    if (m_data.index() == 2) {
      auto &values = std::get<2>(m_data);
      values.emplace(values.begin() + index, std::forward<Args>(args)...);
    } else if (empty()) {
      try {
        m_data.template emplace<1>(std::forward<Args>(args)...);
      } catch (...) {
        m_data.template emplace<0>();
        throw;
      }
    } else {
      std::vector<T> values;
      values.reserve(2);
      if (index == 0)
        values.emplace_back(std::forward<Args>(args)...);
      values.push_back(std::get<1>(m_data));
      if (index == 1)
        values.emplace_back(std::forward<Args>(args)...);
      m_data.template emplace<2>(std::move(values));
    }
    return iterator(this, index);
  }
  iterator insert(const_iterator position, const T &value) {
    return emplace(position, value);
  }
  iterator insert(const_iterator position, T &&value) {
    return emplace(position, std::move(value));
  }
  template <typename It>
  iterator insert(const_iterator position, It first, It last) {
    const auto index = position - cbegin();
    if (first == last)
      return iterator(this, index);
    auto next = first;
    if (++next == last)
      return emplace(position, *first);
    if (m_data.index() == 2) {
      auto &values = std::get<2>(m_data);
      values.insert(values.begin() + index, first, last);
    } else {
      std::vector<T> values;
      values.reserve(size() + std::distance(first, last));
      if (index != 0)
        values.push_back(std::get<1>(m_data));
      values.insert(values.end(), first, last);
      if (index == 0 && !empty())
        values.push_back(std::get<1>(m_data));
      m_data.template emplace<2>(std::move(values));
    }
    return iterator(this, index);
  }
  void push_back(const T &value) { emplace(cend(), value); }
  void push_back(T &&value) { emplace(cend(), std::move(value)); }

  iterator erase(const_iterator first, const_iterator last) {
    const auto index = first - cbegin();
    const auto count = last - first;
    if (count) {
      if (m_data.index() == 2) {
        auto &values = std::get<2>(m_data);
        values.erase(values.begin() + index, values.begin() + index + count);
      } else {
        m_data.template emplace<0>();
      }
    }
    return iterator(this, index);
  }
  iterator erase(const_iterator position) {
    return erase(position, position + 1);
  }
  void clear() {
    if (m_data.index() == 2)
      std::get<2>(m_data).clear();
    else
      m_data.template emplace<0>();
  }

private:
  std::variant<std::monostate, T, std::vector<T>> m_data;
};

} // namespace sparta
