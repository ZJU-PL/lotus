/**
 * Tropical Semiring for FPSolve
 */

#ifndef FPSOLVE_TROPICAL_SEMIRING_H
#define FPSOLVE_TROPICAL_SEMIRING_H

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include <string>
#include <limits>

namespace fpsolve {

class TropicalSemiring : public StarableSemiring<TropicalSemiring,
                                                   Commutativity::Commutative,
                                                   Idempotence::Idempotent> {
private:
  int val;
  static constexpr int INFTY = std::numeric_limits<int>::max();

public:
  TropicalSemiring() : val(INFTY) {}
  TropicalSemiring(int v) : val(v) {}
  TropicalSemiring(std::string str_val);

  TropicalSemiring operator+=(const TropicalSemiring& elem) {
    val = std::min(val, elem.val);
    return *this;
  }

  TropicalSemiring operator*=(const TropicalSemiring& elem) {
    if (val == INFTY || elem.val == INFTY) {
      val = INFTY;
    } else {
      val = val + elem.val;
    }
    return *this;
  }

  bool operator==(const TropicalSemiring& elem) const {
    return val == elem.val;
  }

  TropicalSemiring star() const {
    return TropicalSemiring::one(); // 0 in tropical
  }

  static TropicalSemiring null() {
    return TropicalSemiring(INFTY);
  }

  static TropicalSemiring one() {
    return TropicalSemiring(0);
  }

  std::string string() const {
    if (val == INFTY) {
      return "inf";
    }
    return std::to_string(val);
  }

  bool isInf() const {
    return val == INFTY;
  }

  int getValue() const { return val; }
};

} // namespace fpsolve

#endif // FPSOLVE_TROPICAL_SEMIRING_H

