#pragma once

namespace llvm {
class Value;
} // namespace llvm

#include "Checker/Framework/ValueTrace.h"

#include <utility>

// LLVM Value Trace is a sequence of LLVM value pairs,
// the first value is usually the operand and the second value is usually the
// operation/use-site of the operand LLVM Value is used to transform different
// type of traces without the knowledge of the details of the traces
//
// for example, SVFGTrace -> LLVMValueTrace -> GVFGTrace
// SVFGTrace -> LLVMValueTrace is maintained by SSU framework writer
// LLVMValueTrace -> GVFGTrace is maintained by GVFG framework writer
// By this means, we can ensure that the private details of different modules
// shall not be exposed
//
// Details of LLVM Value Trace:
//
// ----Step
// * Each step in LLVM Value Trace is a value pair, the first field is the
// operand and the second is the use-site
// * If the step is only single-value step with value Val, please put the pair
// <Val, Val> in the trace
//
// ----Value Flow
// * If value-flow is not cared, please just list the value pairs, null is
// allowed in the value pair fields
// * For callee to caller value flow, please put the <return value, return site>
// pair into the trace, followed by a <Function, callsite> pair, and the callee
// to caller value flow shall be valid
// * For caller to callee value flow, please put the <RealArg, CallSite> pair
// into the trace, followed by a <FormalArg, Function> pair, and the caller to
// callee value flow shall be valid
// * For value flows that b uses the value a, please just put the pair <a, b> in
// the trace
//
// ----Phi Gated
// * For phi gated function, please put the chosen value in the first field and
// the phi instruction in the second field, and the phi gated function shall be
// valid in LLVM Value Trace
//
// ----Path choices
// * To specify a path choice, just put the value-pair <BB, BrInst> in the
// trace, the BrInst shall switch to the BB
namespace lotus {
namespace trace {
using LLVMValueTrace = Trace<std::pair<llvm::Value *, llvm::Value *>>;

} // namespace trace
} // namespace lotus
