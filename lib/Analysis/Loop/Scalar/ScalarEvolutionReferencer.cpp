/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Scalar/ScalarEvolutionReferencer.h"

namespace lotus {
namespace analysis {
namespace loop {
namespace detail {

namespace {

class ReferenceTreeExpander
    : public llvm::SCEVVisitor<ReferenceTreeExpander, llvm::Value *> {
public:
  ReferenceTreeExpander(
      SCEVReference *tree,
      const std::set<llvm::Value *> &valuesToReferenceAndNotExpand,
      llvm::IRBuilder<> &expansionBuilder)
      : currentNode{tree},
        valuesToReferenceAndNotExpand{valuesToReferenceAndNotExpand},
        expansionBuilder{expansionBuilder} {
    this->rootValue = visit(tree->getSCEV());
    this->currentNode = tree;
  }

  llvm::Value *getRootOfTree(void) const { return this->rootValue; }

private:
  friend struct llvm::SCEVVisitor<ReferenceTreeExpander, llvm::Value *>;

  llvm::Value *visitConstant(const llvm::SCEVConstant *) {
    return this->currentNode->getValue();
  }
  llvm::Value *visitTruncateExpr(const llvm::SCEVTruncateExpr *) {
    return nullptr;
  }
  llvm::Value *visitZeroExtendExpr(const llvm::SCEVZeroExtendExpr *) {
    return nullptr;
  }
  llvm::Value *visitSignExtendExpr(const llvm::SCEVSignExtendExpr *) {
    return nullptr;
  }
  llvm::Value *visitUDivExpr(const llvm::SCEVUDivExpr *) { return nullptr; }
  llvm::Value *visitAddRecExpr(const llvm::SCEVAddRecExpr *) { return nullptr; }
  llvm::Value *visitUMaxExpr(const llvm::SCEVUMaxExpr *) { return nullptr; }
  llvm::Value *visitSMinExpr(const llvm::SCEVSMinExpr *) { return nullptr; }
  llvm::Value *visitUMinExpr(const llvm::SCEVUMinExpr *) { return nullptr; }
  llvm::Value *visitSequentialUMinExpr(const llvm::SCEVSequentialUMinExpr *) {
    return nullptr;
  }
  llvm::Value *visitPtrToIntExpr(const llvm::SCEVPtrToIntExpr *) {
    return nullptr;
  }
  llvm::Value *visitCouldNotCompute(const llvm::SCEVCouldNotCompute *) {
    return nullptr;
  }

  std::pair<llvm::Value *, llvm::Value *> visitTwoOperands(
      const llvm::SCEVNAryExpr *S) {
    if (this->currentNode->getNumChildReferences() < 2) {
      return std::make_pair(nullptr, nullptr);
    }

    auto *holder = this->currentNode;
    this->currentNode = holder->getChildReference(0);
    auto *LHS = visit(S->getOperand(0));
    this->currentNode = holder->getChildReference(1);
    auto *RHS = visit(S->getOperand(1));
    this->currentNode = holder;

    if (LHS == nullptr || RHS == nullptr) {
      return std::make_pair(nullptr, nullptr);
    }
    return std::make_pair(LHS, RHS);
  }

  llvm::Value *visitAddExpr(const llvm::SCEVAddExpr *S) {
    if (this->valuesToReferenceAndNotExpand.count(
            this->currentNode->getValue()) != 0) {
      return this->currentNode->getValue();
    }
    if (S->getNumOperands() != 2 || !S->getOperand(0)->getType()->isIntegerTy() ||
        !S->getOperand(1)->getType()->isIntegerTy()) {
      return nullptr;
    }
    auto operands = visitTwoOperands(S);
    if (operands.first == nullptr) {
      return nullptr;
    }
    return this->expansionBuilder.CreateAdd(operands.first, operands.second);
  }

  llvm::Value *visitMulExpr(const llvm::SCEVMulExpr *S) {
    if (this->valuesToReferenceAndNotExpand.count(
            this->currentNode->getValue()) != 0) {
      return this->currentNode->getValue();
    }
    if (S->getNumOperands() != 2 || !S->getOperand(0)->getType()->isIntegerTy() ||
        !S->getOperand(1)->getType()->isIntegerTy()) {
      return nullptr;
    }
    auto operands = visitTwoOperands(S);
    if (operands.first == nullptr) {
      return nullptr;
    }
    return this->expansionBuilder.CreateMul(operands.first, operands.second);
  }

  llvm::Value *visitSMaxExpr(const llvm::SCEVSMaxExpr *S) {
    if (this->valuesToReferenceAndNotExpand.count(
            this->currentNode->getValue()) != 0) {
      return this->currentNode->getValue();
    }
    if (S->getNumOperands() != 2) {
      return nullptr;
    }
    auto operands = visitTwoOperands(S);
    if (operands.first == nullptr) {
      return nullptr;
    }
    return this->expansionBuilder.CreateMaximum(operands.first, operands.second);
  }

  llvm::Value *visitUnknown(const llvm::SCEVUnknown *) {
    auto *value = this->currentNode->getValue();
    return this->valuesToReferenceAndNotExpand.count(value) != 0 ? value
                                                                 : nullptr;
  }

  llvm::Value *rootValue{nullptr};
  SCEVReference *currentNode;
  const std::set<llvm::Value *> &valuesToReferenceAndNotExpand;
  llvm::IRBuilder<> &expansionBuilder;
};

class ReferenceTreeBuilder
    : public llvm::SCEVVisitor<ReferenceTreeBuilder,
                               std::unique_ptr<SCEVReference>> {
public:
  ReferenceTreeBuilder(const llvm::SCEV *scev,
                       SCEVValueMapper &scevValueMapper,
                       const std::set<llvm::Value *> &valuesInScope)
      : valuesInScope{valuesInScope}, scevValueMapper{scevValueMapper} {
    this->tree = visit(scev);
  }

  std::unique_ptr<SCEVReference> takeTree(void) { return std::move(this->tree); }

private:
  friend struct llvm::SCEVVisitor<ReferenceTreeBuilder,
                                    std::unique_ptr<SCEVReference>>;

  llvm::Value *mapToSingleInScopeValue(const llvm::SCEV *S) {
    auto values = this->scevValueMapper.getValuesOf(S);
    llvm::Value *single = nullptr;
    for (auto *V : values) {
      if (this->valuesInScope.count(V) == 0) {
        continue;
      }
      if (single != nullptr) {
        return nullptr;
      }
      single = V;
    }
    return single;
  }

  std::unique_ptr<SCEVReference> createReferenceOfSingleInScopeValue(
      const llvm::SCEV *S) {
    auto *singleValue = mapToSingleInScopeValue(S);
    return singleValue != nullptr ? std::make_unique<SCEVReference>(singleValue, S)
                                  : nullptr;
  }

  std::unique_ptr<SCEVReference> createReferenceOfNArySCEV(
      const llvm::SCEVNAryExpr *S) {
    auto reference =
        std::make_unique<SCEVReference>(mapToSingleInScopeValue(S), S);
    for (auto *operand : S->operands()) {
      auto operandReference = visit(operand);
      if (operandReference == nullptr) {
        break;
      }
      reference->addChildReference(std::move(operandReference));
    }

    if (static_cast<size_t>(reference->getNumChildReferences()) !=
        S->getNumOperands()) {
      if (reference->getValue() == nullptr) {
        return nullptr;
      }
    }
    return reference;
  }

  std::unique_ptr<SCEVReference> visitConstant(const llvm::SCEVConstant *S) {
    return std::make_unique<SCEVReference>(S->getValue(), S);
  }
  std::unique_ptr<SCEVReference> visitUnknown(const llvm::SCEVUnknown *S) {
    auto *value = S->getValue();
    return this->valuesInScope.count(value) == 0
               ? nullptr
               : std::make_unique<SCEVReference>(value, S);
  }
  std::unique_ptr<SCEVReference> visitTruncateExpr(
      const llvm::SCEVTruncateExpr *S) {
    return createReferenceOfSingleInScopeValue(S);
  }
  std::unique_ptr<SCEVReference> visitZeroExtendExpr(
      const llvm::SCEVZeroExtendExpr *S) {
    return createReferenceOfSingleInScopeValue(S);
  }
  std::unique_ptr<SCEVReference> visitSignExtendExpr(
      const llvm::SCEVSignExtendExpr *S) {
    return createReferenceOfSingleInScopeValue(S);
  }
  std::unique_ptr<SCEVReference> visitAddExpr(const llvm::SCEVAddExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitMulExpr(const llvm::SCEVMulExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitUDivExpr(const llvm::SCEVUDivExpr *S) {
    auto LHS = visit(S->getLHS());
    auto RHS = visit(S->getRHS());
    auto *selfValue = mapToSingleInScopeValue(S);
    auto reference = std::make_unique<SCEVReference>(selfValue, S);
    if (LHS != nullptr && RHS != nullptr) {
      reference->addChildReference(std::move(LHS));
      reference->addChildReference(std::move(RHS));
      return reference;
    }
    if (selfValue == nullptr) {
      return nullptr;
    }
    return reference;
  }
  std::unique_ptr<SCEVReference> visitPtrToIntExpr(
      const llvm::SCEVPtrToIntExpr *S) {
    return createReferenceOfSingleInScopeValue(S);
  }
  std::unique_ptr<SCEVReference> visitAddRecExpr(
      const llvm::SCEVAddRecExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitSMaxExpr(const llvm::SCEVSMaxExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitUMaxExpr(const llvm::SCEVUMaxExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitSMinExpr(const llvm::SCEVSMinExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitUMinExpr(const llvm::SCEVUMinExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitSequentialUMinExpr(
      const llvm::SCEVSequentialUMinExpr *S) {
    return createReferenceOfNArySCEV(S);
  }
  std::unique_ptr<SCEVReference> visitCouldNotCompute(
      const llvm::SCEVCouldNotCompute *) {
    return nullptr;
  }

  std::unique_ptr<SCEVReference> tree;
  const std::set<llvm::Value *> &valuesInScope;
  SCEVValueMapper &scevValueMapper;
};

} // namespace

ScalarEvolutionReferentialExpander::ScalarEvolutionReferentialExpander(
    llvm::ScalarEvolution &SE,
    llvm::Function &F)
    : scevValueMapper{std::make_unique<SCEVValueMapper>(SE, F)} {}

ScalarEvolutionReferentialExpander::~ScalarEvolutionReferentialExpander() =
    default;

std::unique_ptr<SCEVReference>
ScalarEvolutionReferentialExpander::createReferenceTree(
    const llvm::SCEV *scev,
    const std::set<llvm::Value *> &valuesInScope) {
  ReferenceTreeBuilder builder(scev, *this->scevValueMapper, valuesInScope);
  return builder.takeTree();
}

llvm::Value *ScalarEvolutionReferentialExpander::expandUsingReferenceValues(
    SCEVReference *tree,
    const std::set<llvm::Value *> &valuesToReferenceAndNotExpand,
    llvm::IRBuilder<> &expansionBuilder) {
  ReferenceTreeExpander expander(tree, valuesToReferenceAndNotExpand,
                                 expansionBuilder);
  return expander.getRootOfTree();
}

SCEVValueMapper::SCEVValueMapper(llvm::ScalarEvolution &SE, llvm::Function &F) {
  for (auto &A : F.args()) {
    if (!SE.isSCEVable(A.getType())) {
      continue;
    }
    const auto *scev = SE.getSCEV(&A);
    this->scevToValues[scev].insert(&A);
    this->valueToSCEV[&A] = scev;
  }

  for (auto &B : F) {
    for (auto &I : B) {
      if (!SE.isSCEVable(I.getType())) {
        continue;
      }
      const auto *scev = SE.getSCEV(&I);
      this->scevToValues[scev].insert(&I);
      this->valueToSCEV[&I] = scev;
    }
  }
}

llvm::Value *SCEVValueMapper::getSingleValueOf(const llvm::SCEV *scev) const {
  auto values = getValuesOf(scev);
  return values.size() == 1 ? *values.begin() : nullptr;
}

std::set<llvm::Value *> SCEVValueMapper::getValuesOf(
    const llvm::SCEV *scev) const {
  auto it = this->scevToValues.find(scev);
  return it != this->scevToValues.end() ? it->second : std::set<llvm::Value *>{};
}

const llvm::SCEV *SCEVValueMapper::getSCEVOf(llvm::Value *value) const {
  auto it = this->valueToSCEV.find(value);
  return it != this->valueToSCEV.end() ? it->second : nullptr;
}

SCEVReference::SCEVReference(llvm::Value *value, const llvm::SCEV *scev)
    : value{value}, scev{scev} {}

llvm::Value *SCEVReference::getValue(void) const { return this->value; }

const llvm::SCEV *SCEVReference::getSCEV(void) const { return this->scev; }

llvm::iterator_range<std::vector<SCEVReference *>::iterator>
SCEVReference::getChildReferences(void) {
  return llvm::make_range(this->childReferences.begin(),
                          this->childReferences.end());
}

SCEVReference *SCEVReference::getChildReference(int32_t idx) {
  return this->childReferences.at(static_cast<size_t>(idx));
}

int32_t SCEVReference::getNumChildReferences(void) const {
  return static_cast<int32_t>(this->childReferences.size());
}

void SCEVReference::addChildReference(std::unique_ptr<SCEVReference> child) {
  auto *raw = child.get();
  this->ownedChildren.push_back(std::move(child));
  this->childReferences.push_back(raw);
}

std::set<SCEVReference *> SCEVReference::collectAllReferences(void) {
  std::set<SCEVReference *> references;
  references.insert(this);
  for (auto *child : this->childReferences) {
    auto children = child->collectAllReferences();
    references.insert(children.begin(), children.end());
  }
  return references;
}

} // namespace detail
} // namespace loop
} // namespace analysis
} // namespace lotus
