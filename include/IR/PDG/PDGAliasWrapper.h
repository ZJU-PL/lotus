/**
 * @file PDGAliasWrapper.h
 * @brief Unified wrapper for alias analysis used in PDG construction
 *
 * This file provides a unified interface for querying alias information from
 * various alias analysis implementations (Andersen, DyckAA, LLVM's built-in AA, etc.).
 * It abstracts away the differences between different alias analysis backends and
 * provides a consistent API for PDG construction.
 *
 * Supported backends:
 * - Andersen's alias analysis (lib/Alias/Andersen)
 * - Dyck alias analysis (lib/Alias/DyckAA)
 * - LLVM's built-in alias analysis
 *
 * Usage:
 *   PDGAliasWrapper wrapper(module, AAType::Andersen);
 *   AliasResult result = wrapper.query(v1, v2);
 */

#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <memory>

// Forward declarations for alias analysis classes
class AndersenAAResult;
class DyckAliasAnalysis;

namespace pdg
{

/**
 * @enum AAType
 * @brief Enumeration of supported alias analysis types
 */
enum class AAType
{
  Andersen,      ///< Andersen's alias analysis (flow-insensitive, context-insensitive)
  DyckAA,        ///< Dyck alias analysis (CFL-reachability based)
  BasicAA,       ///< LLVM's BasicAA (local reasoning about GEPs, PHI nodes, etc.)
  TBAA,          ///< LLVM's Type-Based AA (uses type metadata)
  GlobalsAA,     ///< LLVM's Globals-Modref AA (tracks global variable modifications)
  SCEVAA,        ///< LLVM's ScalarEvolution-based AA
  Combined,      ///< Use multiple analyses (conservative intersection)
  UnderApprox    ///< Simple under-approximation (syntactic pattern matching)
};

/**
 * @class PDGAliasWrapper
 * @brief Unified interface for alias analysis queries
 *
 * This class provides a consistent API for querying alias information from
 * different alias analysis implementations. It handles the initialization
 * and management of the underlying analysis and provides convenient query methods.
 */
class PDGAliasWrapper
{
public:
  /**
   * @brief Construct an alias wrapper with specified analysis type
   * @param M The LLVM module to analyze
   * @param type The type of alias analysis to use
   */
  PDGAliasWrapper(llvm::Module &M, AAType type = AAType::Andersen);

  /**
   * @brief Destructor
   */
  ~PDGAliasWrapper();

  /**
   * @brief Query alias relationship between two values
   * @param v1 First value (must be pointer type)
   * @param v2 Second value (must be pointer type)
   * @return AliasResult indicating the alias relationship
   */
  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query alias relationship with memory locations
   * @param loc1 First memory location
   * @param loc2 Second memory location
   * @return AliasResult indicating the alias relationship
   */
  llvm::AliasResult query(const llvm::MemoryLocation &loc1, 
                          const llvm::MemoryLocation &loc2);

  /**
   * @brief Check if two values may alias
   * @param v1 First value
   * @param v2 Second value
   * @return true if v1 and v2 may alias, false otherwise
   */
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Check if two values must alias
   * @param v1 First value
   * @param v2 Second value
   * @return true if v1 and v2 must alias, false otherwise
   */
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Check if a value may be null
   * @param v The value to check
   * @return true if v may be null, false otherwise
   */
  bool mayNull(const llvm::Value *v);

  /**
   * @brief Get the points-to set for a pointer value
   * @param ptr The pointer value
   * @param ptsSet Output vector to store points-to set
   * @return true if points-to set is available, false otherwise
   */
  bool getPointsToSet(const llvm::Value *ptr, 
                      std::vector<const llvm::Value *> &ptsSet);

  /**
   * @brief Get the alias set for a value
   * @param v The value
   * @param aliasSet Output vector to store alias set
   * @return true if alias set is available, false otherwise
   */
  bool getAliasSet(const llvm::Value *v, 
                   std::vector<const llvm::Value *> &aliasSet);

  /**
   * @brief Get the type of alias analysis being used
   * @return AAType enum value
   */
  AAType getType() const { return _aa_type; }

  /**
   * @brief Check if the wrapper is initialized and ready to use
   * @return true if initialized, false otherwise
   */
  bool isInitialized() const { return _initialized; }

private:
  /// Type of alias analysis being used
  AAType _aa_type;
  
  /// The module being analyzed
  llvm::Module *_module;
  
  /// Whether the wrapper is properly initialized
  bool _initialized;

  /// Andersen AA result (if using Andersen)
  std::unique_ptr<AndersenAAResult> _andersen_aa;

  /// Dyck AA result (if using DyckAA)
  DyckAliasAnalysis *_dyck_aa;

  /// LLVM AA result (if using LLVM)
  llvm::AAResults *_llvm_aa;

  /**
   * @brief Initialize the selected alias analysis
   */
  void initialize();

  /**
   * @brief Query using Andersen's analysis
   */
  llvm::AliasResult queryAndersen(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query using Dyck analysis
   */
  llvm::AliasResult queryDyck(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query using LLVM's built-in analysis
   */
  llvm::AliasResult queryLLVM(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query using simple under-approximation (syntactic pattern matching)
   */
  llvm::AliasResult queryUnderApproximate(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Helper to validate pointer types
   */
  bool isValidPointerQuery(const llvm::Value *v1, const llvm::Value *v2) const;
};

/**
 * @class PDGAliasFactory
 * @brief Factory class for creating PDGAliasWrapper instances
 *
 * This class provides factory methods to create alias wrappers with
 * specific configurations. It can also auto-select the best available
 * alias analysis based on the module characteristics.
 */
class PDGAliasFactory
{
public:
  /**
   * @brief Create an alias wrapper with the specified type
   * @param M The module to analyze
   * @param type The type of alias analysis
   * @return A unique pointer to the created wrapper
   */
  static std::unique_ptr<PDGAliasWrapper> create(llvm::Module &M, AAType type);

  /**
   * @brief Create an alias wrapper with auto-selected analysis
   * @param M The module to analyze
   * @return A unique pointer to the created wrapper
   */
  static std::unique_ptr<PDGAliasWrapper> createAuto(llvm::Module &M);

  /**
   * @brief Get a human-readable name for an AAType
   * @param type The alias analysis type
   * @return String representation of the type
   */
  static const char *getTypeName(AAType type);
};

} // namespace pdg

