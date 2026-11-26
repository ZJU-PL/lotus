/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <optional>
#include <sstream>
#include <type_traits>
#include <variant>

#include <sparta/AbstractDomain.h>

// Forward declarations.
namespace sparta {
namespace duad_impl {
template <typename Visitor, typename = void>
struct HasResultType : std::false_type {};
template <typename Visitor>
struct HasResultType<Visitor, std::void_t<typename Visitor::result_type>>
    : std::true_type {};

template <typename Visitor, typename... Variants>
decltype(auto) visit(const Visitor& visitor, const Variants&... variants) {
  if constexpr (HasResultType<Visitor>::value) {
    using Result = typename Visitor::result_type;
    return std::visit([&](const auto&... domains) -> Result {
      if constexpr (std::is_void_v<Result>) visitor(domains...);
      else return visitor(domains...);
    }, variants...);
  } else {
    return std::visit(visitor, variants...);
  }
}

template <typename FirstDomain, typename... Domains>
class DisjointUnionAbstractDomainStaticAssert;
} // namespace duad_impl

template <typename FirstDomain, typename... Domains>
class DisjointUnionAbstractDomain;

} // namespace sparta

template <typename... Domains>
std::ostream &
operator<<(std::ostream &,
           const sparta::DisjointUnionAbstractDomain<Domains...> &);

namespace sparta {

/*
 * The disjoint union of abstract domains D1 ... Dn can hold any one of those
 * n domains. The join and meet of different domains is always Top and Bottom
 * respectively.
 *
 * In this paper[*], this construction is called the cardinal sum
 * (remark 10.1.10.4 at the top right corner of page 281). The cardinal sum is
 * endowed with two new extremal elements (i.e. Top and Bottom). In our
 * implementation, we treat these extremal elements as equivalent to the
 * respective extremal elements of the component domains. This construction is
 * meant for use cases where the component abstract domains have mostly disjoint
 * denotations. Hence, coalescing the extremal elements shouldn't have any
 * impact on the precision of the analysis in practice. (If the domains were
 * not disjoint, coalescing could lose us information -- for example, if some
 * concrete value is Top in the abstract domain L1 but not Top in L2, coalescing
 * means that x will be abstracted to the Top element in L1+L2.)
 *
 * [*]: Patrick Cousot & Radhia Cousot. Systematic design of program analysis
 * frameworks. POPL'79, pp 269—282.
 * https://cs.nyu.edu/~pcousot/publications.www/CousotCousot-POPL-79-ACM-p269--282-1979.pdf
 */
template <typename FirstDomain, typename... Domains>
class DisjointUnionAbstractDomain final
    : public AbstractDomain<
          DisjointUnionAbstractDomain<FirstDomain, Domains...>>,
      private duad_impl::DisjointUnionAbstractDomainStaticAssert<FirstDomain,
                                                                 Domains...> {

public:
  DisjointUnionAbstractDomain() : m_variant(make_storage(FirstDomain::top())) {}

  DisjointUnionAbstractDomain(const DisjointUnionAbstractDomain& other)
      : m_variant(make_storage(other.variant())) {}
  DisjointUnionAbstractDomain(DisjointUnionAbstractDomain&& other)
      : m_variant(make_storage(std::move(other.variant()))) {}
  DisjointUnionAbstractDomain& operator=(const DisjointUnionAbstractDomain& other) {
    if (this != &other) assign_variant(other.variant());
    return *this;
  }
  DisjointUnionAbstractDomain& operator=(DisjointUnionAbstractDomain&& other) {
    if (this != &other) {
      if constexpr (kInline) m_variant = std::move(other.variant());
      else m_variant = make_storage(other.variant());
    }
    return *this;
  }

  template <typename Domain>
  /* implicit */ DisjointUnionAbstractDomain(Domain d)
      : m_variant(make_storage(std::move(d))) {}

  static DisjointUnionAbstractDomain top() {
    return DisjointUnionAbstractDomain(FirstDomain::top());
  }

  static DisjointUnionAbstractDomain bottom() {
    return DisjointUnionAbstractDomain(FirstDomain::bottom());
  }

  bool is_top() const {
    return std::visit([](const auto &dom) { return dom.is_top(); },
                      this->variant());
  }

  bool is_bottom() const {
    return std::visit([](const auto &dom) { return dom.is_bottom(); },
                      this->variant());
  }

  void set_to_top() {
    std::visit([](auto &dom) { dom.set_to_top(); }, this->variant());
  }

  void set_to_bottom() {
    std::visit([](auto &dom) { dom.set_to_bottom(); }, this->variant());
  }

  bool
  leq(const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) const;

  bool equals(
      const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) const;

  void
  join_with(const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other);

  void widen_with(
      const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) {
    this->join_with(other);
  }

  void
  meet_with(const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other);

  void narrow_with(
      const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) {
    this->meet_with(other);
  }

  /*
   * This will throw if the domain contained in the union differs from the
   * requested Domain.
   */
  template <typename Domain> const Domain get() const {
    if (is_top()) {
      return Domain::top();
    }
    if (is_bottom()) {
      return Domain::bottom();
    }
    return std::get<Domain>(variant());
  }

  template <typename Domain> const std::optional<Domain> maybe_get() const {
    if (is_top()) {
      return Domain::top();
    }
    if (is_bottom()) {
      return Domain::bottom();
    }
    auto *dom = std::get_if<Domain>(&variant());
    if (dom == nullptr) {
      return std::nullopt;
    }
    return *dom;
  }

  template <typename Domain>
  void apply(std::function<void(Domain *)> operation) {
    auto *dom = std::get_if<Domain>(&variant());
    if (dom) {
      operation(dom);
    }
  }

  /*
   * Return a numeric index representing the current type, if any.
   */
  std::optional<int> which() const {
    if (is_top() || is_bottom()) {
      return std::nullopt;
    }
    return static_cast<int>(variant().index());
  }

  template <typename Visitor>
  static decltype(auto) apply_visitor(
      const Visitor &visitor,
      const DisjointUnionAbstractDomain<FirstDomain, Domains...> &dom) {
    return duad_impl::visit(visitor, dom.variant());
  }

  template <typename Visitor>
  static decltype(auto) apply_visitor(
      const Visitor &visitor,
      const DisjointUnionAbstractDomain<FirstDomain, Domains...> &d1,
      const DisjointUnionAbstractDomain<FirstDomain, Domains...> &d2) {
    return duad_impl::visit(visitor, d1.variant(), d2.variant());
  }

  template <typename... Ts>
  friend std::ostream & ::operator<<(
      std::ostream &, const DisjointUnionAbstractDomain<Ts...> &);

private:
  using Variant = std::variant<FirstDomain, Domains...>;
  // std::variant may become valueless when an alternative's copy/move throws.
  // Use inline storage for nothrow-movable domains (the common case); for
  // throwing alternatives, prepare a new variant before changing ownership.
  // Each domain owns its storage, so copies never share mutable lattice state.
  static constexpr bool kInline =
      std::conjunction_v<std::is_nothrow_move_constructible<FirstDomain>,
                         std::is_nothrow_move_constructible<Domains>...>;
  using Storage = std::conditional_t<kInline, Variant, std::unique_ptr<Variant>>;

  template <typename Value>
  static Storage make_storage(Value&& value) {
    if constexpr (kInline) return Variant(std::forward<Value>(value));
    else if constexpr (std::is_same_v<std::decay_t<Value>, Variant>) {
      // Like the reference variant, prefer copying when moving an
      // alternative could throw. This also leaves the source usable.
      return std::make_unique<Variant>(value);
    } else return std::make_unique<Variant>(std::forward<Value>(value));
  }
  Variant& variant() {
    if constexpr (kInline) return m_variant;
    else return *m_variant;
  }
  const Variant& variant() const {
    if constexpr (kInline) return m_variant;
    else return *m_variant;
  }
  void assign_variant(const Variant& value) {
    if (&value == &variant()) return;
    if constexpr (kInline) m_variant = value;
    else m_variant = make_storage(value);
  }
  Storage m_variant;
};

} // namespace sparta

template <typename... Domains>
std::ostream &
operator<<(std::ostream &o,
           const typename sparta::DisjointUnionAbstractDomain<Domains...> &du) {
  o << "[U] ";
  std::visit([&o](const auto &dom) { o << dom; }, du.variant());
  return o;
}

namespace sparta {

namespace duad_impl {

template <typename FirstDomain, typename... Domains>
class DisjointUnionAbstractDomainStaticAssert {
protected:
  ~DisjointUnionAbstractDomainStaticAssert() {
    static_assert(
        std::conjunction_v<
            std::is_base_of<AbstractDomain<FirstDomain>, FirstDomain>,
            std::is_base_of<AbstractDomain<Domains>, Domains>...>,
        "All members of the disjoint union must inherit from AbstractDomain");
  }
};

/*
 * Top and Bottom are canonicalized via the leq and equals predicates, which
 * implement an equivalence relation on the extremal elements. Hence, even
 * though the actual type of the variant may vary, the different Top/Bottom
 * values of the underlying component domains are indistinguishable through the
 * AbstractDomain interface.
 */

class leq_visitor {
public:
  template <typename Domain>
  bool operator()(const Domain &d1, const Domain &d2) const {
    return d1.leq(d2);
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain &d1, const OtherDomain &d2) const {
    if (d1.is_bottom()) {
      return true;
    }
    if (d2.is_bottom()) {
      return false;
    }
    if (d2.is_top()) {
      return true;
    }
    if (d1.is_top()) {
      return false;
    }
    return false;
  }
};

class equals_visitor {
public:
  template <typename Domain>
  bool operator()(const Domain &d1, const Domain &d2) const {
    return d1.equals(d2);
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain &d1, const OtherDomain &d2) const {
    if (d1.is_bottom()) {
      return d2.is_bottom();
    }
    if (d1.is_top()) {
      return d2.is_top();
    }
    return false;
  }
};

class join_visitor {
public:
  template <typename Domain>
  void operator()(Domain &d1, const Domain &d2) const {
    d1.join_with(d2);
  }

  template <typename Domain, typename OtherDomain>
  void operator()(Domain &d1, const OtherDomain &) const {
    d1.set_to_top();
  }
};

class meet_visitor {
public:
  template <typename Domain>
  void operator()(Domain &d1, const Domain &d2) const {
    d1.meet_with(d2);
  }

  template <typename Domain, typename OtherDomain>
  void operator()(Domain &d1, const OtherDomain &) const {
    d1.set_to_bottom();
  }
};

} // namespace duad_impl

template <typename FirstDomain, typename... Domains>
void DisjointUnionAbstractDomain<FirstDomain, Domains...>::join_with(
    const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) {
  if (this->is_bottom()) {
    this->assign_variant(other.variant());
    return;
  }
  if (other.is_bottom()) {
    return;
  }
  std::visit(duad_impl::join_visitor(), this->variant(), other.variant());
}

template <typename FirstDomain, typename... Domains>
void DisjointUnionAbstractDomain<FirstDomain, Domains...>::meet_with(
    const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) {
  if (this->is_top()) {
    this->assign_variant(other.variant());
    return;
  }
  if (other.is_top()) {
    return;
  }
  std::visit(duad_impl::meet_visitor(), this->variant(), other.variant());
}

template <typename FirstDomain, typename... Domains>
bool DisjointUnionAbstractDomain<FirstDomain, Domains...>::leq(
    const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) const {
  return std::visit(duad_impl::leq_visitor(), this->variant(), other.variant());
}

template <typename FirstDomain, typename... Domains>
bool DisjointUnionAbstractDomain<FirstDomain, Domains...>::equals(
    const DisjointUnionAbstractDomain<FirstDomain, Domains...> &other) const {
  return std::visit(duad_impl::equals_visitor(), this->variant(),
                    other.variant());
}

} // namespace sparta
