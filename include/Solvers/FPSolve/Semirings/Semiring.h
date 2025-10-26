/**
 * Semiring base class for FPSolve
 */

#ifndef FPSOLVE_SEMIRING_H
#define FPSOLVE_SEMIRING_H

#include <cstdint>
#include <iosfwd>
#include <string>
#include <functional>
#include <iostream>

namespace fpsolve {

enum class Commutativity { NonCommutative, Commutative };
enum class Idempotence { NonIdempotent, Idempotent };

namespace detail {

template <Commutativity C>
struct IsCommutative_;

template <>
struct IsCommutative_<Commutativity::Commutative> {
  static constexpr bool value = true;
};

template <>
struct IsCommutative_<Commutativity::NonCommutative> {
  static constexpr bool value = false;
};

template <Idempotence C>
struct IsIdempotent_;

template <>
struct IsIdempotent_<Idempotence::Idempotent> {
  static constexpr bool value = true;
};

template <>
struct IsIdempotent_<Idempotence::NonIdempotent> {
  static constexpr bool value = false;
};

} // namespace detail

template <typename SR, Commutativity Comm, Idempotence Idem>
class Semiring {
public:
  virtual ~Semiring() = default;

  friend SR operator*(const SR& lhs, const SR& rhs) {
    SR result = lhs;
    result *= rhs;
    return result;
  }

  friend SR operator*(const SR& lhs, const std::uint_fast16_t& rhs) {
    SR result = lhs;
    result *= rhs;
    return result;
  }

  friend SR operator+(const SR& lhs, const SR& rhs) {
    SR result = lhs;
    result += rhs;
    return result;
  }

  // Compute power by binary exponentiation
  friend SR pow(const SR& lhs, const std::uint_fast16_t& exp) {
    if (0 == exp || lhs == SR::one()) {
      return SR::one();
    }

    if (lhs == SR::null()) {
      return SR::null();
    }

    SR tmp = lhs;
    std::uint_fast16_t tmpexp = exp;
    SR result = SR::one();

    while (true) {
      if (tmpexp % 2 == 1) {
        result *= tmp;
      }
      tmpexp = tmpexp / 2;

      if (0 == tmpexp)
        break;
      tmp *= tmp;
    }

    return result;
  }

  virtual bool operator==(const SR& elem) const = 0;

  virtual bool operator<(const SR& elem) const {
    return string() < elem.string();
  }

  friend bool operator!=(const SR& lhs, const SR& rhs) {
    return !(lhs == rhs);
  }

  static constexpr Commutativity GetCommutativity() { return Comm; }
  static constexpr bool IsCommutative() { return detail::IsCommutative_<Comm>::value; }

  static constexpr Idempotence GetIdempotence() { return Idem; }
  static constexpr bool IsIdempotent() { return detail::IsIdempotent_<Idem>::value; }

  friend SR operator*=(SR& lhs, const std::uint_fast16_t& cnt) {
    if (0 == cnt) {
      lhs = SR::null();
      return SR::null();
    }
    SR result = lhs;
    if (!IsIdempotent()) {
      for (std::uint_fast16_t i = 1; i < cnt; ++i) {
        result += lhs;
      }
    }
    lhs = result;
    return result;
  }

  static SR null();
  static SR one();
  virtual std::string string() const = 0;

  size_t operator()(const SR& sr) const {
    return std::hash<std::string>()(sr.string());
  }
};

template <typename SR>
SR operator*=(SR& lhs, const SR& rhs);

template <typename SR>
SR operator+=(SR& lhs, const SR& rhs);

template <typename SR, Commutativity Comm, Idempotence Idem>
std::ostream& operator<<(std::ostream& os, const Semiring<SR, Comm, Idem>& elem) {
  return os << elem.string();
}

template <typename SR, Commutativity Comm, Idempotence Idem>
class StarableSemiring : public Semiring<SR, Comm, Idem> {
public:
  virtual SR star() const = 0;
  virtual ~StarableSemiring() = default;
};

} // namespace fpsolve

#endif // FPSOLVE_SEMIRING_H

