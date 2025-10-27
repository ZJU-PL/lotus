#pragma once

#ifdef HAVE_CLAM

#include "clam/Clam.hh"
#include "clam/ClamAnalysisParams.hh"
#include "clam/CfgBuilderParams.hh"
#include "clam/CrabDomain.hh"
#include "clam/HeapAbstraction.hh"
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/ADT/Optional.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <memory>

namespace lotus {
namespace clam {

/**
 * Wrapper to integrate CLAM abstract interpretation framework into Lotus
 * 
 * CLAM provides numerical abstract domains and invariant generation
 * that can enhance precision of various analyses in Lotus, including:
 * - Null pointer dereference detection
 * - Buffer overflow detection
 * - Use-after-free detection
 */
class ClamAnalysisWrapper {
public:
  using clam_analysis_t = ::clam::IntraGlobalClam;
  using abs_dom_map_t = ::clam::ClamGlobalAnalysis::abs_dom_map_t;
  using checks_db_t = ::clam::ClamGlobalAnalysis::checks_db_t;
  
  /**
   * Constructor
   * @param M LLVM module to analyze
   * @param man CrabBuilderManager that manages CFG builders
   */
  ClamAnalysisWrapper(llvm::Module &M, ::clam::CrabBuilderManager &man);
  
  /**
   * Run CLAM analysis on the module
   * @param params Analysis parameters (domain choice, widening strategy, etc.)
   * @param assumptions Optional assumptions map
   */
  void analyze(::clam::AnalysisParams &params = getDefaultParams(),
               const abs_dom_map_t &assumptions = abs_dom_map_t());
  
  /**
   * Get invariants at entry of a basic block
   * @param BB Basic block
   * @param keep_shadows Whether to keep shadow variables
   * @return Abstract domain representing invariants, or none if not available
   */
  llvm::Optional<::clam::clam_abstract_domain> 
  getPre(const llvm::BasicBlock *BB, bool keep_shadows = false) const;
  
  /**
   * Get invariants at exit of a basic block
   * @param BB Basic block
   * @param keep_shadows Whether to keep shadow variables
   * @return Abstract domain representing invariants, or none if not available
   */
  llvm::Optional<::clam::clam_abstract_domain> 
  getPost(const llvm::BasicBlock *BB, bool keep_shadows = false) const;
  
  /**
   * Check if a pointer is definitely null at a program point
   * @param BB Basic block
   * @param V Pointer value to check
   * @return true if definitely null, false otherwise
   */
  bool isDefinitelyNull(const llvm::BasicBlock *BB, const llvm::Value *V) const;
  
  /**
   * Check if a pointer is definitely non-null at a program point
   * @param BB Basic block
   * @param V Pointer value to check
   * @return true if definitely non-null, false otherwise
   */
  bool isDefinitelyNotNull(const llvm::BasicBlock *BB, const llvm::Value *V) const;
  
  /**
   * Get the checks database
   * @return Database of all property checks
   */
  const checks_db_t &getChecksDB() const;
  
  /**
   * Check if analysis has been performed
   */
  bool hasAnalyzed() const { return m_analyzed; }
  
  /**
   * Print statistics about the analysis
   */
  void printStats(llvm::raw_ostream &OS) const;

  /**
   * Get default analysis parameters
   */
  static ::clam::AnalysisParams &getDefaultParams();

private:
  std::unique_ptr<clam_analysis_t> m_clam;
  llvm::Module &m_module;
  bool m_analyzed;
};

/**
 * Factory function to create a CrabBuilderManager with default heap abstraction
 * @param params Builder parameters
 * @param tli Target library info wrapper pass
 * @return Unique pointer to CrabBuilderManager
 */
std::unique_ptr<::clam::CrabBuilderManager>
createCrabBuilderManager(const ::clam::CrabBuilderParams &params,
                         llvm::TargetLibraryInfoWrapperPass &tli);

/**
 * Factory function to create a CLAM analysis wrapper with default configuration
 * @param M Module to analyze
 * @param man CrabBuilderManager
 * @return Unique pointer to ClamAnalysisWrapper
 */
std::unique_ptr<ClamAnalysisWrapper> 
createClamAnalysis(llvm::Module &M, ::clam::CrabBuilderManager &man);

} // namespace clam
} // namespace lotus

#endif // HAVE_CLAM
