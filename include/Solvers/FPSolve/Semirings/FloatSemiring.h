/**
 * Float Semiring for FPSolve
 */

#ifndef FPSOLVE_FLOAT_SEMIRING_H
#define FPSOLVE_FLOAT_SEMIRING_H

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include <string>
#include <cmath>
#include <limits>

namespace fpsolve {

class FloatSemiring : public StarableSemiring<FloatSemiring,
                                                Commutativity::Commutative,
                                                Idempotence::NonIdempotent> {
private:
  double val;

public:
  FloatSemiring() : val(0.0) {}
  FloatSemiring(double v) : val(v) {}
  FloatSemiring(std::string str_val) : val(std::stod(str_val)) {}

  FloatSemiring operator+=(const FloatSemiring& elem) {
    val = val + elem.val;
    return *this;
  }

  FloatSemiring operator*=(const FloatSemiring& elem) {
    val = val * elem.val;
    return *this;
  }

  bool operator==(const FloatSemiring& elem) const {
    return std::abs(val - elem.val) < 1e-10;
  }

  FloatSemiring star() const {
    if (std::abs(val) < 1.0) {
      return FloatSemiring(1.0 / (1.0 - val));
    }
    return FloatSemiring(std::numeric_limits<double>::infinity());
  }

  static FloatSemiring null() {
    return FloatSemiring(0.0);
  }

  static FloatSemiring one() {
    return FloatSemiring(1.0);
  }

  std::string string() const {
    return std::to_string(val);
  }

  FloatSemiring operator-(const FloatSemiring& elem) const {
    return FloatSemiring(val - elem.val);
  }

  static bool isInf(const FloatSemiring& elem) {
    return std::isinf(elem.val);
  }

  double getValue() const { return val; }
};

} // namespace fpsolve

#endif // FPSOLVE_FLOAT_SEMIRING_H

