/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <ostream>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <sparta/AbstractSet.h>
#include <sparta/PatriciaTreeUtil.h>
#include <sparta/PerfectForwardCapture.h>

namespace sparta {
namespace fs_impl {

template <typename T, typename = void> struct is_container : std::false_type {};

template <typename T>
struct is_container<
    T, std::void_t<typename T::iterator, typename T::const_iterator,
                   decltype(std::declval<const T &>().begin()),
                   decltype(std::declval<const T &>().end())>>
    : std::true_type {};

} // namespace fs_impl

/* Represents a set implemented with a sorted vector. */
template <typename Element, typename Compare = std::less<Element>,
          typename Equal = std::equal_to<Element>,
          typename AllocatorOrContainer = std::allocator<Element>>
class FlatSet final
    : public AbstractSet<
          FlatSet<Element, Compare, Equal, AllocatorOrContainer>>,
      private std::tuple<Compare> {
private:
  using Storage =
      std::conditional_t<fs_impl::is_container<AllocatorOrContainer>::value,
                         AllocatorOrContainer,
                         std::vector<Element, AllocatorOrContainer>>;

  const Compare& comparator() const {
    return std::get<0>(static_cast<const std::tuple<Compare>&>(*this));
  }

  bool equivalent_keys(const Element& left, const Element& right) const {
    return !comparator()(left, right) && !comparator()(right, left);
  }

public:
  using iterator = typename Storage::const_iterator;
  using const_iterator = iterator;
  using value_type = Element;
  using difference_type = typename Storage::difference_type;
  using size_type = typename Storage::size_type;
  using const_reference = typename Storage::const_reference;
  using const_pointer = typename Storage::const_pointer;

  FlatSet() = default;

  explicit FlatSet(std::initializer_list<Element> values) {
    for (const auto &value : values) {
      insert(value);
    }
  }

  template <typename InputIterator>
  FlatSet(InputIterator first, InputIterator last) {
    for (; first != last; ++first) {
      insert(*first);
    }
  }

  bool empty() const { return m_set.empty(); }
  std::size_t size() const { return m_set.size(); }
  std::size_t max_size() const { return m_set.max_size(); }
  iterator begin() const { return m_set.cbegin(); }
  iterator end() const { return m_set.cend(); }

  bool contains(const Element &key) const {
    const auto it =
        std::lower_bound(m_set.begin(), m_set.end(), key, comparator());
    return it != m_set.end() && equivalent_keys(*it, key);
  }

  bool is_subset_of(const FlatSet &other) const {
    auto other_it = other.m_set.begin();
    for (const auto &element : m_set) {
      other_it =
          std::lower_bound(other_it, other.m_set.end(), element, comparator());
      if (other_it == other.m_set.end() || !Equal()(element, *other_it)) {
        return false;
      }
      ++other_it;
    }
    return true;
  }

  bool equals(const FlatSet &other) const {
    return m_set.size() == other.m_set.size() &&
           std::equal(m_set.begin(), m_set.end(), other.m_set.begin(), Equal());
  }

  FlatSet &insert(Element key) {
    auto it = std::lower_bound(m_set.begin(), m_set.end(), key, comparator());
    if (it == m_set.end() || !equivalent_keys(*it, key)) {
      m_set.insert(it, std::move(key));
    }
    return *this;
  }

  FlatSet &remove(const Element &key) {
    const auto it =
        std::lower_bound(m_set.begin(), m_set.end(), key, comparator());
    if (it != m_set.end() && equivalent_keys(*it, key)) {
      m_set.erase(it);
    }
    return *this;
  }

  template <typename Visitor> void visit(Visitor &&visitor) const {
    for (const auto &element : m_set) {
      visitor(element);
    }
  }

  const Element *singleton() const {
    return m_set.size() == 1 ? &*m_set.begin() : nullptr;
  }

  template <typename Predicate> FlatSet &filter(Predicate &&predicate) {
    m_set.erase(
        std::remove_if(m_set.begin(), m_set.end(),
                       [predicate = fwd_capture(std::forward<Predicate>(
                            predicate))](const Element &element) mutable {
                         return !predicate.get()(element);
                       }),
        m_set.end());
    return *this;
  }

  FlatSet &union_with(const FlatSet &other) {
    auto it = m_set.begin(), end = m_set.end();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, *other_it, comparator());
      if (it == end || (!Equal()(*it, *other_it) &&
                        !equivalent_keys(*it, *other_it))) {
        it = m_set.insert(it, *other_it);
        end = m_set.end();
      }
      ++it;
      ++other_it;
    }
    return *this;
  }

  FlatSet &intersection_with(const FlatSet &other) {
    if (this == &other) return *this;
    auto first = m_set.begin();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
    for (auto it = m_set.begin(), end = m_set.end(); it != end; ++it) {
      other_it = std::lower_bound(other_it, other_end, *it, comparator());
      if (other_it != other_end && Equal()(*it, *other_it)) {
        if (first != it) *first = std::move(*it);
        ++first;
        ++other_it;
      }
    }
    m_set.erase(first, m_set.end());
    return *this;
  }

  FlatSet &difference_with(const FlatSet &other) {
    if (this == &other) {
      clear();
      return *this;
    }
    auto it = m_set.begin(), end = m_set.end();
    auto other_it = other.m_set.begin(), other_end = other.m_set.end();
    while (other_it != other_end) {
      // Upstream omitted Compare here, breaking non-default orderings.
      it = std::lower_bound(it, end, *other_it, comparator());
      if (it != end && Equal()(*it, *other_it)) {
        it = m_set.erase(it);
        end = m_set.end();
      }
      ++other_it;
    }
    return *this;
  }

  FlatSet get_union_with(const FlatSet& other) const {
    if (m_set.size() > other.m_set.size()) {
      auto result = *this;
      result.union_with(other);
      return result;
    } else {
      auto result = other;
      result.union_with(*this);
      return result;
    }
  }

  FlatSet get_intersection_with(const FlatSet& other) const {
    if (m_set.size() < other.m_set.size()) {
      auto result = *this;
      result.intersection_with(other);
      return result;
    } else {
      auto result = other;
      result.intersection_with(*this);
      return result;
    }
  }

  FlatSet get_difference_with(const FlatSet &other) const {
    auto result = *this;
    result.difference_with(other);
    return result;
  }

  void clear() { m_set.clear(); }

  friend std::ostream &operator<<(std::ostream &output, const FlatSet &set) {
    output << "{";
    for (auto it = set.begin(), end = set.end(); it != end;) {
      output << pt_util::deref(*it);
      ++it;
      if (it != end) {
        output << ", ";
      }
    }
    output << "}";
    return output;
  }

private:
  Storage m_set;
};

} // namespace sparta
