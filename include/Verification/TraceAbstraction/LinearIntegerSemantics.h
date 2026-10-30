#pragma once

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <z3++.h>

namespace lotus::verification::traceabstraction::lia {
// Machine-width integer values encoded entirely in Int/LIA. There are no SMT
// bit-vectors. Modular arithmetic keeps unflagged LLVM overflow sound.
inline z3::expr numeral(z3::context &ctx, const llvm::APInt &value, bool sign) {
  llvm::SmallString<80> digits;
  value.toString(digits, 10, sign);
  return ctx.int_val(digits.c_str());
}
inline z3::expr power2(z3::context &ctx, unsigned width) {
  return numeral(ctx, llvm::APInt::getOneBitSet(width + 1, width), false);
}
inline z3::expr unsignedValue(const z3::expr &v, unsigned width) {
  return z3::mod(v, power2(v.ctx(), width));
}
inline z3::expr signedValue(const z3::expr &v, unsigned width) {
  auto half = power2(v.ctx(), width - 1);
  return z3::mod(v + half, power2(v.ctx(), width)) - half;
}
inline z3::expr normalize(const z3::expr &v, unsigned width) {
  if (width == 1)
    return unsignedValue(v, 1);
  auto half = power2(v.ctx(), width - 1);
  return z3::mod(v + half, power2(v.ctx(), width)) - half;
}
inline z3::expr range(const z3::expr &v, unsigned width) {
  auto low = width == 1 ? v.ctx().int_val(0) : -power2(v.ctx(), width - 1);
  auto high = power2(v.ctx(), width == 1 ? 1 : width - 1);
  return v >= low && v < high;
}
} // namespace lotus::verification::traceabstraction::lia
