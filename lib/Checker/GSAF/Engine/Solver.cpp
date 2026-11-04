#include "Checker/GSAF/Engine/Solver.h"

#include "Utils/LLVM/PackedTypeLayout.h"

#include <limits>
#include <sstream>
#include <stdexcept>

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace lotus::gsaf::detail {

uint64_t encodingTypeSize(const DataLayout &layout, Type *type) {
  uint64_t width = PackedTypeLayout(layout).getTypeSizeInBits(type);
  if (!width)
    throw std::runtime_error("GSAF requires a fixed nonzero value width");
  return width;
}

std::string encodingSymbol(const gvfg::GuardedValueFlowNode *node) {
  if (auto *value = dyn_cast_or_null<GlobalValue>(node->getLLVMValue())) {
    if (value->hasName())
      return "global_" + value->getName().str();
    std::ostringstream name;
    name << "global_" << value;
    return name.str();
  }
  std::ostringstream name;
  name << "node_" << node;
  return name.str();
}

SMTExpr encodeScalarConstant(SMTFactory &factory, const Constant *constant,
                             uint64_t width) {
  if (auto *integer = dyn_cast<ConstantInt>(constant))
    return factory.createBitVecVal(integer->getValue().getRawData()[0], width);
  if (auto *floating = dyn_cast<ConstantFP>(constant)) {
    const APFloat &value = floating->getValueAPF();
    if (value.isInfinity() || value.isNaN()) {
      int64_t bound = value.isInfinity() && value.isNegative()
                          ? std::numeric_limits<int64_t>::min()
                          : std::numeric_limits<int64_t>::max();
      return factory.createBitVecVal(std::to_string(bound), width);
    }
    SmallString<64> decimal;
    value.toString(decimal, UINT32_MAX, UINT32_MAX);
    StringRef whole = StringRef(decimal).split('.').first;
    return factory.createBitVecVal(whole.str(), width);
  }
  assert((isa<ConstantPointerNull>(constant) ||
          isa<ConstantAggregateZero>(constant)) &&
         "Unexpected scalar constant kind");
  return factory.createBitVecVal(0, width);
}

} // namespace lotus::gsaf::detail
