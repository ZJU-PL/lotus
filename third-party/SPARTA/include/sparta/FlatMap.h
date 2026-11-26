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
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <sparta/AbstractMap.h>
#include <sparta/AbstractMapValue.h>
#include <sparta/PatriciaTreeCore.h>
#include <sparta/PerfectForwardCapture.h>

namespace sparta {
namespace fm_impl {

template <typename T, typename = void> struct is_container : std::false_type {};

template <typename T>
struct is_container<
    T, std::void_t<typename T::iterator, typename T::const_iterator,
                   decltype(std::declval<const T &>().begin()),
                   decltype(std::declval<const T &>().end())>>
    : std::true_type {};

template <typename Key, typename Value, typename ValueInterface,
          typename KeyCompare, typename KeyEqual, typename AllocatorOrContainer>
class FlatMapStaticAssert;

} // namespace fm_impl

/*
 * Represents a map implemented with a sorted vector.
 *
 * This implementation replaces Boost.Container's flat_map while preserving
 * the map operations used by SPARTA. Passing a container as the final template
 * argument remains supported, as does passing an allocator.
 */
template <typename Key, typename Value,
          typename ValueInterface = pt_core::SimpleValue<Value>,
          typename KeyCompare = std::less<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename AllocatorOrContainer = std::allocator<std::pair<Key, Value>>>
class FlatMap final
    : public AbstractMap<FlatMap<Key, Value, ValueInterface, KeyCompare,
                                 KeyEqual, AllocatorOrContainer>>,
      private fm_impl::FlatMapStaticAssert<Key, Value, ValueInterface,
                                           KeyCompare, KeyEqual,
                                           AllocatorOrContainer>,
      private std::tuple<KeyCompare> {
public:
  using key_type = Key;
  using mapped_type = typename ValueInterface::type;
  using value_type = std::pair<Key, Value>;

private:
  using Storage =
      std::conditional_t<fm_impl::is_container<AllocatorOrContainer>::value,
                         AllocatorOrContainer,
                         std::vector<value_type, AllocatorOrContainer>>;

public:
  using iterator = typename Storage::const_iterator;
  using const_iterator = iterator;
  using difference_type = typename Storage::difference_type;
  using size_type = typename Storage::size_type;
  using const_reference = typename Storage::const_reference;
  using const_pointer = typename Storage::const_pointer;

  constexpr static AbstractMapMutability mutability =
      AbstractMapMutability::Mutable;

private:
  const KeyCompare& key_compare() const {
    return std::get<0>(static_cast<const std::tuple<KeyCompare>&>(*this));
  }

  struct ComparePairWithKey {
    const KeyCompare* compare;
    bool operator()(const value_type &pair, const Key &key) const {
      return (*compare)(pair.first, key);
    }
  };

  struct PairEqual {
    bool operator()(const value_type &left, const value_type &right) const {
      return KeyEqual()(left.first, right.first) &&
             ValueInterface::equals(left.second, right.second);
    }
  };

  iterator lower_bound(const Key &key) const {
    return std::lower_bound(m_map.begin(), m_map.end(), key,
                            ComparePairWithKey{&key_compare()});
  }

  bool equivalent_keys(const Key& left, const Key& right) const {
    return !key_compare()(left, right) && !key_compare()(right, left);
  }

  void erase_default_values() {
    this->filter([](const Key &, const mapped_type &value) {
      return !ValueInterface::is_default_value(value);
    });
  }

public:
  FlatMap() = default;

  explicit FlatMap(std::initializer_list<std::pair<Key, Value>> values) {
    for (const auto &value : values) {
      insert_or_assign(value.first, value.second);
    }
  }

  bool empty() const { return m_map.empty(); }

  size_t size() const { return m_map.size(); }

  size_t max_size() const { return m_map.max_size(); }

  iterator begin() const { return m_map.cbegin(); }

  iterator end() const { return m_map.cend(); }

  const mapped_type &at(const Key &key) const {
    const auto it = lower_bound(key);
    if (it != m_map.end() && equivalent_keys(it->first, key)) {
      return it->second;
    }
    static const mapped_type default_value = ValueInterface::default_value();
    return default_value;
  }

  FlatMap &remove(const Key &key) {
    const auto it = lower_bound(key);
    if (it != m_map.end() && equivalent_keys(it->first, key)) {
      m_map.erase(it);
    }
    return *this;
  }

  template <typename V> FlatMap &insert_or_assign(const Key &key, V &&value) {
    if (ValueInterface::is_default_value(value)) {
      return remove(key);
    }

    auto it =
        std::lower_bound(m_map.begin(), m_map.end(), key, ComparePairWithKey{&key_compare()});
    if (it != m_map.end() && equivalent_keys(it->first, key)) {
      it->second = std::forward<V>(value);
    } else {
      m_map.emplace(it, key, std::forward<V>(value));
    }
    return *this;
  }

  template <typename Operation>
  FlatMap &update(Operation &&operation, const Key &key) {
    auto it =
        std::lower_bound(m_map.begin(), m_map.end(), key, ComparePairWithKey{&key_compare()});
    if (it == m_map.end() || !equivalent_keys(it->first, key)) {
      it = m_map.emplace(it, key, ValueInterface::default_value());
    }
    operation(&it->second);
    if (ValueInterface::is_default_value(it->second)) {
      m_map.erase(it);
    }
    return *this;
  }

 private:
  bool leq_when_default_is_top(const FlatMap& other) const {
    if (m_map.size() < other.m_map.size()) {
      // In this case, there is a key bound to a non-Top value in 'other'
      // that is not defined in 'this' (and is therefore implicitly bound to
      // Top).
      return false;
    }

    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (other_it != other_end) {
      if (std::distance(it, end) < std::distance(other_it, other_end)) {
        // Same logic as above: there is a key bound to a non-Top value between
        // [other_it, other_end] that is not defined within [it, end].
        return false;
      }
      // Performs a binary search (in O(log(n))) which returns an iterator on
      // the first pair where `it->first >= other_it->first`.
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey{&key_compare()});
      if (it == end || !KeyEqual()(it->first, other_it->first)) {
        return false;
      }
      if (!ValueInterface::leq(it->second, other_it->second)) {
        return false;
      } else {
        ++it;
        ++other_it;
      }
    }
    return true;
  }

  bool leq_when_default_is_bottom(const FlatMap& other) const {
    if (m_map.size() > other.m_map.size()) {
      // `this` has at least one non-default binding that `other` doesn't have.
      // There exists a key such that this[key] != Bottom and other[key] ==
      // Bottom.
      return false;
    }

    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (it != end) {
      if (std::distance(it, end) > std::distance(other_it, other_end)) {
        // Same logic as above: there is a non-default binding in [it, end]
        // that does not exist in [it, end], hence is bound to bottom.
        return false;
      }
      // Performs a binary search (in O(log(n))) which returs an iterator on the
      // first pair where `other_it->first >= it->first`.
      other_it = std::lower_bound(other_it, other_end, it->first,
                                  ComparePairWithKey{&key_compare()});
      if (other_it == other_end || !KeyEqual()(it->first, other_it->first)) {
        return false;
      }
      if (!ValueInterface::leq(it->second, other_it->second)) {
        return false;
      } else {
        ++it;
        ++other_it;
      }
    }

    return true;
  }

 public:
  bool leq(const FlatMap& other) const {
    static_assert(std::is_base_of_v<AbstractDomain<Value>, Value>,
                  "leq can only be used when Value implements AbstractDomain");

    // Assumes ValueInterface::default_value() is either Top or Bottom.
    if constexpr (ValueInterface::default_value_kind ==
                  AbstractValueKind::Top) {
      return this->leq_when_default_is_top(other);
    } else if constexpr (ValueInterface::default_value_kind ==
                         AbstractValueKind::Bottom) {
      return this->leq_when_default_is_bottom(other);
    } else {
      static_assert(
          ValueInterface::default_value_kind == AbstractValueKind::Top ||
              ValueInterface::default_value_kind == AbstractValueKind::Bottom,
          "leq can only be used when ValueInterface::default_value() is top or "
          "bottom");
    }
  }

  bool equals(const FlatMap &other) const {
    return m_map.size() == other.m_map.size() &&
           std::equal(m_map.begin(), m_map.end(), other.m_map.begin(),
                      PairEqual());
  }

  template <typename MappingFunction>
  void transform(MappingFunction &&function) {
    bool has_default = false;
    for (auto &binding : m_map) {
      function(&binding.second);
      has_default |= ValueInterface::is_default_value(binding.second);
    }
    if (has_default) erase_default_values();
  }

  template <typename Visitor> void visit(Visitor &&visitor) const {
    for (const auto &binding : m_map) {
      visitor(binding);
    }
  }

  template <typename Predicate> FlatMap &filter(Predicate &&predicate) {
    m_map.erase(std::remove_if(m_map.begin(), m_map.end(),
                               [predicate = fwd_capture(std::forward<Predicate>(
                                    predicate))](const auto &binding) mutable {
                                 return !predicate.get()(binding.first,
                                                         binding.second);
                               }),
                m_map.end());
    return *this;
  }

  bool erase_all_matching(const Key &) {
    throw std::logic_error("erase_all_matching is specific to Patricia trees");
  }

  template <typename CombiningFunction>
  FlatMap &union_with(CombiningFunction &&combine, const FlatMap &other) {
    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey{&key_compare()});
      if (it == end) {
        m_map.insert(end, other_it, other_end);
        break;
      }
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
      } else if (!equivalent_keys(it->first, other_it->first)) {
        it = m_map.insert(it, *other_it);
        end = m_map.end();
      }
      ++it;
      ++other_it;
    }
    erase_default_values();
    return *this;
  }

  template <typename CombiningFunction>
  FlatMap &intersection_with(CombiningFunction &&combine, const FlatMap &other) {
    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (it != end) {
      other_it = std::lower_bound(other_it, other_end, it->first, ComparePairWithKey{&key_compare()});
      if (other_it == other_end) {
        m_map.erase(it, end);
        break;
      }
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
        ++other_it;
      } else {
        it->second = ValueInterface::default_value();
      }
      ++it;
    }
    erase_default_values();
    return *this;
  }

  template <typename CombiningFunction>
  FlatMap &difference_with(CombiningFunction &&combine, const FlatMap &other) {
    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey{&key_compare()});
      if (it == end) break;
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
        ++it;
      }
      ++other_it;
    }
    erase_default_values();
    return *this;
  }

  void clear() { m_map.clear(); }

  friend std::ostream &operator<<(std::ostream &output, const FlatMap &map) {
    using namespace sparta;
    output << "{";
    for (auto it = map.begin(); it != map.end(); ++it) {
      output << pt_util::deref(it->first) << " -> " << it->second;
      if (std::next(it) != map.end()) {
        output << ", ";
      }
    }
    output << "}";
    return output;
  }

private:
  Storage m_map;
};

namespace fm_impl {

template <typename Key, typename Value, typename ValueInterface,
          typename KeyCompare, typename KeyEqual, typename AllocatorOrContainer>
class FlatMapStaticAssert {
protected:
  ~FlatMapStaticAssert() {
    static_assert(std::is_same_v<Value, typename ValueInterface::type>,
                  "Value must be equal to ValueInterface::type");
    static_assert(std::is_base_of<AbstractMapValue<ValueInterface>,
                                  ValueInterface>::value,
                  "ValueInterface doesn't inherit from AbstractMapValue");
    SPARTA_UNUSED_VARIABLE(static_cast<KeyCompare *>(nullptr));
    SPARTA_UNUSED_VARIABLE(static_cast<KeyEqual *>(nullptr));
    SPARTA_UNUSED_VARIABLE(static_cast<AllocatorOrContainer *>(nullptr));
    ValueInterface::check_interface();
  }
};

} // namespace fm_impl

} // namespace sparta
