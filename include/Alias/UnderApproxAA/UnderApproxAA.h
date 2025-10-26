/**
 * @file UnderApproxAA.h
 * @brief Under-approximation alias analysis using syntactic pattern matching
 *
 * This file provides a simple under-approximation alias analysis that uses
 * syntactic pattern matching to identify definite alias relationships.
 * It only reports MustAlias when certain clear patterns are detected,
 * otherwise returns NoAlias (under-approximation).
 *
 * This analysis is useful when:
 * - A lightweight, fast alias analysis is needed
 * - Only definite aliases are required (precision over recall)
 * - More sophisticated analyses are unavailable or too expensive
 */

#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace UnderApprox
{

/**
 * @class UnderApproxAA
 * @brief Simple under-approximation alias analysis
 *
 * This class implements a conservative alias analysis that uses syntactic
 * pattern matching to identify definite (MustAlias) relationships.
 * It recognizes patterns such as:
 * - Same value (after stripping casts)
 * - Bitcast relationships
 * - Load/store to the same address
 *
 * The analysis is an under-approximation: it only reports MustAlias when
 * certain, otherwise returns NoAlias. It never reports MayAlias.
 */
class UnderApproxAA
{
public:
  /**
   * @brief Construct an under-approximation alias analysis
   * @param M The LLVM module to analyze
   */
  UnderApproxAA(llvm::Module &M);

  /**
   * @brief Destructor
   */
  ~UnderApproxAA();

  /**
   * @brief Query alias relationship between two values
   * @param v1 First value (must be pointer type)
   * @param v2 Second value (must be pointer type)
   * @return AliasResult indicating the alias relationship
   *         (either MustAlias or NoAlias, never MayAlias)
   */
  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query alias relationship with memory locations
   * @param loc1 First memory location
   * @param loc2 Second memory location
   * @return AliasResult indicating the alias relationship
   */
  llvm::AliasResult alias(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2);

  /**
   * @brief Check if two values must alias
   * @param v1 First value
   * @param v2 Second value
   * @return true if v1 and v2 must alias, false otherwise
   */
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Get the module being analyzed
   * @return Reference to the module
   */
  llvm::Module &getModule() { return _module; }

private:
  /// The module being analyzed
  llvm::Module &_module;

  /**
   * @brief Helper to validate pointer types
   */
  bool isValidPointerQuery(const llvm::Value *v1, const llvm::Value *v2) const;
};

} // namespace UnderApprox

