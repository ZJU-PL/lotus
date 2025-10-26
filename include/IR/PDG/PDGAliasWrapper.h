/**
 * @file PDGAliasWrapper.h
 * @brief Backward-compatible wrapper for alias analysis used in PDG construction
 *
 * This file provides a backward-compatible interface for PDG construction that
 * delegates to the unified AliasAnalysisWrapper in lib/Alias/.
 *
 * NOTE: This is a compatibility layer. New code should use AliasAnalysisWrapper directly.
 *
 * Usage:
 *   PDGAliasWrapper wrapper(module, pdg::AAType::Andersen);
 *   AliasResult result = wrapper.query(v1, v2);
 */

#pragma once

#include "Alias/AliasAnalysisWrapper.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <memory>

namespace pdg
{

// Re-export AAType from lotus namespace for backward compatibility
using AAType = lotus::AAType;

/**
 * @class PDGAliasWrapper
 * @brief Backward-compatible wrapper that delegates to AliasAnalysisWrapper
 *
 * This class provides backward compatibility for PDG construction. It simply
 * delegates all calls to the unified AliasAnalysisWrapper.
 *
 * @deprecated Use lotus::AliasAnalysisWrapper directly instead.
 */
class PDGAliasWrapper
{
public:
  /**
   * @brief Construct an alias wrapper with specified analysis type
   * @param M The LLVM module to analyze
   * @param type The type of alias analysis to use
   */
  PDGAliasWrapper(llvm::Module &M, AAType type = AAType::Andersen)
    : _wrapper(std::make_unique<lotus::AliasAnalysisWrapper>(M, type)) {}

  /**
   * @brief Destructor
   */
  ~PDGAliasWrapper() = default;

  /**
   * @brief Query alias relationship between two values
   */
  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2) {
    return _wrapper->query(v1, v2);
  }

  /**
   * @brief Query alias relationship with memory locations
   */
  llvm::AliasResult query(const llvm::MemoryLocation &loc1, 
                          const llvm::MemoryLocation &loc2) {
    return _wrapper->query(loc1, loc2);
  }

  /**
   * @brief Check if two values may alias
   */
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2) {
    return _wrapper->mayAlias(v1, v2);
  }

  /**
   * @brief Check if two values must alias
   */
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2) {
    return _wrapper->mustAlias(v1, v2);
  }

  /**
   * @brief Check if a value may be null
   */
  bool mayNull(const llvm::Value *v) {
    return _wrapper->mayNull(v);
  }

  /**
   * @brief Get the points-to set for a pointer value
   */
  bool getPointsToSet(const llvm::Value *ptr, 
                      std::vector<const llvm::Value *> &ptsSet) {
    return _wrapper->getPointsToSet(ptr, ptsSet);
  }

  /**
   * @brief Get the alias set for a value
   */
  bool getAliasSet(const llvm::Value *v, 
                   std::vector<const llvm::Value *> &aliasSet) {
    return _wrapper->getAliasSet(v, aliasSet);
  }

  /**
   * @brief Get the type of alias analysis being used
   */
  AAType getType() const { return _wrapper->getType(); }

  /**
   * @brief Check if the wrapper is initialized and ready to use
   */
  bool isInitialized() const { return _wrapper->isInitialized(); }

private:
  /// The underlying alias analysis wrapper
  std::unique_ptr<lotus::AliasAnalysisWrapper> _wrapper;
};

/**
 * @class PDGAliasFactory
 * @brief Factory class for creating PDGAliasWrapper instances
 *
 * @deprecated Use lotus::AliasAnalysisFactory directly instead.
 */
class PDGAliasFactory
{
public:
  /**
   * @brief Create an alias wrapper with the specified type
   */
  static std::unique_ptr<PDGAliasWrapper> create(llvm::Module &M, AAType type) {
    return std::make_unique<PDGAliasWrapper>(M, type);
  }

  /**
   * @brief Create an alias wrapper with auto-selected analysis
   */
  static std::unique_ptr<PDGAliasWrapper> createAuto(llvm::Module &M) {
    return std::make_unique<PDGAliasWrapper>(M, AAType::Andersen);
  }

  /**
   * @brief Get a human-readable name for an AAType
   */
  static const char *getTypeName(AAType type) {
    return lotus::AliasAnalysisFactory::getTypeName(type);
  }
};

} // namespace pdg

