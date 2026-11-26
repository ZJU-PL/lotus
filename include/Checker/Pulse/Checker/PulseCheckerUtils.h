/** @file PulseCheckerUtils.h @brief Utility functions for the Pulse checker framework. */
#pragma once

#include "Checker/Pulse/Core/PulseFormula.h"

#include <cstdint>
#include <string>

#include <llvm/IR/Instructions.h>

namespace llvm {
class Value;
} // namespace llvm

namespace pulse {
namespace detail {

/** True if v is a null pointer constant (null, or 0 cast to pointer). */
bool isNullPointerConstantValue(const llvm::Value *v);

/** Invert an ICmp predicate (e.g. EQ -> NE). */
llvm::ICmpInst::Predicate invertIcmpPred(llvm::ICmpInst::Predicate p);

/** Add path condition for integer comparison; returns false if UNSAT. */
bool applyIntegerIcmpConstraint(PulseFormula &formula,
                                llvm::ICmpInst::Predicate p, AbstractValue lhs,
                                AbstractValue rhs);

/** Add path condition for floating-point comparison; returns false if UNSAT. */
bool applyFloatCmpConstraint(PulseFormula &formula,
                             llvm::FCmpInst::Predicate p, AbstractValue lhs,
                             AbstractValue rhs, bool assumed_true);

/** Normalize an LLVM RTTI typeinfo operand (e.g. @_ZTI11MyException) to a
 *  canonical type name ("MyException"). Returns "" for null/catch-all. */
std::string normalizeRttiTypeName(const llvm::Value *tinfo);

/** Deterministic positive selector ID matching @llvm.eh.typeid.for. */
int64_t rttiTypeIdForName(const std::string &type_name);

} // namespace detail
} // namespace pulse

