#pragma once

#include <stdexcept>

#include <llvm/IR/Instructions.h>
#include <z3++.h>

namespace symabs_ai {
// Shared scalar semantics: clients supply their own SSA/time-step mapping.
// Overflow/poison policy belongs to the client, not to these value operators.
inline z3::expr integerBinary(unsigned opcode, const z3::expr &a,
                              const z3::expr &b) {
  switch (opcode) {
  case llvm::Instruction::Add:
    return a + b;
  case llvm::Instruction::Sub:
    return a - b;
  case llvm::Instruction::Mul:
    return a * b;
  case llvm::Instruction::And:
    return a & b;
  case llvm::Instruction::Or:
    return a | b;
  case llvm::Instruction::Xor:
    return a ^ b;
  default:
    throw std::invalid_argument("unsupported integer binary operation");
  }
}

inline z3::expr integerCompare(llvm::CmpInst::Predicate predicate,
                               const z3::expr &a, const z3::expr &b) {
  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
    return a == b;
  case llvm::CmpInst::ICMP_NE:
    return a != b;
  case llvm::CmpInst::ICMP_UGT:
    return z3::ugt(a, b);
  case llvm::CmpInst::ICMP_UGE:
    return z3::uge(a, b);
  case llvm::CmpInst::ICMP_ULT:
    return z3::ult(a, b);
  case llvm::CmpInst::ICMP_ULE:
    return z3::ule(a, b);
  case llvm::CmpInst::ICMP_SGT:
    return a > b;
  case llvm::CmpInst::ICMP_SGE:
    return a >= b;
  case llvm::CmpInst::ICMP_SLT:
    return a < b;
  case llvm::CmpInst::ICMP_SLE:
    return a <= b;
  default:
    throw std::invalid_argument("unsupported integer comparison");
  }
}
} // namespace symabs_ai
