/**
 * Boolean Semiring for FPSolve
 */

#ifndef FPSOLVE_BOOL_SEMIRING_H
#define FPSOLVE_BOOL_SEMIRING_H

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include <string>
#include <memory>

namespace fpsolve {

class BoolSemiring : public StarableSemiring<BoolSemiring, 
                                              Commutativity::Commutative, 
                                              Idempotence::Idempotent> {
private:
  bool val;
  static std::shared_ptr<BoolSemiring> elem_null;
  static std::shared_ptr<BoolSemiring> elem_one;

public:
  BoolSemiring();
  BoolSemiring(bool val);
  BoolSemiring(std::string str_val);
  virtual ~BoolSemiring();

  BoolSemiring operator+=(const BoolSemiring& elem);
  BoolSemiring operator*=(const BoolSemiring& elem);
  bool operator==(const BoolSemiring& elem) const;
  BoolSemiring star() const;

  static BoolSemiring null();
  static BoolSemiring one();
  std::string string() const;

  static constexpr bool is_idempotent = true;
  static constexpr bool is_commutative = true;
};

} // namespace fpsolve

#endif // FPSOLVE_BOOL_SEMIRING_H

