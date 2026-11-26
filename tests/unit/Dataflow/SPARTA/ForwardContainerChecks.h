/* Copyright (c) Meta Platforms, Inc. and affiliates. MIT license. */
#pragma once
#include <iterator>
#include <type_traits>
#include <utility>

namespace sparta_test {
// Replace upstream's Boost ForwardContainer assertions with C++17 checks of
// the same exposed container and multipass iterator operations.
template <typename Container> constexpr bool forward_container() {
  using Iterator = typename Container::const_iterator;
  using Traits = std::iterator_traits<Iterator>;
  static_assert(std::is_default_constructible_v<Container>);
  static_assert(std::is_copy_constructible_v<Container>);
  static_assert(std::is_copy_assignable_v<Container>);
  static_assert(std::is_same_v<typename Traits::value_type,
                               typename Container::value_type>);
  static_assert(std::is_base_of_v<std::forward_iterator_tag,
                                  typename Traits::iterator_category>);
  static_assert(std::is_copy_constructible_v<Iterator>);
  static_assert(std::is_copy_assignable_v<Iterator>);
  static_assert(
      std::is_same_v<decltype(++std::declval<Iterator &>()), Iterator &>);
  static_assert(
      std::is_same_v<decltype(std::declval<Iterator &>()++), Iterator>);
  static_assert(std::is_same_v<decltype(*std::declval<Iterator &>()),
                               typename Container::const_reference>);
  static_assert(
      std::is_same_v<decltype(std::declval<const Container &>().begin()),
                     Iterator>);
  static_assert(
      std::is_same_v<decltype(std::declval<const Container &>().end()),
                     Iterator>);
  static_assert(std::is_unsigned_v<typename Container::size_type>);
  static_assert(std::is_signed_v<typename Traits::difference_type>);
  return true;
}
} // namespace sparta_test
