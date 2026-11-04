#pragma once

#include "Checker/GSAF/API/Models.h"
#include "IR/GVFG/GuardedValueFlowSolver.h"

namespace lotus::gsaf {

namespace detail {
uint64_t encodingTypeSize(const llvm::DataLayout &layout, llvm::Type *type);
std::string encodingSymbol(const gvfg::GuardedValueFlowNode *node);
SMTExpr encodeScalarConstant(SMTFactory &factory,
                             const llvm::Constant *constant, uint64_t width);
} // namespace detail

/// Preserves the source engine's scalar abstraction while reusing GVFG's
/// dependency encoding, guards, caches, and incremental SMT implementation.
template <typename SolverBase> class GSAFSolverEncoding : public SolverBase {
  GSAFModels *Models = nullptr;
  bool HeapFailure = false;
  bool FileFailure = false;

protected:
  uint64_t getEncodingTypeSize(llvm::Type *type) const override {
    return detail::encodingTypeSize(this->DL, type);
  }
  std::string
  getEncodingSymbol(const gvfg::GuardedValueFlowNode *node) const override {
    return detail::encodingSymbol(node);
  }
  SMTExpr encodeScalarConstant(const llvm::Constant *constant,
                               uint64_t width) override {
    return detail::encodeScalarConstant(*this->Factory, constant, width);
  }
  std::pair<uint64_t, uint64_t> getEncodingCastWidths(
      const gvfg::GuardedValueFlowOpcodeNode *node) const override {
    return {getEncodingTypeSize(node->getOperand(0)->getType()),
            getEncodingTypeSize(node->getType())};
  }
  bool trackCallOutput(
      const gvfg::GuardedValueFlowCallOutputNode *node) const override {
    return node->getKind() !=
           gvfg::GuardedValueFlowNode::Kind::CallSitePseudoInput;
  }
  bool
  isNonNullTerminal(const gvfg::GuardedValueFlowNode *node) const override {
    if (SolverBase::isNonNullTerminal(node))
      return true;
    auto *value = node->getLLVMValue();
    return value &&
           ((!HeapFailure && Models && Models->isHeapAllocSite(value)) ||
            (!FileFailure && Models && Models->isFileOpenSite(value)));
  }

public:
  using SolverBase::SolverBase;
  void setModels(GSAFModels *models, bool heap_failure, bool file_failure) {
    Models = models;
    HeapFailure = heap_failure;
    FileFailure = file_failure;
  }
};

using GSAFSolver = GSAFSolverEncoding<gvfg::GuardedValueFlowSolver>;
using DTGSAFSolver = GSAFSolverEncoding<gvfg::DTGuardedValueFlowSolver>;

} // namespace lotus::gsaf
