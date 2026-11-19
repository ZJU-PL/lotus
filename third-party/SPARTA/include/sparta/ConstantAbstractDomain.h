/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <utility>

#include <sparta/AbstractDomain.h>

namespace sparta {

/*
 * This abstract domain combinator constructs the lattice of constants of a
 * certain type (also called the flat lattice or the three-level lattice). For
 * more detail on constant propagation please see:
 *
 *   https://www.cs.utexas.edu/users/lin/cs380c/wegman.pdf
 *
 * For example, the lattice of integer constants:
 *
 *                       TOP
 *                     /  |  \
 *           ... -2  -1   0   1  2 ....
 *                    \   |   /
 *                       _|_
 *
 * can be implemented as follows:
 *
 *   using Int32ConstantDomain = ConstantAbstractDomain<int32_t>;
 *
 * Note: The base constant elements should be comparable using `operator==()`.
 */

namespace acd_impl {

template <typename Constant>
class ConstantAbstractValue final
    : public AbstractValue<ConstantAbstractValue<Constant>> {
public:
  static_assert(std::is_default_constructible_v<Constant>,
                "Constant is not default constructible");
  static_assert(std::is_copy_constructible_v<Constant>,
                "Constant is not copy constructible");
  static_assert(std::is_copy_assignable_v<Constant>,
                "Constant is not copy assignable");

  ConstantAbstractValue() = default;

  explicit ConstantAbstractValue(Constant constant)
      : m_constant(std::move(constant)) {}

  void clear() {}

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool leq(const ConstantAbstractValue &other) const { return equals(other); }

  bool equals(const ConstantAbstractValue &other) const {
    return m_constant == other.get_constant();
  }

  AbstractValueKind join_with(const ConstantAbstractValue &other) {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    return AbstractValueKind::Top;
  }

  AbstractValueKind widen_with(const ConstantAbstractValue &other) {
    return join_with(other);
  }

  AbstractValueKind meet_with(const ConstantAbstractValue &other) {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    return AbstractValueKind::Bottom;
  }

  AbstractValueKind narrow_with(const ConstantAbstractValue &other) {
    return meet_with(other);
  }

  const Constant &get_constant() const { return m_constant; }

private:
  Constant m_constant{};
};

template <typename Constant, typename = void> class ConstantAbstractValueRepr {
private:
  Constant m_constant{};
  AbstractValueKind m_kind{AbstractValueKind::Top};

public:
  AbstractValueKind kind() const { return m_kind; }
  const Constant &constant() const { return m_constant; }

  void set(AbstractValueKind kind, Constant constant) {
    m_kind = kind;
    m_constant = std::move(constant);
  }
  void set(AbstractValueKind kind) { m_kind = kind; }
};

template <typename Pointer, typename = void>
struct is_aligned_object_pointer : std::false_type {};

template <typename T>
struct is_aligned_object_pointer<
    T*, std::void_t<std::enable_if_t<std::is_object_v<T>>, decltype(sizeof(T))>>
    : std::bool_constant<(alignof(T) >= 4)> {};

// Preserve the upstream one-pointer representation for aligned object
// pointers. Store the tag in an integer to avoid reading an uninitialized or
// non-pointer representation when creating Top and Bottom.
template <typename Constant>
class ConstantAbstractValueRepr<
    Constant, std::enable_if_t<is_aligned_object_pointer<Constant>::value>> {
 public:
  AbstractValueKind kind() const {
    return static_cast<AbstractValueKind>(m_bits & kMask);
  }

  Constant constant() const {
    return reinterpret_cast<Constant>(m_bits & ~kMask);
  }

  void set(AbstractValueKind kind, Constant constant) {
    const auto pointer = reinterpret_cast<std::uintptr_t>(constant);
    assert((pointer & kMask) == 0);
    m_bits = pointer | static_cast<std::uintptr_t>(kind);
  }

  void set(AbstractValueKind kind) {
    m_bits = (m_bits & ~kMask) | static_cast<std::uintptr_t>(kind);
  }

 private:
  static constexpr std::uintptr_t kMask = 3;
  static_assert(static_cast<unsigned>(AbstractValueKind::Top) <= kMask);
  static_assert(static_cast<unsigned>(AbstractValueKind::Value) <= kMask);
  static_assert(static_cast<unsigned>(AbstractValueKind::Bottom) <= kMask);
  std::uintptr_t m_bits{static_cast<std::uintptr_t>(AbstractValueKind::Top)};
};

} // namespace acd_impl

template <typename Constant>
class ConstantAbstractDomain final
    : public AbstractDomain<ConstantAbstractDomain<Constant>> {
public:
  using ReprType = acd_impl::ConstantAbstractValueRepr<Constant>;
  using ConstantType = Constant;

  ConstantAbstractDomain() { m_repr.set(AbstractValueKind::Top); }

  explicit ConstantAbstractDomain(Constant cst) {
    m_repr.set(AbstractValueKind::Value, std::move(cst));
  }

  /*
   * A convenience constructor for creating Bottom and Top.
   */
  explicit ConstantAbstractDomain(AbstractValueKind kind) {
    m_repr.set(kind);
    SPARTA_RUNTIME_CHECK(kind != AbstractValueKind::Value,
                         invalid_abstract_value() << actual_kind(kind));
  }

  std::optional<Constant> get_constant() const {
    return (m_repr.kind() == AbstractValueKind::Value)
               ? std::optional<Constant>(m_repr.constant())
               : std::nullopt;
  }

  static ConstantAbstractDomain bottom() {
    return ConstantAbstractDomain(AbstractValueKind::Bottom);
  }

  static ConstantAbstractDomain top() {
    return ConstantAbstractDomain(AbstractValueKind::Top);
  }

  friend std::ostream &
  operator<<(std::ostream &out,
             const typename sparta::ConstantAbstractDomain<Constant> &x) {
    using namespace sparta;
    switch (x.kind()) {
    case AbstractValueKind::Bottom: {
      out << "_|_";
      break;
    }
    case AbstractValueKind::Top: {
      out << "T";
      break;
    }
    case AbstractValueKind::Value: {
      out << *x.get_constant();
      break;
    }
    }
    return out;
  }

  AbstractValueKind kind() const { return m_repr.kind(); }

  bool is_bottom() const { return m_repr.kind() == AbstractValueKind::Bottom; }

  bool is_top() const { return m_repr.kind() == AbstractValueKind::Top; }

  bool is_value() const { return m_repr.kind() == AbstractValueKind::Value; }

  void set_to_bottom() { m_repr.set(AbstractValueKind::Bottom); }

  void set_to_top() { m_repr.set(AbstractValueKind::Top); }

  bool leq(const ConstantAbstractDomain<Constant> &other) const {
    if (is_bottom()) {
      return true;
    }
    if (other.is_bottom()) {
      return false;
    }
    if (other.is_top()) {
      return true;
    }
    if (is_top()) {
      return false;
    }
    return m_repr.constant() == other.m_repr.constant();
  }

  bool equals(const ConstantAbstractDomain<Constant> &other) const {
    if (is_bottom()) {
      return other.is_bottom();
    }
    if (is_top()) {
      return other.is_top();
    }
    if (!other.is_value()) {
      return false;
    }
    return m_repr.constant() == other.m_repr.constant();
  }

  void join_with(const ConstantAbstractDomain<Constant> &other) {
    if (is_top() || other.is_bottom()) {
      return;
    }
    if (other.is_top()) {
      set_to_top();
      return;
    }
    if (is_bottom()) {
      m_repr = other.m_repr;
      return;
    }
    m_repr.set(m_repr.constant() == other.m_repr.constant()
                   ? AbstractValueKind::Value
                   : AbstractValueKind::Top);
  }

  void widen_with(const ConstantAbstractDomain<Constant> &other) {
    return join_with(other);
  }

  void meet_with(const ConstantAbstractDomain<Constant> &other) {
    if (is_bottom() || other.is_top()) {
      return;
    }
    if (other.is_bottom()) {
      set_to_bottom();
      return;
    }
    if (is_top()) {
      m_repr = other.m_repr;
      return;
    }
    m_repr.set(m_repr.constant() == other.m_repr.constant()
                   ? AbstractValueKind::Value
                   : AbstractValueKind::Bottom);
  }

  void narrow_with(const ConstantAbstractDomain<Constant> &other) {
    return meet_with(other);
  }

private:
  ReprType m_repr;
};

} // namespace sparta
