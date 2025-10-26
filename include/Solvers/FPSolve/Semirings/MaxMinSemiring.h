/**
 * MaxMin Semiring (dual of Viterbi)
 */

#ifndef FPSOLVE_MAXMIN_SEMIRING_H
#define FPSOLVE_MAXMIN_SEMIRING_H

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include <algorithm>
#include <string>
#include <limits>

namespace fpsolve {

class MaxMinSemiring : public StarableSemiring<MaxMinSemiring,
                                                Commutativity::Commutative,
                                                Idempotence::Idempotent> {
private:
  double val;

public:
  MaxMinSemiring() : val(-std::numeric_limits<double>::infinity()) {}
  MaxMinSemiring(double v) : val(v) {}
  MaxMinSemiring(std::string str_val) : val(std::stod(str_val)) {}

  MaxMinSemiring operator+=(const MaxMinSemiring& elem) {
    val = std::max(val, elem.val);  // Max for addition
    return *this;
  }

  MaxMinSemiring operator*=(const MaxMinSemiring& elem) {
    val = std::min(val, elem.val);  // Min for multiplication
    return *this;
  }

  bool operator==(const MaxMinSemiring& elem) const {
    return std::abs(val - elem.val) < 1e-10;
  }

  MaxMinSemiring star() const {
    return MaxMinSemiring::one();
  }

  static MaxMinSemiring null() {
    return MaxMinSemiring(-std::numeric_limits<double>::infinity());
  }

  static MaxMinSemiring one() {
    return MaxMinSemiring(std::numeric_limits<double>::infinity());
  }

  std::string string() const {
    if (std::isinf(val)) {
      return val < 0 ? "-inf" : "inf";
    }
    return std::to_string(val);
  }

  double getValue() const { return val; }
};

} // namespace fpsolve

#endif // FPSOLVE_MAXMIN_SEMIRING_H

