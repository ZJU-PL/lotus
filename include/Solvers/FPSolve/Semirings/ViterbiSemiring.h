/**
 * Viterbi Semiring for probabilistic reasoning
 */

#ifndef FPSOLVE_VITERBI_SEMIRING_H
#define FPSOLVE_VITERBI_SEMIRING_H

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include <algorithm>
#include <string>

namespace fpsolve {

class ViterbiSemiring : public StarableSemiring<ViterbiSemiring,
                                                 Commutativity::Commutative,
                                                 Idempotence::Idempotent> {
private:
  double val;

public:
  ViterbiSemiring() : val(0.0) {}
  ViterbiSemiring(double v) : val(std::min(1.0, std::max(0.0, v))) {}
  ViterbiSemiring(std::string str_val) : val(std::stod(str_val)) {
    val = std::min(1.0, std::max(0.0, val));
  }

  ViterbiSemiring operator+=(const ViterbiSemiring& elem) {
    val = std::max(val, elem.val);  // Max for addition
    return *this;
  }

  ViterbiSemiring operator*=(const ViterbiSemiring& elem) {
    val = val * elem.val;  // Product for multiplication
    return *this;
  }

  bool operator==(const ViterbiSemiring& elem) const {
    return std::abs(val - elem.val) < 1e-10;
  }

  ViterbiSemiring star() const {
    return ViterbiSemiring::one();  // 1 for idempotent semiring
  }

  static ViterbiSemiring null() {
    return ViterbiSemiring(0.0);
  }

  static ViterbiSemiring one() {
    return ViterbiSemiring(1.0);
  }

  std::string string() const {
    return std::to_string(val);
  }

  double getValue() const { return val; }
};

} // namespace fpsolve

#endif // FPSOLVE_VITERBI_SEMIRING_H

