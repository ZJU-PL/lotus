/** @file ScalarEvolutionReferencer.h @brief SCEV value-reference expander used by induction variable analysis. */
/*
 * Copyright 2026 Lotus contributors
 */
#pragma once

#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/IRBuilder.h"

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
namespace analysis {
namespace loop {
namespace detail {

class SCEVReference {
public:
  SCEVReference(llvm::Value *value, const llvm::SCEV *scev);

  llvm::Value *getValue(void) const;
  const llvm::SCEV *getSCEV(void) const;
  llvm::iterator_range<std::vector<SCEVReference *>::iterator>
  getChildReferences(void);
  SCEVReference *getChildReference(int32_t idx);
  int32_t getNumChildReferences(void) const;

  void addChildReference(std::unique_ptr<SCEVReference> child);
  std::set<SCEVReference *> collectAllReferences(void);

private:
  llvm::Value *value;
  const llvm::SCEV *scev;
  std::vector<std::unique_ptr<SCEVReference>> ownedChildren;
  std::vector<SCEVReference *> childReferences;
};

class SCEVValueMapper {
public:
  SCEVValueMapper(llvm::ScalarEvolution &SE, llvm::Function &F);

  llvm::Value *getSingleValueOf(const llvm::SCEV *scev) const;
  std::set<llvm::Value *> getValuesOf(const llvm::SCEV *scev) const;
  const llvm::SCEV *getSCEVOf(llvm::Value *value) const;

private:
  std::unordered_map<const llvm::SCEV *, std::set<llvm::Value *>> scevToValues;
  std::unordered_map<llvm::Value *, const llvm::SCEV *> valueToSCEV;
};

class ScalarEvolutionReferentialExpander {
public:
  ScalarEvolutionReferentialExpander(llvm::ScalarEvolution &SE,
                                     llvm::Function &F);
  ~ScalarEvolutionReferentialExpander();

  std::unique_ptr<SCEVReference> createReferenceTree(
      const llvm::SCEV *scev,
      const std::set<llvm::Value *> &valuesInScope);

  llvm::Value *expandUsingReferenceValues(
      SCEVReference *tree,
      const std::set<llvm::Value *> &valuesToReferenceAndNotExpand,
      llvm::IRBuilder<> &expansionBuilder);

private:
  std::unique_ptr<SCEVValueMapper> scevValueMapper;
};

} // namespace detail
} // namespace loop
} // namespace analysis
} // namespace lotus
